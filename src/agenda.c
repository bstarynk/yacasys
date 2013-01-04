/** file yacasys/src/agenda.c

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


__thread struct yaca_worker_st *yaca_this_worker;

/**
   We have a small number of worker threads; each worker thread is
   "interruptible" (either by a timer, every few milliseconds, or by
   other workers or threads. We depend upon the fact that a SIGALRM
   signal, produced by a timer, is sent to its thread. 

   We manage an agenda, which is an organization of task items; each
   task has a priority, and belongs to a FIFO queue of that
   priority. The agenda contains several priority queues of agenda
   entries (each containing a task item). The worker threads are
   taking each one task from the agenda, and run that task (in turn,
   this task run usually update the agenda).
**/



struct yaca_worker_st yaca_worktab[YACA_MAX_WORKERS + 1];




static pthread_mutex_t yaca_agenda_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t yaca_agendachanged_cond = PTHREAD_COND_INITIALIZER;

typedef int32_t yaca_agindex_t;

#define YACA_AGENTRY_MAGIC 1049484667	/*0x3e8ddd7b */
#define YACA_AGENTRY_EMPTY ~0U	/*0xffffffff */

struct yaca_agentry_st
{
  unsigned age_magic;		/* always YACA_AGENTRY_MAGIC (or 0 or
				   YACA_AGENTRY_EMPTY for an empty
				   entry) */
  uint16_t age_prio;		/* priority of the tasklet */
  struct yaca_item_st *age_item;	/* task item */
  /* next and previous indexes in the priority queue */
  yaca_agindex_t age_nextix;
#define age_nextfreeix age_nextix
  yaca_agindex_t age_previx;
  yaca_agindex_t age_hashix;	/* index in hashtable */
};

struct yaca_agenda_st
{
  yaca_agindex_t ag_count;
  yaca_agindex_t ag_size;	/* some prime number above ag_count */
  struct yaca_agentry_st *ag_arr;	/* of ag_size elements, entry#0 unused */
  yaca_agindex_t *ag_hasht;	/* of ag_size elements */
  // head & tail index of each priority queue
  yaca_agindex_t ag_headix[1 + (int) tkprio__last];
  yaca_agindex_t ag_tailix[1 + (int) tkprio__last];
  yaca_agindex_t ag_freeix;	/* index of first free element */
  enum yaca_agenda_state_en ag_state;
};
static struct yaca_agenda_st agenda;

static struct yaca_worker_st yaca_gcworker;
static struct yaca_worker_st yaca_fcgiworker;


static void *yaca_worker_work (void *);


// return false if agenda stopped
static bool yaca_do_one_task (void);

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
    pthread_create (&tsk->worker_thread, NULL, yaca_gcthread_work, tsk);
  }
  goto end;
end:
  pthread_mutex_unlock (&yaca_agenda_mutex);
}

