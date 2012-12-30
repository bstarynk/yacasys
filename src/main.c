/** file yacasys/src/main.c

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

time_t yaca_start_time;
char yaca_hostname[64];
const char *yaca_progname;
unsigned yaca_nb_workers = 3;
const char *yaca_users_base;
const char *yaca_data_dir = "data";
const char *yaca_source_dir = "src";
const char *yaca_object_dir = "obj";

struct yaca_itemtype_st *yaca_typetab[YACA_ITEM_MAX_TYPE];
pthread_mutex_t yaca_syslog_mutex = PTHREAD_MUTEX_INITIALIZER;
volatile sig_atomic_t yaca_interrupt;

static struct option yaca_options[] = {
  {"help", no_argument, NULL, 'h'},
  {"workers", required_argument, NULL, 'w'},
  {"usersbase", required_argument, NULL, 'u'},
  {"pidfile", required_argument, NULL, 'p'},
  {"datadir", required_argument, NULL, 'd'},
  {"sourcedir", required_argument, NULL, 's'},
  {"objectdir", required_argument, NULL, 'o'},
  {"nice", required_argument, NULL, 'n'},
  {NULL, no_argument, NULL, 0}
};


static struct random_data yaca_random_data;
struct drand48_data yaca_rand48_data;
static pthread_mutex_t yaca_random_mutex = PTHREAD_MUTEX_INITIALIZER;
static const char *pid_file_path;
static int nice_level;

static struct
{
  pthread_mutex_t mutex;
  yaca_id_t sizarr;
  yaca_id_t count;
  struct yaca_item_st **itemarr;	/* array of sizarr entries */
  unsigned char *markarr;	/* array of sizarr entries */
  struct drand48_data r48data;
  pthread_mutexattr_t mutexattr;
} yaca_items =
{
  PTHREAD_MUTEX_INITIALIZER, 0, 0, NULL, NULL,
  {
  },
  {
  }
};

static void
print_usage (void)
{
  printf ("Usage: %s\n", yaca_progname);
  printf ("\t -h | --help " " \t# Give this help.\n");
  printf ("\t -w | --workers <nb-workers> "
	  " \t# Number of working threads.\n");
  printf ("\t -u | --usersbase <users-file> " " \t# file of HTTP users.\n");
  printf ("\t -p | --pid <pid-file> " " \t# written file with pid.\n");
  printf ("\t -s | --sourcedir <directory> " " \t# source directory.\n");
  printf ("\t -d | --datadir <directory> " " \t# data directory.\n");
  printf ("\t -o | --objectdir <directory> " " \t# object directory.\n");
  printf ("\t -n | --nice <nice_level> " " \t# process nice priority.\n");
  printf ("\t built on %s\n", yaca_build_timestamp);
}


static void
remove_pid_file_path_at_exit (void)
{
  if (pid_file_path)
    remove (pid_file_path);
  pid_file_path = NULL;
}

static void
parse_program_arguments (int argc, char **argv)
{
  int opt = -1;
  while ((opt =
	  getopt_long (argc, argv, "hw:u:p:d:s:o:n:", yaca_options,
		       NULL)) >= 0)
    {
      switch (opt)
	{
	case 'h':
	  print_usage ();
	  exit (EXIT_SUCCESS);
	  return;
	case 'w':
	  if (optarg)
	    yaca_nb_workers = atoi (optarg);
	  break;
	case 'u':
	  yaca_users_base = optarg;
	  break;
	case 'p':
	  pid_file_path = optarg;
	  break;
	case 'd':
	  yaca_data_dir = optarg;
	  break;
	case 's':
	  yaca_source_dir = optarg;
	  break;
	case 'o':
	  yaca_object_dir = optarg;
	  break;
	case 'n':
	  if (optarg)
	    nice_level = atoi (optarg);
	default:
	  print_usage ();
	  fprintf (stderr, "%s: unexpected argument\n", yaca_progname);
	  exit (EXIT_FAILURE);
	}
    }
}


static void
initialize_random (void)
{
  FILE *rf = fopen ("/dev/urandom", "r");
  static unsigned rbuf[16];
  if (!rf)
    perror ("yaca fopen /dev/urandom"), exit (EXIT_FAILURE);
  fread (rbuf, sizeof (rbuf), 1, rf);
  seed48_r ((unsigned short int *) (rbuf + 4), &yaca_rand48_data);
  seed48_r ((unsigned short int *) (rbuf + 9), &yaca_items.r48data);
  unsigned int seed = (time (NULL) ^ (getpid ())) + (rbuf[0] ^ rbuf[1] << 7);
  initstate_r (seed, (char *) rbuf, sizeof (rbuf), &yaca_random_data);
}

