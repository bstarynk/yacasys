/** file yacasys/src/garbcoll.c

     Copyright (C) 2013 Basile Starynkevitch <basile@starynkevitch.net>

     This file is part of YacaSys

      YacaSys is free software; you can redistribute it and/or modify
      it under the terms of the GNU General Public License as published by
      the Free Software Foundation; either version 3, or (at your option)
      any later version.

      YacaSys is distributed in the hope that it will be useful,
      but WITHOUT ANY WARRANTY; without even the implied warranty of
      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
      GNU General Public License for more details.

      You should have received a copy of the GNU General Public License
      along with YacaSys; see the file COPYING3.   If not see
      <http://www.gnu.org/licenses/>.

**/

#include "yaca.h"

/** The garbage collector deals with memory regions. It is invoked only
   when the agenda is not running (and when every worker thread is in
   GC state). Memory regions have a fixed size (power of two, megabyte[s]).
   Data in memory region may be copied.
**/

static pthread_mutex_t yaca_memory_mutex = PTHREAD_MUTEX_INITIALIZER;

#define YACA_SMALLREGION_MAGIC 1379233909	/* 0x52357075 */
#define YACA_BIGREGION_MAGIC 1260589607	/* 0x4b231227 */

// region sizes are large power of two, and are aligned to their size */
#define SMALLREGION_LOG 20
#define BIGREGION_LOG 24
#define YACA_SMALLREGION_SIZE (1<<SMALLREGION_LOG)	/* 1Megabyte */
#define YACA_BIGREGION_SIZE (8<<BIGREGION_LOG)	/*16Megabytes */

// hashtable for small & big regions
static struct
{
  unsigned count;
  unsigned size;		/* a prime number */
  struct yaca_region_st **arr;
} smallregion, bigregion;

struct yaca_region_st
{
  unsigned reg_magic;		/* YACA_SMALLREGION_MAGIC or
				   YACA_BIGREGION_MAGIC */
  unsigned reg_index;		/* index in smallregion or bigregion */
  uint16_t reg_state;
  uint64_t reg_spare1;
  uint64_t reg_spare2;
  uint64_t reg_spare3;
  void *reg_free;
  void *reg_end;
  long long reg_data[];
};
#define YACA_REGION_EMPTY ((struct yaca_region_st*)-1L)

static void
add_smallregion (struct yaca_region_st *reg)
{
  unsigned count = smallregion.count;
  unsigned size = smallregion.size;
  struct yaca_region_st **arr = smallregion.arr;
  assert (count + 5 < size && arr != NULL);
  assert (reg != NULL && reg->reg_magic == YACA_SMALLREGION_MAGIC);
  assert (((uintptr_t) reg % YACA_SMALLREGION_SIZE) == 0);
  int pos = -1;
  uintptr_t h = ((uintptr_t) reg / YACA_SMALLREGION_SIZE) % size;
  for (unsigned ix = h; ix < size; ix++)
    {
      struct yaca_region_st *cureg = arr[ix];
      if (cureg == reg)
	return;
      else if (cureg == NULL)
	{
	  if (pos < 0)
	    pos = ix;
	  arr[pos] = reg;
	  smallregion.count = count + 1;
	  reg->reg_index = pos;
	  return;
	}
      else if (cureg == YACA_REGION_EMPTY)
	{
	  if (pos < 0)
	    pos = ix;
	  continue;
	}
    }
  for (unsigned ix = 0; ix < h; ix++)
    {
      struct yaca_region_st *cureg = arr[ix];
      if (cureg == reg)
	return;
      else if (cureg == NULL)
	{
	  if (pos < 0)
	    pos = ix;
	  arr[pos] = reg;
	  reg->reg_index = pos;
	  smallregion.count = count + 1;
	  return;
	}
      else if (cureg == YACA_REGION_EMPTY)
	{
	  if (pos < 0)
	    pos = ix;
	  continue;
	}
    }
  if (YACA_UNLIKELY (pos < 0))
    // should not happen
    YACA_FATAL ("small region array full & corrupted");
  arr[pos] = reg;
  reg->reg_index = pos;
  smallregion.count = count + 1;
}

