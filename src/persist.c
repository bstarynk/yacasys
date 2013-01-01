/** file yacasys/src/persist.c

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

#define YACA_DUMP_MAGIC 684241137	/*0x28c8b0f1 */
struct yaca_dumper_st
{
  uint32_t dump_magic;
};

void
yaca_load (void)
{
}


void
yaca_dump (void)
{
}