static void
initialize_items (void)
{
  pthread_mutexattr_init (&yaca_items.mutexattr);
  pthread_mutexattr_settype (&yaca_items.mutexattr,
			     PTHREAD_MUTEX_RECURSIVE_NP);
  unsigned inisiz = 1024;
  yaca_items.itemarr = calloc (inisiz, sizeof (struct yaca_item_st *));
  yaca_items.markarr = calloc (inisiz, sizeof (char));
  yaca_items.sizarr = inisiz;
  if (!yaca_items.itemarr || !yaca_items.markarr)
    YACA_FATAL ("cannot initialize items of %d", inisiz);
}


int32_t
yaca_random (void)
{
  int32_t r;
  pthread_mutex_lock (&yaca_random_mutex);
  random_r (&yaca_random_data, &r);
  pthread_mutex_unlock (&yaca_random_mutex);
  return r;
}


long
yaca_lrand48 (void)
{
  long r;
  pthread_mutex_lock (&yaca_random_mutex);
  lrand48_r (&yaca_rand48_data, &r);
  pthread_mutex_unlock (&yaca_random_mutex);
  return r;
}


double
yaca_drand48 (void)
{
  double d;
  pthread_mutex_lock (&yaca_random_mutex);
  drand48_r (&yaca_rand48_data, &d);
  pthread_mutex_unlock (&yaca_random_mutex);
  return d;
}

struct yaca_item_st *
yaca_item_make (yaca_typenum_t typnum,
		yaca_spacenum_t spacenum, unsigned extrasize)
{
  struct yaca_item_st *itm = NULL;
  size_t sz = extrasize + sizeof (struct yaca_item_st);
  if (!typnum || typnum >= YACA_ITEM_MAX_TYPE)
    YACA_FATAL ("invalid type number %d", (int) typnum);
  if (sz >= YACA_ITEM_MAX_SIZE)
    YACA_FATAL ("invalid total size %ld", (long) sz);
  if (spacenum && spacenum >= YACA_MAX_SPACE)
    YACA_FATAL ("invalid space number %d", (int) spacenum);
  pthread_mutex_lock (&yaca_items.mutex);
  {
    yaca_id_t id = 0;
    if (YACA_UNLIKELY (3 * yaca_items.count + 50 > 2 * yaca_items.sizarr))
      {
	yaca_id_t newsiz = ((3 * yaca_items.count / 2 + 300) | 0x1ff) + 1;
	struct yaca_item_st **newarr =
	  calloc (newsiz, sizeof (struct yaca_item_st *));
	if (!newarr)
	  YACA_FATAL ("failed to grow item array to %ld", (long) newsiz);
	unsigned char *newmarkarr = calloc (newsiz, sizeof (char));
	if (!newmarkarr)
	  YACA_FATAL ("failed to grow mark array to %ld", (long) newsiz);
	if (yaca_items.itemarr)
	  {
	    memcpy (newarr, yaca_items.itemarr,
		    yaca_items.sizarr * sizeof (struct yaca_item_st *));
	    free (yaca_items.itemarr);
	  }
	yaca_items.itemarr = newarr;
	if (yaca_items.markarr)
	  {
	    memcpy (newmarkarr, yaca_items.markarr,
		    yaca_items.sizarr * sizeof (char));
	    free (yaca_items.markarr);
	  }
	yaca_items.markarr = newmarkarr;
	yaca_items.sizarr = newsiz;
      };
    if (yaca_typetab[typnum] == NULL)
      YACA_FATAL ("undefined type number %d", (int) typnum);
    if (spacenum && YACA_UNLIKELY (yaca_spacetab[spacenum] == NULL))
      YACA_FATAL ("undefined space number %d", (int) spacenum);
    do
      {
	long candid = 0;
	lrand48_r (&yaca_items.r48data, &candid);
	candid = candid % yaca_items.sizarr;
	if (candid == 0)
	  candid = 1 + yaca_items.count / 8;
	if (yaca_items.itemarr[candid] == 0)
	  id = candid;
	else if (candid + 1 < yaca_items.sizarr
		 && yaca_items.itemarr[candid + 1] == 0)
	  id = candid + 1;
	else if (candid + 11 < yaca_items.sizarr
		 && yaca_items.itemarr[candid + 11] == 0)
	  id = candid + 11;
	else if (candid + 3 < yaca_items.sizarr
		 && yaca_items.itemarr[candid + 3] == 0)
	  id = candid + 3;
	else if (candid + 19 < yaca_items.sizarr
		 && yaca_items.itemarr[candid + 19] == 0)
	  id = candid + 19;
      }
    while (YACA_UNLIKELY (id == 0));
    itm = calloc (1, sz);
    if (!itm)
      YACA_FATAL ("failed to allocate item of %d bytes", (int) sz);
    itm->itm_id = id;
    itm->itm_typnum = typnum;
    itm->itm_spacnum = spacenum;
    pthread_mutex_init (&itm->itm_mutex, &yaca_items.mutexattr);
    yaca_items.itemarr[id] = itm;
    yaca_items.markarr[id] = 0;
    itm->itm_magic = YACA_ITEM_MAGIC;
    yaca_items.count++;
    goto end;
  }
end:
  pthread_mutex_unlock (&yaca_items.mutex);
  return itm;
}