static void
reorganize_smallregion (unsigned gap)
{
  unsigned count = smallregion.count;
  unsigned oldsize = smallregion.size;
  struct yaca_region_st **oldarr = smallregion.arr;
  unsigned long newsize = yaca_prime_after (3 * count / 2 + 40 + gap);
  if (newsize >= INT_MAX)
    YACA_FATAL ("too big small region size %ld", (long) newsize);
  smallregion.count = 0;
  smallregion.arr = calloc (newsize, sizeof (struct yaca_region_st *));
  if (!smallregion.arr)
    YACA_FATAL ("failed to allocate small region array of %d", (int) newsize);
  smallregion.size = newsize;
  for (unsigned ix = 0; ix < oldsize; ix++)
    {
      struct yaca_region_st *cureg = oldarr[ix];
      if (!cureg || cureg == YACA_REGION_EMPTY)
	continue;
      add_smallregion (cureg);
    }
  free (oldarr);
}

static void
add_bigregion (struct yaca_region_st *reg)
{
  unsigned count = bigregion.count;
  unsigned size = bigregion.size;
  struct yaca_region_st **arr = bigregion.arr;
  assert (count + 5 < size && arr != NULL);
  assert (reg != NULL && reg->reg_magic == YACA_BIGREGION_MAGIC);
  assert (((uintptr_t) reg % YACA_BIGREGION_SIZE) == 0);
  int pos = -1;
  uintptr_t h = ((uintptr_t) reg / YACA_BIGREGION_SIZE) % size;
  for (unsigned ix = h; ix < size; ix++)
    {
      struct yaca_region_st *cureg = arr[ix];
      if (cureg == reg)
	return;
      else if (cureg == NULL)
	{
	  if (pos < 0)
	    pos = ix;
	  arr[pos] = reg;
	  reg->reg_index = pos;
	  bigregion.count = count + 1;
	  return;
	}
      else if (cureg == YACA_REGION_EMPTY)
	{
	  if (pos < 0)
	    pos = ix;
	  continue;
	}
    }
  for (unsigned ix = 0; ix < h; ix++)
    {
      struct yaca_region_st *cureg = arr[ix];
      if (cureg == reg)
	return;
      else if (cureg == NULL)
	{
	  if (pos < 0)
	    pos = ix;
	  arr[pos] = reg;
	  reg->reg_index = pos;
	  bigregion.count = count + 1;
	  return;
	}
      else if (cureg == YACA_REGION_EMPTY)
	{
	  if (pos < 0)
	    pos = ix;
	  continue;
	}
    }
  if (YACA_UNLIKELY (pos < 0))
    // should not happen
    YACA_FATAL ("big region array full & corrupted");
  arr[pos] = reg;
  reg->reg_index = pos;
  bigregion.count = count + 1;
}



static void
reorganize_bigregion (unsigned gap)
{
  unsigned count = bigregion.count;
  unsigned oldsize = bigregion.size;
  struct yaca_region_st **oldarr = bigregion.arr;
  unsigned long newsize = yaca_prime_after (3 * count / 2 + 40 + gap);
  if (newsize >= INT_MAX)
    YACA_FATAL ("too big big region size %ld", (long) newsize);
  bigregion.count = 0;
  bigregion.arr = calloc (newsize, sizeof (struct yaca_region_st *));
  if (!bigregion.arr)
    YACA_FATAL ("failed to allocate big region array of %d", (int) newsize);
  bigregion.size = newsize;
  for (unsigned ix = 0; ix < oldsize; ix++)
    {
      struct yaca_region_st *cureg = oldarr[ix];
      if (!cureg || cureg == YACA_REGION_EMPTY)
	continue;
      add_bigregion (cureg);
    }
  free (oldarr);
}



