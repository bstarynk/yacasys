/** file yacasys/src/yaca.h

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
#ifndef _YACA_H_INCLUDED_
#define _YACA_H_INCLUDED_

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <fastcgi.h>
#include <unistd.h>
#include <time.h>
#include <syslog.h>
#include <getopt.h>
#include <string.h>
#include <jansson.h>
#include <pthread.h>
#include <signal.h>
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <sys/mman.h>
#include <sys/time.h>

#define YACA_MAX_WORKERS 16
#define YACA_MAX_TYPENUM 4096

#define YACA_FATAL(Fmt,...) do {				\
    fprintf(stderr, "YACA FATAL [%s:%d] " Fmt "\n",		\
	    __FILE__, __LINE__,					\
	    ##__VA_ARGS__);					\
    fflush (NULL);						\
    pthread_mutex_lock (&yaca_syslog_mutex);			\
    syslog(LOG_CRIT,"[%s:%d] FATAL: " Fmt, __FILE__, __LINE__,	\
	   ##__VA_ARGS__);					\
    abort(); }while(0)

#define YACA_LIKELY(C) !__builtin_expect(0,!(C))
#define YACA_UNLIKELY(C) __builtin_expect(0,(C))

#define YACA_SYSLOG(Lev,Fmt,...) do {			\
  pthread_mutex_lock (&yaca_syslog_mutex);		\
  syslog(Lev,Fmt, ##__VA_ARGS__);			\
  pthread_mutex_unlock (&yaca_syslog_mutex);} while(0)

extern time_t yaca_start_time;
extern char yaca_hostname[];
extern char yaca_build_timestamp[];
extern const char *yaca_progname;
extern const char *yaca_users_base;
extern const char *yaca_data_dir;
extern const char *yaca_source_dir;
extern const char *yaca_object_dir;
extern unsigned yaca_nb_workers;
extern pthread_mutex_t yaca_syslog_mutex;
extern volatile sig_atomic_t yaca_interrupt;

// pseudo-random number generator, like random(3), lrand48(3),
// drand48(3) but with a mutex
int32_t yaca_random (void);
long yaca_lrand48 (void);
double yaca_drand48 (void);

// give a prime number above some given threshold, or 0 if not found
unsigned long yaca_prime_after (unsigned long l);

typedef uint32_t yaca_id_t;
typedef uint16_t yaca_typenum_t;
typedef uint16_t yaca_mark_t;
typedef uint16_t yaca_spacenum_t;
struct yaca_item_st;
struct yaca_itemtype_st;
struct yaca_dumper_st;
struct yaca_tupleitems_st;
struct yaca_worker_st;		/* in agenda.c */
extern __thread struct yaca_worker_st *yaca_this_worker;

typedef struct yaca_item_st *yaca_loaditem_sig_t (json_t *, yaca_id_t);
typedef void yaca_fillitem_sig_t (json_t *, struct yaca_item_st *);
typedef json_t *yaca_dumpitem_sig_t (struct yaca_item_st *);
typedef json_t *yaca_dumpcontent_sig_t (struct yaca_item_st *);
typedef void yaca_runitem_sig_t (struct yaca_item_st *);
typedef void yaca_scandump_sig_t (struct yaca_item_st *,
				  struct yaca_dumper_st *);


// make a new item
struct yaca_item_st *yaca_item_make (yaca_typenum_t typnum,
				     yaca_spacenum_t spacenum,
				     unsigned extrasize);
// build an item of given id, useful at load time
struct yaca_item_st *yaca_item_build (yaca_typenum_t typnum,
				      yaca_spacenum_t spacenum,
				      unsigned extrasize, yaca_id_t id);
// get the item of a given id
struct yaca_item_st *yaca_item_of_id (yaca_id_t id);


typedef struct yaca_item_st *yaca_loaditem_sig_t (json_t *, yaca_id_t);
typedef void yaca_fillitem_sig_t (json_t *, struct yaca_item_st *);
typedef json_t *yaca_dumpitem_sig_t (struct yaca_item_st *);
typedef json_t *yaca_dumpcontent_sig_t (struct yaca_item_st *);
typedef void yaca_runitem_sig_t (struct yaca_item_st *);
typedef void yaca_scandump_sig_t (struct yaca_item_st *,
				  struct yaca_dumper_st *);

