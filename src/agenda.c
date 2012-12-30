/** file yacasys/src/agenda.c

     Copyright (C) 2012 Basile Starynkevitch <basile@starynkevitch.net>

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


/* We have a small number of worker threads; each worker thread is
   "interruptible" (either by a timer, every few milliseconds, or by
   other workers or threads. We depend upon the fact that a SIGALRM
   signal, produced by a timer, is sent to its thread. */

#define YACA_WORKER_MAGIC 471856441	/*0x1c1ff539 */
struct yaca_worker_st
{
  uint32_t worker_magic;
  int16_t worker_num;
  uint16_t worker_state;
  pthread_t worker_thread;
  timer_t worker_timer;
  volatile sig_atomic_t worker_interrupted;
};

#define YACA_WORKER_SIGNAL SIGALRM
#define YACA_WORKER_TICKMILLISEC 2	/* milliseconds */
static __thread struct yaca_worker_st *yaca_this_worker;

struct yaca_worker_st yaca_worktab[YACA_MAX_WORKERS + 1];




enum yacaspecworker_en
{
  yacaworker__none,
  yacaworker_gc,
  yacaworker_fcgi,
  yacaworker__last
};


#define YACA_AGENDAELEM_MAGIC 1162937659	/* 0x4551053b */
/* the element length should be not too big, and not too small */
#define YACA_AGENDAELEM_LENGTH 6
struct yaca_agendaelem_st
{
  uint32_t agel_magic;		/* always YACA_AGENDAELEM_MAGIC */
  uint16_t agel_prio;		/* actually enum yaca_taskprio_en */
  struct yaca_agendaelem_st *agel_prev;
  struct yaca_agendaelem_st *agel_next;
  struct yaca_item_st *agel_elemtab[YACA_AGENDAELEM_LENGTH];
};

static pthread_mutex_t yaca_agenda_mutex = PTHREAD_MUTEX_INITIALIZER;
#define YACA_MAX_AGENDA 1000000
#define  YACA_AGENDAELEM_EMPTY ((struct yaca_agendaelem_st*)(-1L))
static struct
{
  // the queue for each priority has its head, its tail, its count
  struct yaca_agendaelem_st *headtab[tkprio__last];
  struct yaca_agendaelem_st *tailtab[tkprio__last];
  unsigned countab[tkprio__last];
  // the total count
  unsigned totalcount;
  // the prime size of the hash table (associating items to agenda elements)
  unsigned hashsize;
  struct yaca_agendaelem_st **hasharr;	/* array of hashsize elements */
} agenda;

static struct yaca_worker_st yaca_gcworker;
static struct yaca_worker_st yaca_fcgiworker;


static void *yaca_worker_work (void *);
static void *yaca_gc_work (void *);

static void yaca_work_alarm_sigaction (int sig, siginfo_t * sinf, void *data);

void
yaca_start_agenda (void)
{
  // setup the signal handler, valid inside each thread
  {
    struct sigaction alact;
    memset (&alact, 0, sizeof (alact));
    alact.sa_sigaction = yaca_work_alarm_sigaction;
    alact.sa_flags = SA_SIGINFO;
    sigaction (YACA_WORKER_SIGNAL, &alact, NULL);
  }
  pthread_mutex_lock (&yaca_agenda_mutex);
  memset (&agenda, 0, sizeof (agenda));
  agenda.hashsize = yaca_prime_after (YACA_MAX_AGENDA / 1000);
  assert (agenda.hashsize > 0);
  agenda.hasharr =
    calloc (agenda.hashsize, sizeof (struct yaca_agendaelem_st *));
  if (!agenda.hasharr)
    YACA_FATAL ("failed to allocat agenda hash table of %d elements",
		agenda.hashsize);
  // start the workers
  assert (yaca_nb_workers >= 2 && yaca_nb_workers <= YACA_MAX_WORKERS);
  for (unsigned ix = 1; ix <= yaca_nb_workers; ix++)
    {
      struct yaca_worker_st *tsk = yaca_worktab + ix;
      assert (!tsk->worker_thread);
      tsk->worker_num = ix;
      tsk->worker_magic = YACA_WORKER_MAGIC;
      pthread_create (&tsk->worker_thread, NULL, yaca_worker_work, tsk);
    }
  // start the gc worker
  {
    struct yaca_worker_st *tsk = &yaca_gcworker;
    assert (!tsk->worker_thread);
    tsk->worker_num = -(int) yacaworker_gc;
    tsk->worker_magic = YACA_WORKER_MAGIC;
    pthread_create (&tsk->worker_thread, NULL, yaca_gc_work, tsk);
  }
  goto end;
end:
  pthread_mutex_unlock (&yaca_agenda_mutex);
}

