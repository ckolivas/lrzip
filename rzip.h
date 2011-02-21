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

#define LRZIP_MAJOR_VERSION 0
#define LRZIP_MINOR_VERSION 5
#define LRZIP_MINOR_SUBVERSION 60

#define NUM_STREAMS 2
#define STREAM_BUFSIZE (1024 * 1024 * 10)

#include "config.h"
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

void fatal(const char *format, ...);
void failure(const char *format, ...);

#ifdef __APPLE__
 #include <sys/sysctl.h>
 #define fmemopen fake_fmemopen
 #define open_memstream fake_open_memstream
 #define memstream_update_buffer fake_open_memstream_update_buffer
 #define mremap fake_mremap
static inline i64 get_ram(void)
{
	int mib[2];
	size_t len;
	i64 *p, ramsize;

	mib[0] = CTL_HW;
	mib[1] = HW_MEMSIZE;
	sysctl(mib, 2, NULL, &len, NULL, 0);
	p = malloc(len);
	sysctl(mib, 2, p, &len, NULL, 0);
	ramsize = *p;

	return ramsize;
}
#else /* __APPLE__ */
 #define memstream_update_buffer(A, B, C) (0)
static inline i64 get_ram(void)
{
	i64 ramsize;
	FILE *meminfo;
	char aux[256];
	char *ignore;

	ramsize = (i64)sysconf(_SC_PHYS_PAGES) * PAGE_SIZE;
	if (ramsize > 0)
		return ramsize;

	/* Workaround for uclibc which doesn't properly support sysconf */
	if(!(meminfo = fopen("/proc/meminfo", "r")))
		fatal("fopen\n");

	while(!feof(meminfo) && !fscanf(meminfo, "MemTotal: %Lu kB", &ramsize))
		ignore = fgets(aux, sizeof(aux), meminfo);
	if (fclose(meminfo) == -1)
		fatal("fclose");
	ramsize *= 1000;

	return ramsize;
}
#endif

#if defined(__FreeBSD__) || defined(__FreeBSD_kernel__)
 #define mremap fake_mremap
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
#define FLAG_MAXRAM		(1 << 15)
#define FLAG_UNLIMITED		(1 << 16)
#define FLAG_HASH		(1 << 17)
#define FLAG_MD5		(1 << 18)
#define FLAG_CHECK		(1 << 19)
#define FLAG_KEEP_BROKEN	(1 << 20)

#define FLAG_VERBOSE (FLAG_VERBOSITY | FLAG_VERBOSITY_MAX)
#define FLAG_NOT_LZMA (FLAG_NO_COMPRESS | FLAG_LZO_COMPRESS | FLAG_BZIP2_COMPRESS | FLAG_ZLIB_COMPRESS | FLAG_ZPAQ_COMPRESS)
#define LZMA_COMPRESS	(!(control.flags & FLAG_NOT_LZMA))

#define SHOW_PROGRESS	(control.flags & FLAG_SHOW_PROGRESS)
#define KEEP_FILES	(control.flags & FLAG_KEEP_FILES)
#define TEST_ONLY	(control.flags & FLAG_TEST_ONLY)
#define FORCE_REPLACE	(control.flags & FLAG_FORCE_REPLACE)
#define DECOMPRESS	(control.flags & FLAG_DECOMPRESS)
#define NO_COMPRESS	(control.flags & FLAG_NO_COMPRESS)
#define LZO_COMPRESS	(control.flags & FLAG_LZO_COMPRESS)
#define BZIP2_COMPRESS	(control.flags & FLAG_BZIP2_COMPRESS)
#define ZLIB_COMPRESS	(control.flags & FLAG_ZLIB_COMPRESS)
#define ZPAQ_COMPRESS	(control.flags & FLAG_ZPAQ_COMPRESS)
#define VERBOSE		(control.flags & FLAG_VERBOSE)
#define VERBOSITY	(control.flags & FLAG_VERBOSITY)
#define MAX_VERBOSE	(control.flags & FLAG_VERBOSITY_MAX)
#define STDIN		(control.flags & FLAG_STDIN)
#define STDOUT		(control.flags & FLAG_STDOUT)
#define INFO		(control.flags & FLAG_INFO)
#define MAXRAM		(control.flags & FLAG_MAXRAM)
#define UNLIMITED	(control.flags & FLAG_UNLIMITED)
#define HASH_CHECK	(control.flags & FLAG_HASH)
#define HAS_MD5		(control.flags & FLAG_MD5)
#define CHECK_FILE	(control.flags & FLAG_CHECK)
#define KEEP_BROKEN	(control.flags & FLAG_KEEP_BROKEN)

#define NO_MD5		(!(HASH_CHECK) && !(HAS_MD5))

#define BITS32		(sizeof(long) == 4)

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
	char *tmpdir; // when stdin, stdout, or test used
	FILE *msgout; //stream for output messages
	const char *suffix;
	int compression_level;
	unsigned char lzma_properties[5]; // lzma properties, encoded
	double threshold;
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
	struct md5_ctx ctx;
	i64 md5_read; // How far into the file the md5 has done so far 
} control;

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
	i64 max_bufsize;
	i64 cur_pos;
	i64 initial_pos;
	i64 total_read;
	i64 ram_alloced;
	long thread_no;
	long next_thread;
	int chunks;
	char chunk_bytes;
};

void sighandler();
i64 runzip_fd(int fd_in, int fd_out, int fd_hist, i64 expected_size);
void rzip_fd(int fd_in, int fd_out);
void *open_stream_out(int f, int n, i64 limit, char cbytes);
void *open_stream_in(int f, int n);
int write_stream(void *ss, int stream, uchar *p, i64 len);
i64 read_stream(void *ss, int stream, uchar *p, i64 len);
int close_stream_out(void *ss);
int close_stream_in(void *ss);
void flush_buffer(struct stream_info *sinfo, int stream);
void read_config(struct rzip_control *s);
ssize_t write_1g(int fd, void *buf, i64 len);
ssize_t read_1g(int fd, void *buf, i64 len);
void zpipe_compress(FILE *in, FILE *out, FILE *msgout, long long int buf_len, int progress, long thread);
void zpipe_decompress(FILE *in, FILE *out, FILE *msgout, long long int buf_len, int progress, long thread);
const i64 two_gig;
void prepare_streamout_threads(void);
void close_streamout_threads(void);
void round_to_page(i64 *size);

#define print_err(format, args...)	do {\
	fprintf(stderr, format, ##args);	\
} while (0)

#define print_output(format, args...)	do {\
	fprintf(control.msgout, format, ##args);	\
	fflush(control.msgout);	\
} while (0)

#define print_progress(format, args...)	do {\
	if (SHOW_PROGRESS)	\
		print_output(format, ##args);	\
} while (0)

#define print_verbose(format, args...)	do {\
	if (VERBOSE)	\
		print_output(format, ##args);	\
} while (0)

#define print_maxverbose(format, args...)	do {\
	if (MAX_VERBOSE)	\
		print_output(format, ##args);	\
} while (0)