///// items

#define YACA_ITEM_MAGIC 971394241	/*0x39e64cc1 */
struct yaca_item_st		// in calloc-ed memory, and explicitly free-d by the GC
{
  uint32_t itm_magic;		/* always YACA_ITEM_MAGIC */
  yaca_id_t itm_id;
  yaca_typenum_t itm_typnum;
  yaca_spacenum_t itm_spacnum;
  pthread_mutex_t itm_mutex;
  long itm_dataspace[];
};
#define YACA_ITEM_MAX_SIZE (256*1024*sizeof(void*))

// make a new item
struct yaca_item_st *yaca_item_make (yaca_typenum_t typnum,
				     yaca_spacenum_t spacenum,
				     unsigned extrasize);
// build an item of given id, useful at load time
struct yaca_item_st *yaca_item_build (yaca_typenum_t typnum,
				      yaca_spacenum_t spacenum,
				      unsigned extrasize, yaca_id_t id);
// get the item of a given id
struct yaca_item_st *yaca_item_of_id (yaca_id_t id);

// touch an item (write barrier for the GC) --forwarded definition
static inline void yaca_item_touch (struct yaca_item_st *itm);


struct yaca_tupleitems_st
{
  unsigned tup_len;
  struct yaca_item_st *tup_items[];
};

///// types
#define YACA_TYPE_MAGIC 657176525	/* 0x272bb7cd */
struct yaca_itemtype_st
{
  uint32_t typ_magic;		/* always YACA_TYPE_MAGIC */
  yaca_typenum_t typ_num;
  const char *typ_name;
  struct ya_item_t *typ_item;
  yaca_loaditem_sig_t *typr_loaditem;
  yaca_fillitem_sig_t *typr_fillitem;
  yaca_scandump_sig_t *typr_scandump;
  yaca_dumpitem_sig_t *typr_dumpitem;
  yaca_dumpcontent_sig_t *typr_dumpcontent;
  yaca_runitem_sig_t *typr_runitem;
  void *typ_spare_[10];
};
#define YACA_ITEM_MAX_TYPE 4096
extern struct yaca_itemtype_st *yaca_typetab[];


///// spaces
#define YACA_SPACE_MAGIC 327731843	/* 0x1388ca83 */
struct yaca_space_st
{
  uint32_t spa_magic;
  yaca_spacenum_t spa_num;
  const char *spa_name;
  struct yaca_spacedata_st *spa_data;
};
#define YACA_MAX_SPACE 1024
extern struct yaca_space_st *yaca_spacetab[];

// memory region
#define YACA_SMALLREGION_MAGIC 1379233909	/* 0x52357075 */
#define YACA_BIGREGION_MAGIC 1260589607	/* 0x4b231227 */

// region sizes are large power of two, and are aligned to their size */
#define SMALLREGION_LOG 20
#define BIGREGION_LOG 24
#define YACA_SMALLREGION_SIZE (1<<SMALLREGION_LOG)	/* 1Megabyte */
#define YACA_BIGREGION_SIZE (8<<BIGREGION_LOG)	/*16Megabytes */

#define YACA_MINALIGNMENT __BIGGEST_ALIGNMENT__	/* 16 bytes on x86-64/Linux */
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
  struct yaca_region_st *reg_next;
  void *reg_end;
  long long reg_data[] __attribute__ ((aligned (YACA_MINALIGNMENT)));
};
#define YACA_REGION_EMPTY ((struct yaca_region_st*)-1L)

// quickly allocate in a region -without any locking- or NULL if full
static inline void *
yaca_allocate_in_region (struct yaca_region_st *reg, unsigned siz)
{
  if (!reg || !siz)
    return NULL;
  assert (reg->reg_magic == YACA_SMALLREGION_MAGIC
	  || reg->reg_magic == YACA_BIGREGION_MAGIC);
  if (YACA_UNLIKELY (siz % YACA_MINALIGNMENT != 0))
    siz = (siz | (YACA_MINALIGNMENT - 1)) + 1;
  void *p = reg->reg_free;
  if (YACA_LIKELY ((char *) p + siz < (char *) reg->reg_end))
    {
      reg->reg_free = (char *) p + siz;
      return p;
    }
  return NULL;
}




