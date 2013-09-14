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
#include <stdarg.h>

void register_infile(rzip_control *control, const char *name, char delete);
void register_outfile(rzip_control *control, const char *name, char delete);
void unlink_files(rzip_control *control);
void register_outputfile(rzip_control *control, FILE *f);
void fatal_exit(rzip_control *control);
/* Failure when there is likely to be a meaningful error in perror */
static inline void fatal(const rzip_control *control, unsigned int line, const char *file, const char *func, const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	if (!control->log_cb) {
		vfprintf(stderr, format, ap);
		perror(NULL);
	} else
		control->log_cb(control->log_data, 0, line, file, func, format, ap);
	va_end(ap);
	if (!control->library_mode)
		fatal_exit((rzip_control*)control);
}
#ifdef fatal
# undef fatal
#endif
#define fatal(...) fatal(control, __LINE__, __FILE__, __func__, __VA_ARGS__)
#define fatal_return(stuff, ...) do { \
	fatal stuff; \
	return __VA_ARGS__; \
} while (0)
#define fatal_goto(stuff, label) do { \
	fatal stuff; \
	goto label; \
} while (0)
static inline void failure(const rzip_control *control, unsigned int line, const char *file, const char *func, const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	if (!control->log_cb)
		vfprintf(stderr, format, ap);
	else
		control->log_cb(control->log_data, 0, line, file, func, format, ap);
	va_end(ap);
	if (!control->library_mode)
		fatal_exit((rzip_control*)control);
}
#ifdef failure
# undef failure
#endif
#define failure(...) failure(control, __LINE__, __FILE__, __func__, __VA_ARGS__)
#define failure_return(stuff, ...) do { \
	failure stuff; \
	return __VA_ARGS__; \
} while (0)
#define failure_goto(stuff, label) do { \
	failure stuff; \
	goto label; \
} while (0)
void setup_overhead(rzip_control *control);
void setup_ram(rzip_control *control);
void round_to_page(i64 *size);
size_t round_up_page(rzip_control *control, size_t len);
bool get_rand(rzip_control *control, uchar *buf, int len);
bool read_config(rzip_control *control);
void lrz_stretch(rzip_control *control);
void lrz_stretch2(rzip_control *control);
bool lrz_crypt(const rzip_control *control, uchar *buf, i64 len, const uchar *salt, int encrypt);

#define LRZ_DECRYPT	(0)
#define LRZ_ENCRYPT	(1)

static inline bool lrz_encrypt(const rzip_control *control, uchar *buf, i64 len, const uchar *salt)
{
	return lrz_crypt(control, buf, len, salt, LRZ_ENCRYPT);
}

static inline bool lrz_decrypt(const rzip_control *control, uchar *buf, i64 len, const uchar *salt)
{
	return lrz_crypt(control, buf, len, salt, LRZ_DECRYPT);
}

#endif