void
yaca_interrupt_agenda (void)
{
  pthread_mutex_lock (&yaca_agenda_mutex);
  yaca_interrupt = 1;
  for (unsigned ix = 1; ix <= yaca_nb_workers; ix++)
    {
      struct yaca_worker_st *tsk = yaca_worktab + ix;
      assert (tsk->worker_magic == YACA_WORKER_MAGIC);
      tsk->worker_interrupted = 1;
      pthread_kill (tsk->worker_thread, YACA_WORKER_SIGNAL);
    }
  goto end;
end:
  pthread_mutex_unlock (&yaca_agenda_mutex);
}

void
yaca_stop_agenda (void)
{
  yaca_interrupt = 1;
  pthread_mutex_lock (&yaca_agenda_mutex);
  goto end;
end:
  pthread_mutex_unlock (&yaca_agenda_mutex);
}

static void
yaca_work_alarm_sigaction (int sig, siginfo_t * sinf, void *data)
{
  yaca_interrupt = 1;
  assert (sig == YACA_WORKER_SIGNAL);
  if (!yaca_this_worker)
    return;
  assert (yaca_this_worker->worker_magic == YACA_WORKER_MAGIC);
  yaca_this_worker->worker_interrupted = 1;
}

void *
yaca_worker_work (void *d)
{
  struct yaca_worker_st *tsk = (struct yaca_worker_st *) d;
  if (!tsk || tsk->worker_magic != YACA_WORKER_MAGIC)
    YACA_FATAL ("invalid worker@%p", tsk);
  assert (tsk->worker_num > 0 && tsk->worker_num <= YACA_MAX_WORKERS
	  && yaca_worktab + tsk->worker_num == tsk);
  yaca_this_worker = tsk;
  {
    struct sigevent sev = { };
    struct itimerspec its = { };
    memset (&sev, 0, sizeof (sev));
    memset (&its, 0, sizeof (its));
    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = YACA_WORKER_SIGNAL;
    if (timer_create (CLOCK_MONOTONIC, &sev, &tsk->worker_timer))
      YACA_FATAL ("failed to create time for worker #%d - %m",
		  tsk->worker_num);
    its.it_interval.tv_sec = YACA_WORKER_TICKMILLISEC / 1000;
    its.it_interval.tv_nsec = (YACA_WORKER_TICKMILLISEC % 1000) * 1000000;
    its.it_value = its.it_interval;
    timer_settime (tsk->worker_timer, 0, &its, NULL);
  }
  sched_yield ();
#warning incomplete yaca_worker_work
}

void *
yaca_gc_work (void *d)
{
  struct yaca_worker_st *tsk = (struct yaca_worker_st *) d;
  if (!tsk || tsk->worker_magic != YACA_WORKER_MAGIC)
    YACA_FATAL ("invalid worker@%p", tsk);
  assert (tsk->worker_num == -(int) yacaworker_gc && tsk == &yaca_gcworker);
  yaca_this_worker = tsk;
  sched_yield ();
#warning incomplete yaca_gc_work
}

