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

#ifndef RZIP_H
#define RZIP_H
#include "lrzip.h" /* includes config.h */
#include "liblrzip.h"
#include "md5.h"

#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <bzlib.h>
#include <zlib.h>
#include <pthread.h>
#include <sys/statvfs.h>
#include <sys/resource.h>
#include <netinet/in.h>

#include <sys/time.h>

#include <sys/mman.h>
#include <sys/syscall.h>

#include <lzo/lzoconf.h>
#include <lzo/lzo1x.h>

/* LZMA C Wrapper */
#include "lzma/C/LzmaLib.h"

#ifdef HAVE_STRING_H
#include <string.h>
#endif

#ifdef HAVE_MALLOC_H
#include <malloc.h>
#endif

#include <fcntl.h>
#include <sys/stat.h>

#ifdef HAVE_CTYPE_H
#include <ctype.h>
#endif
#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif
#include <sys/mman.h>

/* needed for CRC routines */
#include "lzma/C/7zCrc.h"

void fatal(const char *format, ...);
void failure(const char *format, ...);

void sighandler();
i64 runzip_fd(rzip_control *control, int fd_in, int fd_out, int fd_hist, i64 expected_size);
void rzip_fd(rzip_control *control, int fd_in, int fd_out);
void *open_stream_out(rzip_control *control, int f, int n, i64 limit, char cbytes);
void *open_stream_in(rzip_control *control, int f, int n);
int write_stream(rzip_control *control, void *ss, int stream, uchar *p, i64 len);
i64 read_stream(rzip_control *control, void *ss, int stream, uchar *p, i64 len);
int close_stream_out(rzip_control *control, void *ss);
int close_stream_in(void *ss);
void flush_buffer(rzip_control *control, struct stream_info *sinfo, int stream);
ssize_t write_1g(int fd, void *buf, i64 len);
ssize_t read_1g(int fd, void *buf, i64 len);
void zpipe_compress(FILE *in, FILE *out, FILE *msgout, long long int buf_len, int progress, long thread);
void zpipe_decompress(FILE *in, FILE *out, FILE *msgout, long long int buf_len, int progress, long thread);
const i64 two_gig;
void prepare_streamout_threads(rzip_control *control);
void close_streamout_threads(rzip_control *control);
void round_to_page(i64 *size);

void register_infile(const char *name, char delete);
void register_outfile(const char *name, char delete);
void register_outputfile(FILE *f);

#define print_err(format, args...)	do {\
	fprintf(stderr, format, ##args);	\
} while (0)

/* Macros for testing parameters */

#define isparameter( parmstring, value )	(!strcasecmp( parmstring, value ))
#define iscaseparameter( parmvalue, value )	(!strcmp( parmvalue, value ))

#endif
