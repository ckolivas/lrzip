/*
   Copyright (C) 2006-2011 Con Kolivas
   Copyright (C) 2011 Peter Hyman
   Copyright (C) 1998-2003 Andrew Tridgell

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

#ifndef LIBLRZIP_H
#define LIBLRZIP_H

#include <stdbool.h>

typedef struct Lrzip Lrzip;

typedef enum {
	LRZIP_MODE_NONE,
	LRZIP_MODE_DECOMPRESS,
	LRZIP_MODE_NO_COMPRESS,
	LRZIP_MODE_LZO_COMPRESS,
	LRZIP_MODE_BZIP2_COMPRESS,
	LRZIP_MODE_ZLIB_COMPRESS,
	LRZIP_MODE_ZPAQ_COMPRESS
} Lrzip_Mode;

typedef void (*Lrzip_Password_Cb)(void *, char **, size_t);

bool lrzip_init(void);
void lrzip_config_env(Lrzip *lr);
void lrzip_free(Lrzip *lr);
Lrzip *lrzip_new(Lrzip_Mode mode);
Lrzip_Mode lrzip_mode_get(Lrzip *lr);
char **lrzip_files_get(Lrzip *lr);
bool lrzip_file_add(Lrzip *lr, const char *file);
bool lrzip_file_del(Lrzip *lr, const char *file);
void lrzip_outfile_set(Lrzip *lr, const char *file);
const char *lrzip_outfile_get(Lrzip *lr);
bool lrzip_run(Lrzip *lr);

#endif