struct yaca_region_st *
yaca_new_smallregion (void)
{
  struct yaca_region_st *reg = NULL;
  pthread_mutex_lock (&yaca_memory_mutex);
  if (YACA_UNLIKELY(4*smallregion.count + 50 > 3*smallregion.size))
    reorganize_smallregion (smallregion.count/8+20);
  // allocate the region
  {
    void *ad = mmap (NULL, 2 * YACA_SMALLREGION_SIZE,
		     PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
		     -1,
		     (off_t) 0);
    if (ad == MAP_FAILED)
      YACA_FATAL ("failed to mmap small region - %m");
    if ((uintptr_t) ad % YACA_SMALLREGION_SIZE == 0)
      {
	munmap ((char *) ad + YACA_SMALLREGION_SIZE, YACA_SMALLREGION_SIZE);
	reg = (struct yaca_region_st *) ad;
      }
    else
      {
	uintptr_t begreg = ((uintptr_t) ad | (YACA_SMALLREGION_SIZE - 1)) + 1;
	uintptr_t endreg = begreg + YACA_SMALLREGION_SIZE;
	munmap ((char *) ad, begreg - (uintptr_t) ad);
	munmap ((char *) endreg,
		(uintptr_t) (ad + 2 * YACA_SMALLREGION_SIZE) - endreg);
	reg = (struct yaca_region_st *) begreg;
      }
  }
  // register the region
  reg->reg_magic = YACA_SMALLREGION_MAGIC;
  add_smallregion (reg);
  // initialize the region
  reg->reg_state = 0;
  reg->reg_free = reg->reg_data;
  reg->reg_end = (char *) reg + YACA_SMALLREGION_SIZE;
  if (!reg->reg_end)
    YACA_FATAL("unlucky small region ending at NIL");
  goto end;
end:
  pthread_mutex_unlock (&yaca_memory_mutex);
  return reg;
}


struct yaca_region_st *
yaca_new_bigregion (void)
{
  struct yaca_region_st *reg = NULL;
  pthread_mutex_lock (&yaca_memory_mutex);
  if (YACA_UNLIKELY(4*bigregion.count + 50 > 3*bigregion.size))
    reorganize_bigregion (bigregion.count/8+20);
  // allocate the region
  {
    void *ad = mmap (NULL, 2 * YACA_BIGREGION_SIZE,
		     PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
		     -1,
		     (off_t) 0);
    if (ad == MAP_FAILED)
      YACA_FATAL ("failed to mmap big region - %m");
    if ((uintptr_t) ad % YACA_BIGREGION_SIZE == 0)
      {
	munmap ((char *) ad + YACA_BIGREGION_SIZE, YACA_BIGREGION_SIZE);
	reg = (struct yaca_region_st *) ad;
      }
    else
      {
	uintptr_t begreg = ((uintptr_t) ad | (YACA_BIGREGION_SIZE - 1)) + 1;
	uintptr_t endreg = begreg + YACA_BIGREGION_SIZE;
	munmap ((char *) ad, begreg - (uintptr_t) ad);
	munmap ((char *) endreg,
		(uintptr_t) (ad + 2 * YACA_BIGREGION_SIZE) - endreg);
	reg = (struct yaca_region_st *) begreg;
      }
  }
  // register the region
  reg->reg_magic = YACA_BIGREGION_MAGIC;
  add_bigregion (reg);
  // initialize the region
  reg->reg_state = 0;
  reg->reg_free = reg->reg_data;
  reg->reg_end = (char *) reg + YACA_BIGREGION_SIZE;
  if (!reg->reg_end)
    YACA_FATAL("unlucky big region ending at NIL");
  goto end;
end:
  pthread_mutex_unlock (&yaca_memory_mutex);
  return reg;
}