void
yaca_interrupt_agenda (enum yaca_interrupt_reason_en ireas)
{
  pthread_mutex_lock (&yaca_agenda_mutex);
  yaca_interrupt = 1;
  for (unsigned ix = 1; ix <= yaca_nb_workers; ix++)
    {
      struct yaca_worker_st *tsk = yaca_worktab + ix;
      assert (tsk->worker_magic == YACA_WORKER_MAGIC);
      tsk->worker_interrupted = 1;
      if (ireas > yaint__none && ireas < yaint__last)
	tsk->worker_need |= (1 << (int) ireas);
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
  long cnt = 0;
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
  for (;;)
    {
      if (yaca_do_one_task ())
	{
	  break;
	}
      cnt++;
      if (YACA_UNLIKELY (cnt % 1024 == 0))
	sched_yield ();
      uint32_t need = 0;
      {
	pthread_mutex_lock (&yaca_agenda_mutex);
	need = tsk->worker_need;
	tsk->worker_need = 0;
	pthread_mutex_unlock (&yaca_agenda_mutex);
      }
      if (need & (1 << yaint_gc))
	yaca_worker_garbcoll ();
    }
#warning incomplete yaca_worker_work
}


static void
initialize_agenda (unsigned sizlow)
{
  unsigned long primsiz = yaca_prime_after (sizlow + 10);
  if (YACA_UNLIKELY (primsiz < sizlow || primsiz > UINT_MAX))
    YACA_FATAL ("cannot initialize agenda of %u elements", sizlow);
  agenda.ag_count = 0;
  agenda.ag_size = primsiz;
  struct yaca_agentry_st *arr =
    calloc (primsiz, sizeof (struct yaca_agentry_st));
  if (YACA_UNLIKELY (!arr))
    YACA_FATAL ("cannot allocate agenda entry table of %lu elements",
		primsiz);
  agenda.ag_arr = arr;
  agenda.ag_hasht = calloc (primsiz, sizeof (yaca_agindex_t));
  if (YACA_UNLIKELY (!agenda.ag_hasht))
    YACA_FATAL ("cannot allocate agenda hash table of %lu elements", primsiz);
  // make the free list, but skip entry#0
  yaca_agindex_t frix = 0;
  for (yaca_agindex_t ix = primsiz - 1; ix > 0; ix--)
    {
      arr[ix].age_nextfreeix = frix;
      frix = ix;
    };
  agenda.ag_freeix = frix;
}


static struct yaca_agentry_st *
find_agentry (struct yaca_item_st *agitm)
{
  if (!agitm)
    return NULL;
  assert (agitm->itm_magic == YACA_ITEM_MAGIC);
  yaca_agindex_t siz = agenda.ag_size;
  assert (siz > 2);
  yaca_agindex_t cnt = agenda.ag_count;
  assert (cnt + 1 < siz);
  struct yaca_agentry_st *arr = agenda.ag_arr;
  yaca_agindex_t *hasht = agenda.ag_hasht;
  assert (arr != NULL);
  assert (hasht != NULL);
  yaca_agindex_t ith = agitm->itm_id % siz;
  for (yaca_agindex_t hx = ith; hx < siz; hx++)
    {
      yaca_agindex_t curix = hasht[hx];
      if (!curix)
	return NULL;
      if (curix == YACA_AGENTRY_EMPTY)
	continue;
      assert (curix < siz);
      struct yaca_agentry_st *ae = arr + curix;
      if (ae->age_magic == 0)
	return NULL;
      if (ae->age_magic == YACA_AGENTRY_EMPTY)
	continue;
      if (ae->age_magic == YACA_AGENTRY_MAGIC && ae->age_item == agitm)
	return ae;
    }
  for (yaca_agindex_t hx = 0; hx < ith; hx++)
    {
      yaca_agindex_t curix = hasht[hx];
      if (!curix)
	return NULL;
      if (curix == YACA_AGENTRY_EMPTY)
	continue;
      assert (curix < siz);
      struct yaca_agentry_st *ae = arr + curix;
      if (ae->age_magic == 0)
	return NULL;
      if (ae->age_magic == YACA_AGENTRY_EMPTY)
	continue;
      if (ae->age_magic == YACA_AGENTRY_MAGIC && ae->age_item == agitm)
	return ae;
    }
  return NULL;
}


// add or find an entry, but don't bother adding it to priority queues
// (it is the role of the caller)
static struct yaca_agentry_st *
add_agentry (struct yaca_item_st *agitm, unsigned prio)
{
  if (!agitm)
    return NULL;
  assert (agitm->itm_magic == YACA_ITEM_MAGIC);
  yaca_agindex_t siz = agenda.ag_size;
  assert (siz > 2);
  yaca_agindex_t cnt = agenda.ag_count;
  assert (cnt + 5 < siz);
  struct yaca_agentry_st *arr = agenda.ag_arr;
  yaca_agindex_t *hasht = agenda.ag_hasht;
  assert (arr != NULL);
  assert (hasht != NULL);
  assert (agenda.ag_freeix > 0 && agenda.ag_freeix < siz);
  int hpos = -1;
  yaca_agindex_t ith = agitm->itm_id % siz;
  for (yaca_agindex_t hx = ith; hx < siz; hx++)
    {
      yaca_agindex_t curix = hasht[hx];
      if (!curix)
	{
	  if (hpos < 0)
	    hpos = hx;
	  break;
	}
      else if (curix == YACA_AGENTRY_EMPTY)
	{
	  if (hpos < 0)
	    hpos = hx;
	  continue;
	}
      assert (curix < siz);
      struct yaca_agentry_st *ae = arr + curix;
      if (ae->age_magic == 0)
	{
	  if (hpos < 0)
	    hpos = hx;
	  break;
	}
      if (ae->age_magic == YACA_AGENTRY_EMPTY)
	{
	  if (hpos < 0)
	    hpos = hx;
	  continue;
	}
      if (ae->age_magic == YACA_AGENTRY_MAGIC && ae->age_item == agitm)
	return ae;
    }
  for (yaca_agindex_t hx = 0; hx < ith; hx++)
    {
      yaca_agindex_t curix = hasht[hx];
      if (!curix)
	{
	  if (hpos < 0)
	    hpos = hx;
	  break;
	}
      else if (curix == YACA_AGENTRY_EMPTY)
	{
	  if (hpos < 0)
	    hpos = hx;
	  continue;
	}
      assert (curix < siz);
      struct yaca_agentry_st *ae = arr + curix;
      if (ae->age_magic == 0)
	{
	  if (hpos < 0)
	    hpos = hx;
	  break;
	}
      if (ae->age_magic == YACA_AGENTRY_EMPTY)
	{
	  if (hpos < 0)
	    hpos = hx;
	  continue;
	}
      if (ae->age_magic == YACA_AGENTRY_MAGIC && ae->age_item == agitm)
	return ae;
    }
  assert (hpos >= 0 && hpos < siz);
  yaca_agindex_t nix = agenda.ag_freeix;
  assert (nix > 0 && nix < siz);
  struct yaca_agentry_st *nae = arr + nix;
  agenda.ag_freeix = nae->age_nextfreeix;
  agenda.ag_count++;
  memset (nae, 0, sizeof (*nae));
  nae->age_magic = YACA_AGENTRY_MAGIC;
  nae->age_prio = prio;
  nae->age_item = agitm;
  nae->age_hashix = hpos;
  hasht[hpos] = nix;
  return nae;
}


static void
reorganize_agenda (unsigned gap)
{
  struct yaca_agenda_st oldagenda = agenda;
  yaca_agindex_t oldcount = oldagenda.ag_count;
  yaca_agindex_t oldsize = oldagenda.ag_size;
  struct yaca_agentry_st *oldarr = oldagenda.ag_arr;
  enum yaca_agenda_state_en oldstate = oldagenda.ag_state;
  assert (oldarr != NULL);
  initialize_agenda (3 * oldcount / 2 + 50 + gap);
  for (unsigned prio = 1; prio < tkprio__last; prio++)
    {
      for (yaca_agindex_t ix = oldagenda.ag_headix[prio];
	   ix > 0; ix = oldarr[ix].age_nextix)
	{
	  assert (ix > 0 && ix < oldsize);
	  struct yaca_agentry_st *oldae = oldarr + ix;
	  assert (oldae->age_magic == YACA_AGENTRY_MAGIC);
	  assert (oldae->age_item != NULL);
	  assert (oldae->age_prio == prio);
	  yaca_agindex_t pfrix = agenda.ag_freeix;
	  assert (pfrix > 0);
	  struct yaca_agentry_st *ae = add_agentry (oldae->age_item, prio);
	  assert (ae == agenda.ag_arr + pfrix);
	  assert (ae->age_nextix == 0 && ae->age_previx == 0);
	  if (agenda.ag_headix[prio] == 0)
	    {
	      agenda.ag_headix[prio] = agenda.ag_tailix[prio] = pfrix;
	    }
	  else
	    {
	      yaca_agindex_t tailix = agenda.ag_tailix[prio];
	      assert (tailix > 0 && tailix < agenda.ag_size);
	      struct yaca_agentry_st *tailae = agenda.ag_arr + tailix;
	      assert (tailae->age_magic == YACA_AGENTRY_MAGIC);
	      tailae->age_nextix = pfrix;
	      ae->age_previx = tailix;
	      agenda.ag_tailix[prio] = pfrix;
	    }
	}
    };
  memset (oldagenda.ag_arr, 0, sizeof (struct yaca_agentry_st) * oldsize);
  memset (oldagenda.ag_hasht, 0, sizeof (yaca_agindex_t) * oldsize);
  free (oldagenda.ag_arr);
  free (oldagenda.ag_hasht);
  agenda.ag_state = oldstate;
}

// unlink an existing agenda entry from its priority queue
static void
agenda_unlink (struct yaca_agentry_st *agel)
{
  assert (agel && agel->age_magic == YACA_AGENTRY_MAGIC);
  unsigned oldprio = agel->age_prio;
  // remove that entry from the oldprio
  assert (oldprio > 0 && oldprio < (int) tkprio__last);
  yaca_agindex_t oldprevix = agel->age_previx;
  yaca_agindex_t oldnextix = agel->age_nextix;
  assert (oldprevix < agenda.ag_size);
  assert (oldnextix < agenda.ag_size);
  if (oldprevix == 0)
    agenda.ag_headix[oldprio] = oldnextix;
  else
    agenda.ag_arr[oldprevix].age_nextix = oldnextix;
  if (oldnextix == 0)
    agenda.ag_tailix[oldprio] = oldprevix;
  else
    agenda.ag_arr[oldnextix].age_previx = oldprevix;
}

bool
yaca_agenda_add_back (struct yaca_item_st *agitm, enum yaca_taskprio_en prio)
{
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
  {
    if (YACA_UNLIKELY (4 * agenda.ag_count + 50 >= 3 * agenda.ag_size))
      reorganize_agenda (agenda.ag_count / 4 + 30);
    yaca_agindex_t pfrix = agenda.ag_freeix;
    assert (pfrix > 0);
    struct yaca_agentry_st *agel = add_agentry (agitm, prio);
    if (YACA_UNLIKELY (agel != agenda.ag_arr + pfrix))
      {				// existing agenda entry; should unlink it
	agenda_unlink (agel);
      };
    //add this entry at tail of priority queue
    {
      yaca_agindex_t oldtailix = agenda.ag_tailix[prio];
      if (oldtailix == 0)
	{
	  agenda.ag_tailix[prio] = agenda.ag_headix[prio] = pfrix;
	}
      else
	{
	  assert (oldtailix < agenda.ag_size);
	  agel->age_previx = oldtailix;
	  agenda.ag_arr[oldtailix].age_nextix = pfrix;
	}
    }
    pthread_cond_broadcast (&yaca_agendachanged_cond);
  }
  goto end;
end:
  pthread_mutex_unlock (&yaca_agenda_mutex);
  return true;
}



bool
yaca_agenda_add_front (struct yaca_item_st * agitm,
		       enum yaca_taskprio_en prio)
{
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
  {
    if (YACA_UNLIKELY (4 * agenda.ag_count + 50 >= 3 * agenda.ag_size))
      reorganize_agenda (agenda.ag_count / 4 + 30);
    yaca_agindex_t pfrix = agenda.ag_freeix;
    assert (pfrix > 0);
    struct yaca_agentry_st *agel = add_agentry (agitm, prio);
    if (YACA_UNLIKELY (agel != agenda.ag_arr + pfrix))
      {				// existing agenda entry; should unlink it
	agenda_unlink (agel);
      };
    //add this entry at head of priority queue
    {
      yaca_agindex_t oldheadix = agenda.ag_headix[prio];
      if (oldheadix == 0)
	{
	  agenda.ag_headix[prio] = agenda.ag_tailix[prio] = pfrix;
	}
      else
	{
	  assert (oldheadix < agenda.ag_size);
	  agel->age_nextix = oldheadix;
	  agenda.ag_arr[oldheadix].age_previx = pfrix;
	}
    }
    pthread_cond_broadcast (&yaca_agendachanged_cond);
  }
  goto end;
end:
  pthread_mutex_unlock (&yaca_agenda_mutex);
  return true;
}

enum yaca_taskprio_en
yaca_agenda_remove (struct yaca_item_st *agitm)
{
  enum yaca_taskprio_en oldprio = tkprio__none;
  if (!agitm)
    return tkprio__none;
  assert (agitm->itm_magic == YACA_ITEM_MAGIC);
  pthread_mutex_lock (&yaca_agenda_mutex);
  {
    if (YACA_UNLIKELY
	(3 * agenda.ag_count < agenda.ag_size && agenda.ag_size > 200))
      reorganize_agenda (agenda.ag_count / 4 + 10);
    struct yaca_agentry_st *agel = find_agentry (agitm);
    if (!agel)
      goto end;
    assert (agel->age_magic == YACA_AGENTRY_MAGIC);
    oldprio = agel->age_prio;
    agenda_unlink (agel);
    agenda.ag_count--;
    pthread_cond_broadcast (&yaca_agendachanged_cond);
  }
  goto end;
end:
  pthread_mutex_unlock (&yaca_agenda_mutex);
  return oldprio;
}

bool
yaca_do_one_task (void)
{
  static long docount;
  bool res = false;
  struct yaca_item_st *agitm = NULL;
  pthread_mutex_lock (&yaca_agenda_mutex);
  {
    while (agenda.ag_count == 0 && agenda.ag_state == yacag_run)
      {
	struct timespec ts = { 0, 0 };
	clock_gettime (CLOCK_REALTIME, &ts);
	ts.tv_nsec += 2 * YACA_WORKER_TICKMILLISEC * 1000000;
	while (YACA_UNLIKELY (ts.tv_nsec > 1000000000))
	  {
	    ts.tv_sec++;
	    ts.tv_nsec -= 1000000000;
	  };
	pthread_cond_timedwait (&yaca_agendachanged_cond, &yaca_agenda_mutex,
				&ts);
      }
    if (agenda.ag_state != yacag_run)
      goto end;
    if (agenda.ag_count == 0)
      goto end;
    for (unsigned prio = tkprio__last - 1; prio > 0 && !agitm; prio--)
      {
	yaca_agindex_t tix = 0;
	yaca_agindex_t nextix = 0;
	for (tix = agenda.ag_headix[prio]; tix != 0 && !agitm; tix = nextix)
	  {
	    if (!tix)
	      break;
	    nextix = 0;
	    assert (tix > 0 && tix < agenda.ag_size);
	    struct yaca_agentry_st *agel = agenda.ag_arr + tix;
	    assert (agel->age_magic == YACA_AGENTRY_MAGIC);
	    assert (agel->age_prio == prio);
	    nextix = agel->age_nextix;
	    agitm = agel->age_item;
	    agenda_unlink (agel);
	    agenda.ag_count--;
	  }
      }
    if (agitm)
      {
	docount++;
	if (YACA_UNLIKELY (docount % 1024 == 0
			   && agenda.ag_size > 100
			   && 3 * agenda.ag_count + 50 < agenda.ag_size))
	  reorganize_agenda (agenda.ag_count / 4 + 10);
      }
  }
  goto end;
end:
  if (agitm)
    yaca_this_worker->worker_state = yawrk_run;
  else
    yaca_this_worker->worker_state = yawrk_idle;
  pthread_mutex_unlock (&yaca_agenda_mutex);
  if (agitm)
    {
      /// run the item
      assert (agitm->itm_magic == YACA_ITEM_MAGIC);
      yaca_typenum_t typnum = agitm->itm_typnum;
      assert (typnum > 0 && typnum < YACA_ITEM_MAX_TYPE);
      struct yaca_itemtype_st *typ = yaca_typetab[typnum];
      assert (typ && typ->typ_magic == YACA_TYPE_MAGIC);
      yaca_runitem_sig_t *run = typ->typr_runitem;
      if (run)
	{
	  (*run) (agitm);
	  res = true;
	}
    }
  return res;
}


void
yaca_agenda_stop (void)
{
  pthread_mutex_lock (&yaca_agenda_mutex);
  agenda.ag_state = yacag_stop;
  pthread_cond_broadcast (&yaca_agendachanged_cond);
  goto end;
end:
  pthread_mutex_unlock (&yaca_agenda_mutex);
#warning should wait for all workers to stop
}

void
yaca_should_garbage_collect (void)
{
  yaca_interrupt_agenda (yaint_gc);
}
