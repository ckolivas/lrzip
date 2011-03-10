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

#ifndef LRZIP_PRIV_H
#define LRZIP_PRIV_H

#define LRZIP_MAJOR_VERSION VMAJ
#define LRZIP_MINOR_VERSION VMIN
#define LRZIP_MINOR_SUBVERSION VMIC

#define NUM_STREAMS 2
#define STREAM_BUFSIZE (1024 * 1024 * 10)

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#ifdef HAVE_STRING_H
# include <string.h>
#endif

#ifdef HAVE_MALLOC_H
# include <malloc.h>
#endif

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

typedef struct rzip_control rzip_control;
typedef struct md5_ctx md5_ctx;

#if defined(__FreeBSD__) || defined(__FreeBSD_kernel__)
# define mremap fake_mremap
#endif

#define FLAG_SHOW_PROGRESS	(1 << 0)
#define FLAG_KEEP_FILES		(1 << 1)
#define FLAG_TEST_ONLY		(1 << 2)
#define FLAG_FORCE_REPLACE	(1 << 3)
#define FLAG_DECOMPRESS		(1 << 4)
#define FLAG_NO_COMPRESS	(1 << 5)
#define FLAG_LZO_COMPRESS	(1 << 6)
#define FLAG_BZIP2_COMPRESS	(1 << 7)
#define FLAG_ZLIB_COMPRESS	(1 << 8)
#define FLAG_ZPAQ_COMPRESS	(1 << 9)
#define FLAG_VERBOSITY		(1 << 10)
#define FLAG_VERBOSITY_MAX	(1 << 11)
#define FLAG_STDIN		(1 << 12)
#define FLAG_STDOUT		(1 << 13)
#define FLAG_INFO		(1 << 14)
#define FLAG_UNLIMITED		(1 << 15)
#define FLAG_HASH		(1 << 16)
#define FLAG_MD5		(1 << 17)
#define FLAG_CHECK		(1 << 18)
#define FLAG_KEEP_BROKEN	(1 << 19)
#define FLAG_THRESHOLD		(1 << 20)

#define NO_MD5		(!(HASH_CHECK) && !(HAS_MD5))

#define BITS32		(sizeof(long) == 4)

#define CTYPE_NONE 3
#define CTYPE_BZIP2 4
#define CTYPE_LZO 5
#define CTYPE_LZMA 6
#define CTYPE_GZIP 7
#define CTYPE_ZPAQ 8

/* Needs to be less than 31 bits and page aligned on 32 bits */
#define two_gig ((1ull << 31) - 4096)

#define print_err(format, args...)	do {\
	fprintf(stderr, format, ##args);	\
} while (0)


/* Structure to save state of computation between the single steps.  */
struct md5_ctx
{
  uint32_t A;
  uint32_t B;
  uint32_t C;
  uint32_t D;

  uint32_t total[2];
  uint32_t buflen;
  uint32_t buffer[32];
};

struct rzip_control {
	char *infile;
	char *outname;
	char *outfile;
	char *outdir;
	char *tmpdir; // when stdin, stdout, or test used
	FILE *msgout; //stream for output messages
	const char *suffix;
	int compression_level;
	i64 overhead; // compressor overhead
	i64 maxram; // the largest chunk of ram to allocate
	unsigned char lzma_properties[5]; // lzma properties, encoded
	i64 window;
	unsigned long flags;
	i64 ramsize;
	i64 max_chunk;
	i64 max_mmap;
	int threads;
	int nice_val;		// added for consistency
	int major_version;
	int minor_version;
	i64 st_size;
	long page_size;
	int fd_out;
	md5_ctx ctx;
	i64 md5_read; // How far into the file the md5 has done so far
};

struct stream {
	i64 last_head;
	uchar *buf;
	i64 buflen;
	i64 bufp;
	int eos;
	long uthread_no;
	long unext_thread;
	long base_thread;
	int total_threads;
};

struct stream_info {
	struct stream *s;
	int num_streams;
	int fd;
	i64 bufsize;
	i64 cur_pos;
	i64 initial_pos;
	i64 total_read;
	i64 ram_alloced;
	long thread_no;
	long next_thread;
	int chunks;
	char chunk_bytes;
};
#endif