static void
agenda_add_hash_agendael (struct yaca_agendaelem_st *agel)
{
  assert (agel != NULL && agel != YACA_AGENDAELEM_EMPTY);
  assert (agel->agel_magic == YACA_AGENDAELEM_MAGIC);
  unsigned aghsiz = agenda.hashsize;
  assert (aghsiz > 2);
  struct yaca_agendaelem_st **harr = agenda.hasharr;
  assert (harr != NULL);
  assert (agenda.totalcount + YACA_AGENDAELEM_LENGTH < aghsiz);
  for (unsigned eix = 0; eix < YACA_AGENDAELEM_LENGTH; eix++)
    {
      struct yaca_item_st *agitm = agel->agel_elemtab[eix];
      if (!agitm)
	continue;
      assert (agitm->itm_magic == YACA_ITEM_MAGIC);
      unsigned h = agitm->itm_id % aghsiz;
      int pos = -1;
      for (unsigned ix = h; ix < aghsiz && pos < 0; ix++)
	{
	  if (!harr[ix] || harr[ix] == YACA_AGENDAELEM_EMPTY)
	    {
	      harr[ix] = agel;
	      pos = ix;
	      break;
	    }
	  else if (harr[ix] == agel)
	    {
	      pos = ix;
	      break;
	    }
	}
      if (pos < 0)
	for (unsigned ix = 0; ix < h && pos < 0; ix++)
	  {
	    if (!harr[ix] || harr[ix] == YACA_AGENDAELEM_EMPTY)
	      {
		harr[ix] = agel;
		pos = ix;
		break;
	      }
	    else if (harr[ix] == agel)
	      {
		pos = ix;
		break;
	      }
	  };
      assert (pos > 0);
    }
}

static void
agenda_remove_hash_agendael (struct yaca_agendaelem_st *agel)
{
  assert (agel != NULL && agel != YACA_AGENDAELEM_EMPTY);
  assert (agel->agel_magic == YACA_AGENDAELEM_MAGIC);
  unsigned aghsiz = agenda.hashsize;
  assert (aghsiz > 2);
  struct yaca_agendaelem_st **harr = agenda.hasharr;
  assert (harr != NULL);
  assert (agenda.totalcount + YACA_AGENDAELEM_LENGTH < aghsiz);
  for (unsigned eix = 0; eix < YACA_AGENDAELEM_LENGTH; eix++)
    {
      struct yaca_item_st *agitm = agel->agel_elemtab[eix];
      if (!agitm)
	continue;
      assert (agitm->itm_magic == YACA_ITEM_MAGIC);
      unsigned h = agitm->itm_id % aghsiz;
      int pos = -1;
      for (unsigned ix = h; ix < aghsiz && pos < 0; ix++)
	{
	  if (!harr[ix])
	    break;
	  else if (harr[ix] == agel)
	    {
	      harr[ix] = YACA_AGENDAELEM_EMPTY;
	      pos = ix;
	      break;
	    }
	}
      for (unsigned ix = 0; ix < h && pos < 0; ix++)
	{
	  if (!harr[ix])
	    break;
	  else if (harr[ix] == agel)
	    {
	      harr[ix] = YACA_AGENDAELEM_EMPTY;
	      pos = ix;
	      break;
	    }
	}
    }
}

static struct yaca_agendaelem_st *
agenda_find_hash_agendael (struct yaca_item_st *agitm)
{
  if (!agitm)
    return NULL;
  assert (agitm->itm_magic == YACA_ITEM_MAGIC);
  unsigned aghsiz = agenda.hashsize;
  assert (aghsiz > 2);
  struct yaca_agendaelem_st **harr = agenda.hasharr;
  assert (harr != NULL);
  assert (agenda.totalcount + YACA_AGENDAELEM_LENGTH < aghsiz);
  unsigned h = agitm->itm_id % aghsiz;
  for (unsigned ix = h; ix < aghsiz; ix++)
    {
      struct yaca_agendaelem_st *curel = harr[ix];
      if (!curel)
	return NULL;
      else if (curel == YACA_AGENDAELEM_EMPTY)
	continue;
      assert (curel->agel_magic == YACA_AGENDAELEM_MAGIC);
      for (unsigned j = YACA_AGENDAELEM_LENGTH - 1; j >= 0; j--)
	if (curel->agel_elemtab[j] == agitm)
	  return curel;
    }
  for (unsigned ix = 0; ix < h; ix++)
    {
      struct yaca_agendaelem_st *curel = harr[ix];
      if (!curel)
	return NULL;
      else if (curel == YACA_AGENDAELEM_EMPTY)
	continue;
      assert (curel->agel_magic == YACA_AGENDAELEM_MAGIC);
      for (unsigned j = YACA_AGENDAELEM_LENGTH - 1; j >= 0; j--)
	if (curel->agel_elemtab[j] == agitm)
	  return curel;
    }
  return NULL;
}

