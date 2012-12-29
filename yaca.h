/** file yacasys/yaca.h

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
#include <fastcgi.h>
#include <unistd.h>
#include <time.h>
#include <syslog.h>
#include <getopt.h>
#include <string.h>
#include <jansson.h>
#include <pthread.h>

#define YACA_MAX_WORKERS 16
#define YACA_MAX_TYPENUM 4096

#define YACA_FATAL(Fmt,...) do {pthread_mutex_lock(&yaca_syslog_mutex); \
    syslog(LOG_CRIT,"[%s:%d] FATAL: " Fmt, __FILE__, __LINE__,		\
	   ##__VA_ARGS__); abort(); }while(0)

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
extern const char *yaca_state_file;
extern unsigned yaca_nb_workers;
extern pthread_mutex_t yaca_syslog_mutex;

// pseudo-random number generator, like random(3), lrand48(3),
// drand48(3) but with a mutex
int32_t yaca_random (void);
long yaca_lrand48 (void);
double yaca_drand48 (void);


typedef uint32_t yaca_id_t;
typedef uint16_t yaca_typenum_t;
typedef uint16_t yaca_mark_t;
struct yaca_item_st;
struct yaca_itemtype_st;

typedef struct yaca_item_st *yaca_loaditem_sig_t (json_t *, yaca_id_t);
typedef void yaca_fillitem_sig_t (json_t *, struct yaca_item_st *);
typedef json_t *yaca_dumpitem_sig_t (struct yaca_item_st *);
typedef json_t *yaca_dumpcontent_sig_t (struct yaca_item_st *);
typedef int yaca_runitem_sig_t (struct yaca_item_st *);

struct yaca_item_st
{
  uint32_t itm_magic;
  yaca_id_t itm_id;
  yaca_typenum_t itm_typnum;
  yaca_mark_t itm_mark;
  pthread_mutex_t itm_mutex;
  long itm_dataspace[];
};
#define YACA_ITEM_MAGIC 971394241	/*0x39e64cc1 */
#define YACA_ITEM_MAX_SIZE (256*1024*sizeof(void*))

struct yaca_itemtype_st
{
  uint32_t typ_magic;
  yaca_typenum_t typ_num;
  const char *typ_name;
  struct ya_item_t *typ_item;
  yaca_loaditem_sig_t *typr_loaditem;
  yaca_fillitem_sig_t *typr_fillitem;
  yaca_dumpitem_sig_t *typr_dumpitem;
  yaca_dumpcontent_sig_t *typr_dumpcontent;
  yaca_runitem_sig_t *typr_runitem;
  void *typ_spare_[10];
};

#define YACA_ITEM_MAX_TYPE 4096
extern struct yaca_itemtype_st *yaca_typetab[];

struct yaca_item_st *yaca_item_make (yaca_typenum_t typnum,
				     unsigned extrasize);
struct yaca_item_st *yaca_item_build (yaca_typenum_t typnum,
				      unsigned extrasize, yaca_id_t id);
struct yaca_item_st *yaca_item_of_id (yaca_id_t id);

#endif /* _YACA_H_INCLUDED_ */
/* eof yacasys/yaca.h */
