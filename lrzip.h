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
#ifndef LRZIP_H
#define LRZIP_H

#define LRZIP_MAJOR_VERSION 0
#define LRZIP_MINOR_VERSION 5
#define LRZIP_MINOR_SUBVERSION 70

#define NUM_STREAMS 2
#define STREAM_BUFSIZE (1024 * 1024 * 10)

#include "config.h"
#include <stdint.h>

#ifndef uchar
#define uchar unsigned char
#endif

#ifndef int32
#if (SIZEOF_INT == 4)
#define int32 int
#elif (SIZEOF_LONG == 4)
#define int32 long
#elif (SIZEOF_SHORT == 4)
#define int32 short
#endif
#endif

#ifndef int16
#if (SIZEOF_INT == 2)
#define int16 int
#elif (SIZEOF_SHORT == 2)
#define int16 short
#endif
#endif

#ifndef uint32
#define uint32 unsigned int32
#endif

#ifndef uint16
#define uint16 unsigned int16
#endif

#ifndef MIN
#define MIN(a, b) ((a) < (b)? (a): (b))
#endif

#ifndef MAX
#define MAX(a, b) ((a) > (b)? (a): (b))
#endif

#if !HAVE_STRERROR
extern char *sys_errlist[];
#define strerror(i) sys_errlist[i]
#endif

#ifndef HAVE_ERRNO_H
extern int errno;
#endif

#define likely(x)	__builtin_expect(!!(x), 1)
#define unlikely(x)	__builtin_expect(!!(x), 0)

typedef long long int i64;
typedef uint16_t u16;
typedef uint32_t u32;

#ifndef MAP_ANONYMOUS
 #define MAP_ANONYMOUS MAP_ANON
#endif

#if defined(NOTHREAD) || !defined(_SC_NPROCESSORS_ONLN)
 #define PROCESSORS (1)
#else
 #define PROCESSORS (sysconf(_SC_NPROCESSORS_ONLN))
#endif

#ifdef _SC_PAGE_SIZE
 #define PAGE_SIZE (sysconf(_SC_PAGE_SIZE))
#else
 #define PAGE_SIZE (4096)
#endif

typedef struct rzip_control rzip_control;

void write_magic(rzip_control *control, int fd_in, int fd_out);
void read_magic(rzip_control *control, int fd_in, i64 *expected_size);
void preserve_perms(rzip_control *control, int fd_in, int fd_out);
int open_tmpoutfile(rzip_control *control);
void dump_tmpoutfile(rzip_control *control, int fd_out);
int open_tmpinfile(rzip_control *control);
void read_tmpinfile(rzip_control *control, int fd_in);
void decompress_file(rzip_control *control);
void get_header_info(rzip_control *control, int fd_in, uchar *ctype, i64 *c_len, i64 *u_len, i64 *last_head);
void get_fileinfo(rzip_control *control);
void compress_file(rzip_control *control);
#endif
