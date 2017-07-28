/************************************************************************
 *   IRC - Internet Relay Chat, irc/c_debug.c
 *   Copyright (C) 1990 Jarkko Oikarinen and
 *                      University of Oulu, Computing Center
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 1, or (at your option)
 *   any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

char debug_id[] = "debug.c v2.0 (c) 1988 University of Oulu, Computing Center and Jarkko Oikarinen";

#include <sys/types.h>
#include <sys/file.h>
#include <stdio.h>
#include "struct.h"
#include "common.h"
#if HPUX
#include <fcntl.h>
#endif

extern int debuglevel;

#ifdef DEBUGMODE
debug(level, form, para1, para2, para3, para4, para5, para6)
int level;
char *form, *para1, *para2, *para3, *para4, *para5, *para6;
{
  if (debuglevel >= 0)
    if (level <= debuglevel) {
      char buf[512];
      sprintf(buf, form, para1, para2, para3, para4, para5, para6);
      putline(buf);
    }
}
#else
debug()
{
	/* do nothing so we dont chew much cpu (hopefully!) -avalon */
}
#endif