////////////////////////////////////////////////////////////////
/// agenda

enum yacaspecworker_en
{
  yacaworker__none,
  yacaworker_gc,
  yacaworker_fcgi,
  yacaworker__last
};


enum yaca_agenda_state_en
{
  yacag_stop = 0,
  yacag_run,
};

enum yaca_worker_state_en
{
  yawrk__none = 0,
  yawrk_idle,
  yawrk_run,
  yawrk_start_gc,
  yawrk__last = 0
};


enum yaca_interrupt_reason_en
{
  yaint__none = 0,
  yaint_gc,
  yaint__last
};

#define YACA_WORKER_TOUCH_CACHE_LEN 17	/* a small prime number */
#define YACA_WORKER_MAGIC 471856441	/*0x1c1ff539 */
struct yaca_worker_st
{
  uint32_t worker_magic;
  int16_t worker_num;
  uint16_t worker_state;
  pthread_t worker_thread;
  timer_t worker_timer;
  uint32_t worker_need;
  struct yaca_region_st *worker_region;
  volatile sig_atomic_t worker_interrupted;
  struct yaca_item_st *worker_touchcache[YACA_WORKER_TOUCH_CACHE_LEN];
};

#define YACA_WORKER_SIGNAL SIGALRM
#define YACA_WORKER_TICKMILLISEC 25	/* milliseconds */
void yaca_load (void);

void yaca_start_agenda (void);
void yaca_interrupt_agenda (enum yaca_interrupt_reason_en reason);

enum yaca_taskprio_en
{
  tkprio__none,
  tkprio_low,
  tkprio_normal,
  tkprio_high,
  tkprio__last
};


// return false if failed to add or move
bool yaca_agenda_add_back (struct yaca_item_st *itmtask,
			   enum yaca_taskprio_en prio);
// return false if failed to add or move
bool yaca_agenda_add_front (struct yaca_item_st *itmtask,
			    enum yaca_taskprio_en prio);

// remove a task item, return tkprio__none if failed to remove else
// its old priority
enum yaca_taskprio_en yaca_agenda_remove (struct yaca_item_st *itmtask);

// query the priority of an item, or tkprio__none if not found
enum yaca_taskprio_en yaca_agenda_task_prio (struct yaca_item_st *itmtask);

// stop the agenda
void yaca_agenda_stop (void);


// initialize memory management & garbage collection
void yaca_initialize_memgc (void);

// wait till all workers reached the current state
void yaca_wait_workers_all_at_state (unsigned state);

// allocate from a worker (preferably), and ask for GC when needed
void *yaca_work_allocate (unsigned siz);

// signal that a garbage collection is needed
void yaca_should_garbage_collect (void);


// the work routine of the gc thread; argument is the struct
// yaca_worker_st of the GC thread
void *yaca_gcthread_work (void *);

// this is called by worker threads when GC is needed
void yaca_worker_garbcoll (void);


static inline void
yaca_item_touch (struct yaca_item_st *itm)
{
  extern void yaca_item_really_touch (struct yaca_item_st *);
  if (YACA_UNLIKELY (itm == NULL))
    return;
  assert (itm->itm_magic == YACA_ITEM_MAGIC);
  if (yaca_this_worker)
    {
      yaca_id_t id = itm->itm_id;
      if (yaca_this_worker->
	  worker_touchcache[id % YACA_WORKER_TOUCH_CACHE_LEN] == itm
	  || yaca_this_worker->worker_touchcache[(id + 1) %
						 YACA_WORKER_TOUCH_CACHE_LEN]
	  == itm)
	return;
    }
  yaca_item_really_touch (itm);
}

#endif /* _YACA_H_INCLUDED_ */
/* eof yacasys/yaca.h */