static void
agenda_delete_agendael (struct yaca_agendaelem_st *agel)
{
  assert (agel != NULL && agel->agel_magic == YACA_AGENDAELEM_MAGIC);
  agenda_remove_hash_agendael (agel);
  struct yaca_agendaelem_st *prevel = agel->agel_prev;
  struct yaca_agendaelem_st *nextel = agel->agel_next;
  uint16_t prio = agel->agel_prio;
  assert (prio > tkprio__none && prio < tkprio__last);
  if (prevel)
    {
      assert (prevel->agel_magic == YACA_AGENDAELEM_MAGIC);
      assert (prevel->agel_next == agel);
      prevel->agel_next = nextel;
    }
  else
    {
      assert (agenda.headtab[prio] == agel);
      agenda.headtab[prio] = nextel;
    }
  if (nextel)
    {
      assert (nextel->agel_magic == YACA_AGENDAELEM_MAGIC);
      assert (nextel->agel_prev == agel);
      nextel->agel_prev = prevel;
    }
  else
    {
      assert (agenda.tailtab[prio] == agel);
      agenda.tailtab[prio] = prevel;
    }
  memset (agel, 0, sizeof (agel));
  free (agel);
}


bool
yaca_agenda_add_back (struct yaca_item_st *agitm, enum yaca_taskprio_en prio)
{
  struct yaca_agendaelem_st *agel = NULL;
  if (!agitm)
    return false;
  assert (agitm->itm_magic == YACA_ITEM_MAGIC);
  if ((int) prio <= 0 || (int) prio >= (int) tkprio__last)
    return false;
  {
    struct yaca_itemtype_st *typit =
      yaca_typetab[agitm->itm_typnum % YACA_ITEM_MAX_TYPE];
    assert (typit && typit->typ_magic == YACA_TYPE_MAGIC);
    if (!typit->typr_runitem)
      return false;
  }
  pthread_mutex_lock (&yaca_agenda_mutex);
  // remove the agitm from its old agenda element
  agel = agenda_find_hash_agendael (agitm);
  if (YACA_UNLIKELY (agel != NULL))
    {
      assert (agel->agel_magic == YACA_AGENDAELEM_MAGIC);
      agenda_remove_hash_agendael (agel);
      for (unsigned j = YACA_AGENDAELEM_LENGTH - 1; j >= 0; j--)
	if (agel->agel_elemtab[j] == agitm)
	  agel->agel_elemtab[j] = NULL;
      bool emptyagel = true;
      for (unsigned j = YACA_AGENDAELEM_LENGTH - 1; j >= 0 && emptyagel; j--)
	emptyagel = (agel->agel_elemtab[j] == NULL);
      if (emptyagel)
	agenda_delete_agendael (agel);
      else
	agenda_add_hash_agendael (agel);
    };
  agel = agenda.tailtab[prio];
  if (YACA_UNLIKELY (agel == NULL))
    {
      agel = malloc (sizeof (*agel));
      if (!agel)
	YACA_FATAL ("failed to allocate agenda element");
      memset (agel, 0, sizeof (*agel));
      agel->agel_magic = YACA_AGENDAELEM_MAGIC;
      agel->agel_prio = prio;
      agel->agel_elemtab[0] = agitm;
      agenda.tailtab[prio] = agenda.headtab[prio] = agel;
    }
  goto end;
end:
  pthread_mutex_unlock (&yaca_agenda_mutex);
}
