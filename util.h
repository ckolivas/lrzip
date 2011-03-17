/*
   Copyright (C) 2006-2011 Con Kolivas
   Copyright (C) 2011 Peter Hyman
   Copyright (C) 1998 Andrew Tridgell

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program. If not, see <http://www.gnu.org/licenses/>.
*/
#ifndef LRZIP_UTIL_H
#define LRZIP_UTIL_H

#include "lrzip_private.h"

void register_infile(const char *name, char delete);
void register_outfile(const char *name, char delete);
void unlink_files(void);
void register_outputfile(FILE *f);
void fatal(const char *format, ...);
void failure(const char *format, ...);
void round_to_page(i64 *size);
void get_rand(uchar *buf, int len);
void lrz_crypt(rzip_control *control, uchar *buf, i64 len, uchar *salt, int encrypt);

#endif
