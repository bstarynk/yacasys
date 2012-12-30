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

#define YACA_TASK_MAGIC 471856441	/*0x1c1ff539 */
struct yaca_task_st
{
  uint32_t task_magic;
  int16_t task_num;
  pthread_t task_thread;
  timer_t task_timer;
  volatile sig_atomic_t task_interrupted;
};

#define YACA_TASK_SIGNAL SIGALRM
#define YACA_TASK_TICKMILLISEC 2	/* milliseconds */
static __thread struct yaca_task_st *yaca_this_task;

struct yaca_task_st yaca_worktab[YACA_MAX_WORKERS + 1];

enum yacaspectask_en
{
  yacatask__none,
  yacatask_gc,
  yacatask_fcgi,
  yacatask__last
};

static struct yaca_task_st yaca_gctask;
static struct yaca_task_st yaca_fcgitask;

static pthread_mutex_t yaca_agenda_mutex = PTHREAD_MUTEX_INITIALIZER;

static void *yaca_task_work (void *);
static void *yaca_gc_work (void *);

void
yaca_start_agenda (void)
{
  pthread_mutex_lock (&yaca_agenda_mutex);
  // start the workers
  assert (yaca_nb_workers >= 2 && yaca_nb_workers <= YACA_MAX_WORKERS);
  for (unsigned ix = 1; ix <= yaca_nb_workers; ix++)
    {
      struct yaca_task_st *tsk = yaca_worktab + ix;
      assert (!tsk->task_thread);
      tsk->task_num = ix;
      tsk->task_magic = YACA_TASK_MAGIC;
      pthread_create (&tsk->task_thread, NULL, yaca_task_work, tsk);
    }
  // start the gc task
  {
    struct yaca_task_st *tsk = &yaca_gctask;
    assert (!tsk->task_thread);
    tsk->task_num = -(int) yacatask_gc;
    tsk->task_magic = YACA_TASK_MAGIC;
    pthread_create (&tsk->task_thread, NULL, yaca_gc_work, tsk);
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
      struct yaca_task_st *tsk = yaca_worktab + ix;
      assert (tsk->task_magic == YACA_TASK_MAGIC);
      tsk->task_interrupted = 1;
      pthread_kill (tsk->task_thread, YACA_TASK_SIGNAL);
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
end:
  pthread_mutex_unlock (&yaca_agenda_mutex);
}

static void
yaca_work_alarm_sigaction (int sig, siginfo_t * sinf, void *data)
{
  assert (sig == YACA_TASK_SIGNAL);
  assert (yaca_this_task && yaca_this_task->task_magic == YACA_TASK_MAGIC);
  yaca_interrupt = 1;
  yaca_this_task->task_interrupted = 1;
}

void *
yaca_task_work (void *d)
{
  struct yaca_task_st *tsk = (struct yaca_task_st *) d;
  if (!tsk || tsk->task_magic != YACA_TASK_MAGIC)
    YACA_FATAL ("invalid task@%p", tsk);
  assert (tsk->task_num > 0 && tsk->task_num <= YACA_MAX_WORKERS
	  && yaca_worktab + tsk->task_num == tsk);
  yaca_this_task = tsk;
  {
    struct sigaction alact;
    memset (&alact, 0, sizeof (alact));
    alact.sa_sigaction = yaca_work_alarm_sigaction;
    alact.sa_flags = SA_SIGINFO;
    sigaction (YACA_TASK_SIGNAL, &alact, NULL);
  }
  {
    struct sigevent sev = { };
    struct itimerspec its = { };
    memset (&sev, 0, sizeof (sev));
    memset (&its, 0, sizeof (its));
    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = YACA_TASK_SIGNAL;
    if (timer_create (CLOCK_MONOTONIC, &sev, &tsk->task_timer))
      YACA_FATAL ("failed to create time for task #%d - %m", tsk->task_num);
    its.it_interval.tv_sec = YACA_TASK_TICKMILLISEC / 1000;
    its.it_interval.tv_nsec = (YACA_TASK_TICKMILLISEC % 1000) * 1000000;
    its.it_value = its.it_interval;
    timer_settime (tsk->task_timer, 0, &its, NULL);
  }
  sched_yield ();
#warning incomplete yaca_task_work
}

void *
yaca_gc_work (void *d)
{
  struct yaca_task_st *tsk = (struct yaca_task_st *) d;
  if (!tsk || tsk->task_magic != YACA_TASK_MAGIC)
    YACA_FATAL ("invalid task@%p", tsk);
  assert (tsk->task_num == -(int) yacatask_gc && tsk == &yaca_gctask);
  yaca_this_task = tsk;
  sched_yield ();
#warning incomplete yaca_gc_work
}
