/** file yacasys/src/yaca.h

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

typedef struct yaca_item_st *yaca_loaditem_sig_t (json_t *, yaca_id_t);
typedef void yaca_fillitem_sig_t (json_t *, struct yaca_item_st *);
typedef json_t *yaca_dumpitem_sig_t (struct yaca_item_st *);
typedef json_t *yaca_dumpcontent_sig_t (struct yaca_item_st *);
typedef void yaca_runitem_sig_t (struct yaca_item_st *);
typedef void yaca_scandump_sig_t (struct yaca_item_st *,
				  struct yaca_dumper_st *);

///// items

#define YACA_ITEM_MAGIC 971394241	/*0x39e64cc1 */
struct yaca_item_st
{
  uint32_t itm_magic;		/* always YACA_ITEM_MAGIC */
  yaca_id_t itm_id;
  yaca_typenum_t itm_typnum;
  yaca_spacenum_t itm_spacnum;
  pthread_mutex_t itm_mutex;
  long itm_dataspace[];
};
#define YACA_ITEM_MAX_SIZE (256*1024*sizeof(void*))

struct yaca_item_st *yaca_item_make (yaca_typenum_t typnum,
				     yaca_spacenum_t spacenum,
				     unsigned extrasize);
struct yaca_item_st *yaca_item_build (yaca_typenum_t typnum,
				      yaca_spacenum_t spacenum,
				      unsigned extrasize, yaca_id_t id);
struct yaca_item_st *yaca_item_of_id (yaca_id_t id);

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


void yaca_load (void);

void yaca_start_agenda (void);
void yaca_interrupt_agenda (void);
void yaca_stop_agenda (void);
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

#endif /* _YACA_H_INCLUDED_ */
/* eof yacasys/yaca.h */
