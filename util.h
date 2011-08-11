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

void register_infile(rzip_control *control, const char *name, char delete);
void register_outfile(rzip_control *control, const char *name, char delete);
void unlink_files(rzip_control *control);
void register_outputfile(rzip_control *control, FILE *f);
void fatal(const rzip_control *control, const char *format, ...);
void failure(const rzip_control *control, const char *format, ...);
void setup_overhead(rzip_control *control);
void setup_ram(rzip_control *control);
void round_to_page(i64 *size);
void get_rand(rzip_control *control, uchar *buf, int len);
void read_config(rzip_control *control);
void lrz_stretch(rzip_control *control);
void lrz_stretch2(rzip_control *control);
void lrz_crypt(const rzip_control *control, uchar *buf, i64 len, const uchar *salt, int encrypt);

#define LRZ_DECRYPT	(0)
#define LRZ_ENCRYPT	(1)

static inline void lrz_encrypt(const rzip_control *control, uchar *buf, i64 len, const uchar *salt)
{
	lrz_crypt(control, buf, len, salt, LRZ_ENCRYPT);
}

static inline void lrz_decrypt(const rzip_control *control, uchar *buf, i64 len, const uchar *salt)
{
	lrz_crypt(control, buf, len, salt, LRZ_DECRYPT);
}

#endif