struct yaca_item_st *
yaca_item_build (yaca_typenum_t typnum, yaca_spacenum_t spacenum,
		 unsigned extrasize, yaca_id_t id)
{
  struct yaca_item_st *itm = NULL;
  size_t sz = extrasize + sizeof (struct yaca_item_st);
  if (!typnum || typnum >= YACA_ITEM_MAX_TYPE)
    YACA_FATAL ("invalid type number %d", (int) typnum);
  if (sz >= YACA_ITEM_MAX_SIZE)
    YACA_FATAL ("invalid total size %ld", (long) sz);
  if (id == 0)
    YACA_FATAL ("zero id for item build");
  if (spacenum && spacenum >= YACA_MAX_SPACE)
    YACA_FATAL ("invalid space number %d", (int) spacenum);
  pthread_mutex_lock (&yaca_items.mutex);
  {
    if (YACA_UNLIKELY (id >= yaca_items.sizarr))
      {
	yaca_id_t newsiz = ((id + yaca_items.count / 4 + 100) | 0x1ff) + 1;
	struct yaca_item_st **newarr =
	  calloc (newsiz, sizeof (struct yaca_item_st *));
	if (!newarr)
	  YACA_FATAL ("failed to grow item array to %ld", (long) newsiz);
	if (yaca_items.itemarr)
	  {
	    memcpy (newarr, yaca_items.itemarr,
		    yaca_items.sizarr * sizeof (struct yaca_item_st *));
	    free (yaca_items.itemarr);
	  }
	yaca_items.itemarr = newarr;
	yaca_items.sizarr = newsiz;
      }
    if (YACA_UNLIKELY (yaca_items.itemarr[id] != NULL))
      YACA_FATAL ("already used id %ld", (long) id);
    if (YACA_UNLIKELY (yaca_typetab[typnum] == NULL))
      YACA_FATAL ("unexistent type number %d", (int) typnum);
    if (spacenum && YACA_UNLIKELY (yaca_spacetab[spacenum] == NULL))
      YACA_FATAL ("undefined space number %d", (int) spacenum);
    itm = calloc (1, sz);
    if (!itm)
      YACA_FATAL ("failed to allocate item of %d bytes", (int) sz);
    itm->itm_id = id;
    itm->itm_typnum = typnum;
    itm->itm_spacnum = spacenum;
    pthread_mutex_init (&itm->itm_mutex, &yaca_items.mutexattr);
    yaca_items.itemarr[id] = itm;
    yaca_items.markarr[id] = 0;
    itm->itm_magic = YACA_ITEM_MAGIC;
    yaca_items.count++;
    goto end;
  }
end:
  pthread_mutex_unlock (&yaca_items.mutex);
  return itm;
}

struct yaca_item_st *
yaca_item_of_id (yaca_id_t id)
{
  struct yaca_item_st *itm = NULL;
  if (id == 0)
    return NULL;
  pthread_mutex_lock (&yaca_items.mutex);
  if (id > 0 && id < yaca_items.sizarr)
    itm = yaca_items.itemarr[id];
  goto end;
  assert (!itm || (itm->itm_magic == YACA_ITEM_MAGIC && itm->itm_id == id));
end:
  pthread_mutex_unlock (&yaca_items.mutex);
  return itm;
}


int
main (int argc, char **argv)
{
  time (&yaca_start_time);
  yaca_progname = (argc > 0) ? argv[0] : "*yacaprogname*";
  parse_program_arguments (argc, argv);
  if (yaca_nb_workers < 2)
    yaca_nb_workers = 2;
  else if (yaca_nb_workers > YACA_MAX_WORKERS)
    yaca_nb_workers = YACA_MAX_WORKERS;
  initialize_random ();
  initialize_items ();
  if (nice_level)
    nice (nice_level);
  openlog ("yacasys", LOG_PID, LOG_DAEMON);
  {
    char nowbuf[64];
    memset (nowbuf, 0, sizeof (nowbuf));
    gethostname (yaca_hostname, sizeof (yaca_hostname) - 1);
    strftime (nowbuf, sizeof (nowbuf), "%Y %b %d %H:%M:%S %Z",
	      localtime (&yaca_start_time));
    syslog (LOG_INFO,
	    "start of yacasys pid %d on %s at %s, %d workers, nice_level %d, built %s",
	    (int) getpid (), yaca_hostname, nowbuf, yaca_nb_workers,
	    nice_level, yaca_build_timestamp);
  }
  if (pid_file_path)
    {
      FILE *pf = fopen (pid_file_path, "w");
      if (pf)
	{
	  fprintf (pf, "%d\n", (int) getpid ());
	  fclose (pf);
	  syslog (LOG_INFO, "wrote pid file %s", pid_file_path);
	  atexit (remove_pid_file_path_at_exit);
	}
      else
	YACA_FATAL ("failed to open pid path %s - %s",
		    pid_file_path, strerror (errno));
    }
}
