/*
   Copyright (C) Andrew Tridgell 1998,
   Con Kolivas 2006-2010

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#define LRZIP_MAJOR_VERSION 0
#define LRZIP_MINOR_VERSION 5
#define LRZIP_MINOR_SUBVERSION 0

#define NUM_STREAMS 2

#include "config.h"

#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/resource.h>
#include <netinet/in.h>

#include <sys/time.h>

#include <sys/mman.h>
#include <sys/syscall.h>

#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

#include <lzo/lzoconf.h>
#include <lzo/lzo1x.h>

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
#include <errno.h>
#include <sys/mman.h>

/* needed for CRC routines */
#include "lzma/C/7zCrc.h"

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

#ifndef HAVE_ERRNO_DECL
extern int errno;
#endif

typedef long long int i64;
typedef uint16_t u16;
typedef uint32_t u32;

#define FLAG_SHOW_PROGRESS 2
#define FLAG_KEEP_FILES 4
#define FLAG_TEST_ONLY 8
#define FLAG_FORCE_REPLACE 16
#define FLAG_DECOMPRESS 32
#define FLAG_NO_COMPRESS 64
#define FLAG_LZO_COMPRESS 128
#define FLAG_BZIP2_COMPRESS 256
#define FLAG_ZLIB_COMPRESS 512
#define FLAG_ZPAQ_COMPRESS 1024
#define FLAG_VERBOSITY 2048
#define FLAG_VERBOSITY_MAX 4096
#define FLAG_NO_SET_PERMS 8192
#define FLAG_STDIN 16384
#define FLAG_STDOUT 32768
#define FLAG_INFO 65536

#define FLAG_VERBOSE (FLAG_VERBOSITY | FLAG_VERBOSITY_MAX)
#define FLAG_NOT_LZMA (FLAG_NO_COMPRESS | FLAG_LZO_COMPRESS | FLAG_BZIP2_COMPRESS | FLAG_ZLIB_COMPRESS | FLAG_ZPAQ_COMPRESS)
#define LZMA_COMPRESS(C)	(!((C) & FLAG_NOT_LZMA))

#define CTYPE_NONE 3
#define CTYPE_BZIP2 4
#define CTYPE_LZO 5
#define CTYPE_LZMA 6
#define CTYPE_GZIP 7
#define CTYPE_ZPAQ 8

struct rzip_control {
	char *infile;
	char *outname;
	char *outfile;
	char *outdir;
	FILE *msgout; //stream for output messages
	const char *suffix;
	int compression_level;
	unsigned char lzma_properties[5]; // lzma properties, encoded
	double threshold;
	unsigned long long window;
	unsigned long flags;
	unsigned long long ramsize;
	unsigned long threads;
	int nice_val;		// added for consistency
	int major_version;
	int minor_version;
};

extern struct rzip_control control;

void fatal(const char *format, ...);
void sighandler();
void err_msg(const char *format, ...);
i64 runzip_fd(int fd_in, int fd_out, int fd_hist, i64 expected_size);
void rzip_fd(int fd_in, int fd_out);
void *open_stream_out(int f, int n, i64 limit);
void *open_stream_in(int f, int n);
int write_stream(void *ss, int stream, uchar *p, i64 len);
i64 read_stream(void *ss, int stream, uchar *p, i64 len);
int close_stream_out(void *ss);
int close_stream_in(void *ss);
void read_config(struct rzip_control *s);
ssize_t write_1g(int fd, void *buf, i64 len);
ssize_t read_1g(int fd, void *buf, i64 len);
extern void zpipe_compress(FILE *in, FILE *out, FILE *msgout, long long int buf_len, int progress);
extern void zpipe_decompress(FILE *in, FILE *out, FILE *msgout, long long int buf_len, int progress);