void
yaca_delete_region (struct yaca_region_st *reg)
{
  if (!reg || reg==YACA_REGION_EMPTY) return;
  pthread_mutex_lock (&yaca_memory_mutex);
  if (reg->reg_magic == YACA_SMALLREGION_MAGIC) 
    {
      assert ((uintptr_t)reg % YACA_SMALLREGION_SIZE == 0);
      assert (smallregion.arr != NULL);
      assert (reg->reg_index < smallregion.size);
      assert (smallregion.arr[reg->reg_index]==reg);
      smallregion.arr[reg->reg_index] = YACA_REGION_EMPTY;
      smallregion.count--;
      if (YACA_UNLIKELY(smallregion.count < smallregion.size/4 && smallregion.size>230))
	reorganize_smallregion (smallregion.count/8+20);
      if (munmap ((char*)reg, YACA_SMALLREGION_SIZE))
	YACA_FATAL("failed to unmap small region@%p - %m", (void*)reg);
    }
  else if (reg->reg_magic == YACA_BIGREGION_MAGIC)
    {
      assert ((uintptr_t)reg % YACA_BIGREGION_SIZE == 0);
      assert (bigregion.arr != NULL);
      assert (reg->reg_index < bigregion.size);
      assert (bigregion.arr[reg->reg_index]==reg);
      bigregion.arr[reg->reg_index] = YACA_REGION_EMPTY;
      bigregion.count--;
      if (YACA_UNLIKELY(bigregion.count < bigregion.size/4 && bigregion.size>120))
	reorganize_bigregion (bigregion.count/8+20);
      if (munmap ((char*)reg, YACA_BIGREGION_SIZE))
	YACA_FATAL("failed to unmap big region@%p - %m", (void*)reg);
    }
  goto end;
end:
  pthread_mutex_unlock (&yaca_memory_mutex);
}


struct yaca_region_st *
yaca_find_region (void *ptr)
{
  struct yaca_region_st *reg = NULL;
  if (!ptr || (uintptr_t)ptr<YACA_SMALLREGION_SIZE 
      || (uintptr_t)ptr > UINTPTR_MAX-YACA_SMALLREGION_SIZE)
    return NULL;
  pthread_mutex_lock (&yaca_memory_mutex);
  // is it inside a small region?
  {
    void* alptr = (void*) ((uintptr_t)ptr & ~(YACA_SMALLREGION_SIZE-1));
    unsigned size = smallregion.size;
    struct yaca_region_st** arr = smallregion.arr;
    assert (size > 2);
    assert (arr != NULL);
    uintptr_t h = ((uintptr_t) ptr / YACA_SMALLREGION_SIZE) % size;
  for (unsigned ix = h; ix < size; ix++)
    {
      struct yaca_region_st *cureg = arr[ix];
      if ((void*)cureg == alptr)
	{ 
	  reg = cureg;
	  break;
	}
      else if (cureg == NULL)
	break;
    };
  for (unsigned ix = 0; ix<h; ix++)
    {
      struct yaca_region_st *cureg = arr[ix];
      if ((void*)cureg == alptr)
	{
	  reg = cureg;
	  break;
	}
      else if (cureg == NULL)
	break;
    };
  }
  // is it inside a big region?
  if (!reg) {
    void* alptr = (void*) ((uintptr_t)ptr & ~(YACA_BIGREGION_SIZE-1));
    unsigned size = bigregion.size;
    struct yaca_region_st** arr = bigregion.arr;
    assert (size > 2);
    assert (arr != NULL);
    uintptr_t h = ((uintptr_t) ptr / YACA_BIGREGION_SIZE) % size;
  for (unsigned ix = h; ix < size; ix++)
    {
      struct yaca_region_st *cureg = arr[ix];
      if ((void*)cureg == alptr)
	{reg = cureg;
	  break;}
      else if (cureg == NULL)
	goto end;
    };
  for (unsigned ix = 0; ix<h; ix++)
    {
      struct yaca_region_st *cureg = arr[ix];
      if ((void*)cureg == alptr)
	{reg = cureg;
	  break;}
      else if (cureg == NULL)
	goto end;
    };
  }
  goto end;
end:
  pthread_mutex_unlock (&yaca_memory_mutex);
  return reg;
}


void
yaca_initialize_memgc (void)
{
  {
    unsigned siz = yaca_prime_after (220);
    memset (&smallregion, 0, sizeof (smallregion));
    smallregion.arr = calloc (siz, sizeof (struct yaca_region_st *));
    if (!smallregion.arr)
      YACA_FATAL ("failed to allocate small region array (%d) - %m",
		  (int) siz);
    smallregion.size = siz;
  }
  {
    unsigned siz = yaca_prime_after (60);
    memset (&bigregion, 0, sizeof (bigregion));
    bigregion.arr = calloc (siz, sizeof (struct yaca_region_st *));
    if (!bigregion.arr)
      YACA_FATAL ("failed to allocate big region array (%d) - %m", (int) siz);
    bigregion.size = siz;
  }
}
