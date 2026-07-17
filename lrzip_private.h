/*
   Copyright (C) 2006-2016,2018,2021-2022,2026 Con Kolivas
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

#include "config.h"

#define NUM_STREAMS 2
#define STREAM_BUFSIZE (1024 * 1024 * 10)

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdarg.h>
#include <semaphore.h>
#include <stdatomic.h>

/* lrzip requires a 64-bit platform (large windows, no 32-bit path left). */
#if defined(__SIZEOF_POINTER__) && (__SIZEOF_POINTER__ < 8)
# error "lrzip requires a 64-bit architecture"
#elif defined(UINTPTR_MAX) && (UINTPTR_MAX < 0xffffffffffffffffULL)
# error "lrzip requires a 64-bit architecture"
#endif
_Static_assert(sizeof(void *) >= 8, "lrzip requires a 64-bit architecture");
_Static_assert(sizeof(long) >= 8, "lrzip requires a 64-bit architecture");

#ifdef HAVE_PTHREAD_H
# include <pthread.h>
#endif

#ifdef HAVE_STRING_H
# include <string.h>
#endif

#ifdef HAVE_MALLOC_H
# include <malloc.h>
#endif

#ifdef HAVE_ALLOCA_H
# include <alloca.h>
#elif defined __GNUC__
# define alloca __builtin_alloca
#elif defined _AIX
# define alloca __alloca
#elif defined _MSC_VER
# include <malloc.h>
# define alloca _alloca
#else
# include <stddef.h>
# ifdef  __cplusplus
extern "C"
# endif
void *alloca (size_t);
#endif

#ifdef HAVE_ENDIAN_H
# include <endian.h>
#elif HAVE_SYS_ENDIAN_H
# include <sys/endian.h>
#endif
#ifndef __BYTE_ORDER
# ifndef __BIG_ENDIAN
#  define __BIG_ENDIAN	4321
#  define __LITTLE_ENDIAN	1234
# endif
# ifdef WORDS_BIGENDIAN
#  define __BYTE_ORDER __BIG_ENDIAN
# else
#  define __BYTE_ORDER __LITTLE_ENDIAN
# endif
#endif

#ifndef MD5_DIGEST_SIZE
# define MD5_DIGEST_SIZE 16
#endif

#define free(X) do { free((X)); (X) = NULL; } while (0)

#ifndef strdupa
# define strdupa(str) strcpy(alloca(strlen(str) + 1), str)
#endif

#ifndef strndupa
# define strndupa(str, len) strncpy(alloca(len + 1), str, len)
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

/* Largest count to pass to a single read()/write(). Linux truncates larger
 * requests but macOS fails them with EINVAL, so all IO loops must chunk. */
#define MAX_RW_COUNT (1L << 30)

#if !HAVE_STRERROR
extern char *sys_errlist[];
#define strerror(i) sys_errlist[i]
#endif

#ifndef HAVE_ERRNO_H
extern int errno;
#endif

#define likely(x)	__builtin_expect(!!(x), 1)
#define unlikely(x)	__builtin_expect(!!(x), 0)
#define __maybe_unused	__attribute__((unused))

#if defined(__MINGW32__) || defined(__CYGWIN__) || defined(__ANDROID__) || defined(__APPLE__) || defined(__OpenBSD__)
# define ffsll __builtin_ffsll
#endif

typedef int64_t i64;
typedef uint32_t u32;

typedef struct rzip_control rzip_control;
typedef struct md5_ctx md5_ctx;

/* ck specific unnamed semaphore implementations to cope with osx not
 * implementing them. */
#ifdef __APPLE__
struct cksem {
	int pipefd[2];
};

typedef struct cksem cksem_t;
#else
typedef sem_t cksem_t;
#endif

#if !defined(__linux)
 #define mremap fake_mremap
#endif

#define bswap_32(x) \
     ((((x) & 0xff000000) >> 24) | (((x) & 0x00ff0000) >>  8) |		      \
      (((x) & 0x0000ff00) <<  8) | (((x) & 0x000000ff) << 24))

# define bswap_64(x) \
     ((((x) & 0xff00000000000000ull) >> 56)				      \
      | (((x) & 0x00ff000000000000ull) >> 40)				      \
      | (((x) & 0x0000ff0000000000ull) >> 24)				      \
      | (((x) & 0x000000ff00000000ull) >> 8)				      \
      | (((x) & 0x00000000ff000000ull) << 8)				      \
      | (((x) & 0x0000000000ff0000ull) << 24)				      \
      | (((x) & 0x000000000000ff00ull) << 40)				      \
      | (((x) & 0x00000000000000ffull) << 56))

#ifdef leto32h
# define le32toh(x) leto32h(x)
# define le64toh(x) leto64h(x)
#endif

#ifndef le32toh
# if __BYTE_ORDER == __LITTLE_ENDIAN
#  define htole32(x) (x)
#  define le32toh(x) (x)
#  define htole64(x) (x)
#  define le64toh(x) (x)
# elif __BYTE_ORDER == __BIG_ENDIAN
#  define htole32(x) bswap_32 (x)
#  define le32toh(x) bswap_32 (x)
#  define htole64(x) bswap_64 (x)
#  define le64toh(x) bswap_64 (x)
#else
#error UNKNOWN BYTE ORDER
#endif
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
#define FLAG_TMP_OUTBUF		(1 << 21)
#define FLAG_TMP_INBUF		(1 << 22)
#define FLAG_ENCRYPT		(1 << 23)
#define FLAG_OUTPUT		(1 << 24)
/* Archive uses v0.7 streaming blocks (LRZC framing / progressive STDOUT) */
#define FLAG_STREAMING_BLOCKS	(1 << 25)
/* Writer: force 0.6-compatible AES-128-CBC (magic[22]=1) */
#define FLAG_ENCRYPT_LEGACY	(1 << 26)
/* Session uses AES-256-GCM suite (magic[22]=3) */
#define FLAG_ENCRYPT_AEAD	(1 << 27)
/* Maximum compression modifier: single block per stream, largest
 * dictionaries, 273 fast bytes. Sacrifices parallelism for ratio. */
#define FLAG_ULTRA		(1 << 28)

#define MAGIC_LEN	24
#define LRZC_LEN	24

/* Suite-3 AEAD (magic[22]=3) */
#define LRZ_AEAD_NONCE_LEN	12
#define LRZ_AEAD_TAG_LEN	16
#define LRZ_AEAD_KEY_LEN	32
#define LRZ_CRYPTO_DESC_LEN	32
#define LRZ_AEAD_SALT_LEN	16
#define LRZ_SUITE_AES256_GCM_PBKDF2	1
#define LRZ_PBKDF2_ITERS_DEFAULT	600000u
#define LRZ_PBKDF2_ITERS_MAX		5000000u

#define NO_MD5		(!(HASH_CHECK) && !(HAS_MD5))

#define CTYPE_NONE 3
#define CTYPE_BZIP2 4
#define CTYPE_LZO 5
#define CTYPE_LZMA 6
#define CTYPE_GZIP 7
#define CTYPE_ZPAQ 8
/* lzma with a reversible filter applied first: x86 or arm64 BCJ branch
 * conversion for executable code, or byte delta with distance 1-4 for
 * numeric/sampled data. Only written when --filter is used; the filter is
 * chosen per block by trial unless one is forced. */
#define CTYPE_LZMA_BCJ 9
#define CTYPE_LZMA_BCJ_ARM64 10
#define CTYPE_LZMA_DELTA1 11
#define CTYPE_LZMA_DELTA2 12
#define CTYPE_LZMA_DELTA3 13
#define CTYPE_LZMA_DELTA4 14

#define PASS_LEN 512
#define HASH_LEN 64
#define SALT_LEN 8
#define CBC_LEN 16

#ifndef PAGE_SIZE
# ifdef _SC_PAGE_SIZE
#  define PAGE_SIZE (sysconf(_SC_PAGE_SIZE))
# else
#  define PAGE_SIZE (4096)
# endif
#endif

#define dealloc(ptr) do { \
	free(ptr); \
	ptr = NULL; \
} while (0)

/* Determine how many times to hash the password when encrypting, based on
 * the date such that we increase the number of loops according to Moore's
 * law relative to when the data is encrypted. It is then stored as a two
 * byte value in the header */
#define MOORE 1.835          // world constant  [TIMES per YEAR]
#define ARBITRARY  1000000   // number of sha2 calls per one second in 2011
#define T_ZERO 1293840000    // seconds since epoch in 2011

#define SECONDS_IN_A_YEAR (365*86400)
#define MOORE_TIMES_PER_SECOND pow (MOORE, 1.0 / SECONDS_IN_A_YEAR)
#define ARBITRARY_AT_EPOCH (ARBITRARY * pow (MOORE_TIMES_PER_SECOND, -T_ZERO))

#define FLAG_VERBOSE (FLAG_VERBOSITY | FLAG_VERBOSITY_MAX)
#define FLAG_NOT_LZMA (FLAG_NO_COMPRESS | FLAG_LZO_COMPRESS | FLAG_BZIP2_COMPRESS | FLAG_ZLIB_COMPRESS | FLAG_ZPAQ_COMPRESS)
#define LZMA_COMPRESS	(!(control->flags & FLAG_NOT_LZMA))

#define SHOW_PROGRESS	(control->flags & FLAG_SHOW_PROGRESS)
#define KEEP_FILES	(control->flags & FLAG_KEEP_FILES)
#define TEST_ONLY	(control->flags & FLAG_TEST_ONLY)
#define FORCE_REPLACE	(control->flags & FLAG_FORCE_REPLACE)
#define DECOMPRESS	(control->flags & FLAG_DECOMPRESS)
#define NO_COMPRESS	(control->flags & FLAG_NO_COMPRESS)
#define LZO_COMPRESS	(control->flags & FLAG_LZO_COMPRESS)
#define BZIP2_COMPRESS	(control->flags & FLAG_BZIP2_COMPRESS)
#define ZLIB_COMPRESS	(control->flags & FLAG_ZLIB_COMPRESS)
#define ZPAQ_COMPRESS	(control->flags & FLAG_ZPAQ_COMPRESS)
#define ULTRA		(control->flags & FLAG_ULTRA)
#define VERBOSE		(control->flags & FLAG_VERBOSE)
#define VERBOSITY	(control->flags & FLAG_VERBOSITY)
#define MAX_VERBOSE	(control->flags & FLAG_VERBOSITY_MAX)
#define STDIN		(control->flags & FLAG_STDIN)
#define STDOUT		(control->flags & FLAG_STDOUT)
#define INFO		(control->flags & FLAG_INFO)
#define UNLIMITED	(control->flags & FLAG_UNLIMITED)
#define HASH_CHECK	(control->flags & FLAG_HASH)
#define HAS_MD5		(control->flags & FLAG_MD5)
#define CHECK_FILE	(control->flags & FLAG_CHECK)
#define KEEP_BROKEN	(control->flags & FLAG_KEEP_BROKEN)
#define LZ4_TEST	(control->flags & FLAG_THRESHOLD)
#define TMP_OUTBUF	(control->flags & FLAG_TMP_OUTBUF)
#define ENCRYPT_LEGACY	(control->flags & FLAG_ENCRYPT_LEGACY)
#define ENCRYPT_AEAD	(control->flags & FLAG_ENCRYPT_AEAD)
#define TMP_INBUF	(control->flags & FLAG_TMP_INBUF)
#define ENCRYPT		(control->flags & FLAG_ENCRYPT)
#define SHOW_OUTPUT	(control->flags & FLAG_OUTPUT)
#define STREAMING_BLOCKS (control->flags & FLAG_STREAMING_BLOCKS)

#define IS_FROM_FILE ( !!(control->inFILE) && !STDIN )


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

struct sliding_buffer {
	uchar *buf_low;	/* The low window buffer */
	uchar *buf_high;/* "" high "" */
	i64 orig_offset;/* Where the original buffer started */
	i64 offset_low;	/* What the current offset the low buffer has */
	i64 offset_high;/* "" high buffer "" */
	i64 offset_search;/* Where the search is up to */
	i64 orig_size;	/* How big the full buffer would be */
	i64 size_low;	/* How big the low buffer is */
	i64 size_high;	/* "" high "" */
	i64 high_length;/* How big the high buffer should be */
	int fd;		/* The fd of the mmap */
};

struct checksum {
	uchar *buf;
	i64 len;
	i64 capacity;
	int shutdown;
	int filling;	/* producer owns buf and is appending (decompress) */
};

/* Rolling hash tag: 32-bit is enough (hash_index is built from random()). */
typedef uint32_t tag;

struct node {
	void *data;
	struct node *prev;
};

struct runzip_node {
	struct stream_info *sinfo;
	pthread_t *pthreads;
	struct runzip_node *prev;
};

/* Compact hash slot: 8 bytes (was 16 with i64 tag + i64 offset).
 * offset is a 32-bit chunk position; chunks larger than 4GiB-1 use a
 * separate wide table (see rzip.c). */
struct hash_entry {
	uint32_t t;
	uint32_t offset;
};

struct hash_entry_wide {
	i64 offset;
	uint32_t t;
	uint32_t _pad;
};

struct rzip_state {
	void *ss;
	struct node *sslist;
	struct node *head;
	struct level *level;
	tag hash_index[256];
	void *hash_table;	/* hash_entry or hash_entry_wide */
	char hash_bits;
	char hash_wide;		/* non-zero: hash_entry_wide slots */
	i64 hash_count;
	i64 hash_limit;
	tag minimum_tag_mask;
	i64 tag_clean_ptr;
	i64 last_match;
	i64 chunk_size;
	i64 mmap_size;
	/* MD5 of this chunk was fed before a chunk filter converted the
	 * buffer, so hash_search must not feed it again. */
	bool chunk_md5_done;
	char chunk_bytes;
	char sliding;	/* non-zero: sliding mmap match path */
	int fd_in, fd_out;
	char stdin_eof;
	/* Batched writes to rzip control stream (stream 0). */
#define RZIP_S0_BUFSIZE 4096
	uchar s0_buf[RZIP_S0_BUFSIZE];
	unsigned s0_len;
	struct {
		i64 inserts;
		i64 literals;
		i64 literal_bytes;
		i64 matches;
		i64 match_bytes;
		i64 tag_hits;
		i64 tag_misses;
	} stats;
};

struct rzip_control {
	char *infile;
	FILE *inFILE; // if a FILE is being read from
	char *outname;
	char *outfile;
	FILE *outFILE; // if a FILE is being written to
	char *outdir;
	char *tmpdir; // when stdin, stdout, or test used
	uchar *tmp_outbuf; // Temporary file storage for stdout
	i64 out_ofs; // Output offset when tmp_outbuf in use
	i64 hist_ofs; // History offset
	i64 out_len; // Total length of tmp_outbuf
	i64 out_maxlen; // The largest the tmp_outbuf can be used
	i64 out_relofs; // Relative tmp_outbuf offset when stdout has been flushed
	uchar *tmp_inbuf;
	i64 in_ofs;
	i64 in_len;
	i64 in_maxlen;
	FILE *msgout; //stream for output messages
	FILE *msgerr; //stream for output errors
	char *suffix;
	uchar compression_level;
	i64 overhead; // compressor overhead
	i64 usable_ram; // the most ram we'll try to use on one activity
	i64 maxram; // the largest chunk of ram to allocate
	unsigned char lzma_properties[5]; // lzma properties, encoded
	u32 lzma_dictsize; // lzma dictionary size, sized to ram and level
	/* --filter: 0 off, -1 per block trial selection, else the forced
	 * LRZ_FILTER_* kind. Backend blocks record their filter in the
	 * block type byte. */
	int filter_mode;
	i64 window;
	unsigned long flags;
	i64 ramsize;
	i64 max_chunk;
	i64 max_mmap;
	int threads;
	char nice_val;		// added for consistency
	int current_priority;
	char major_version;
	char minor_version;
	i64 st_size;
	long page_size;
	int fd_in;
	int fd_out;
	int fd_hist;
	i64 encloops;
	i64 secs;
	void (*pass_cb)(void *, char *, size_t); /* callback to get password in lib */
	void *pass_data;
	uchar salt[SALT_LEN];
	/* Suite-3: full 16-byte salt + PBKDF2 params (CryptoDesc) */
	uchar aead_salt[LRZ_AEAD_SALT_LEN];
	unsigned int aead_iters;
	uchar aead_key_hdr[LRZ_AEAD_KEY_LEN];
	uchar aead_key_data[LRZ_AEAD_KEY_LEN];
	/* Nonce uniqueness: random prefix + counters */
	uchar aead_nonce_prefix[4];
	uint64_t aead_hdr_seq;
	uint64_t aead_data_seq;
	uchar *salt_pass;
	int salt_pass_len;
	uchar *hash;
	char *passphrase;

	pthread_mutex_t control_lock;
	unsigned char eof;
	unsigned char magic_written;
	/* v0.7: number of completed streaming blocks written/read */
	unsigned int blocks_done;
	/* v0.7: absolute output position of current LRZC header (for c_size patch) */
	i64 lrzc_pos;
	/* v0.7: magic[23] / LRZC last-block as read from headers */
	unsigned char last_block;
	/* Frame limits for current/next RCD (from LRZC c_size; 0 = unknown) */
	i64 block_c_size;
	i64 next_block_c_size;
	i64 rcd_start;
	bool lzma_prop_set;
	/* LZ4 prefilter: test first backend block only, reuse for the rest */
	bool lz4_test_done;
	bool lz4_compressible;

	cksem_t cksumsem;	/* MD5 producer: buffer free */
	cksem_t cksum_worksem;	/* MD5 worker: job ready */
	pthread_t md5_thread;
	md5_ctx ctx;
	uchar md5_resblock[MD5_DIGEST_SIZE];
	i64 md5_read; // How far into the file the md5 has done so far
	struct checksum checksum;
	/* Grow-only scratch for runzip literal/match tokens */
	uchar *runzip_buf;
	i64 runzip_buf_len;

	const char *util_infile;
	char delete_infile;
	const char *util_outfile;
	char delete_outfile;
	FILE *outputfile;
	int log_level;
	void (*info_cb)(void *data, int pct, int chunk_pct);
	void *info_data;
	void (*log_cb)(void *data, unsigned int level, unsigned int line, const char *file, const char *func, const char *format, va_list args);
	void *log_data;

	char chunk_bytes;
	/* v0.8: LRZ_FILTER_* branch converter applied to the whole current
	 * chunk before rzip (compress) / to reverse after reconstruction
	 * (decompress). */
	char chunk_filter;
	struct sliding_buffer sb;
	void (*do_mcpy)(rzip_control *, unsigned char *, i64, i64);

	pthread_t *pthreads;
	struct runzip_node *ruhead;
	atomic_int thread_count;
};

struct uncomp_thread {
	uchar *s_buf;
	i64 u_len, c_len;
	i64 m_alloced;	/* bytes counted in stream_info.ram_alloced */
	i64 last_head;
	uchar c_type;
	int busy;
	int streamno;
};

struct stream {
	i64 last_head;
	uchar *buf;
	i64 buflen;
	i64 bufp;
	uchar eos;
	long uthread_no;
	long unext_thread;
	long base_thread;
	int total_threads;
	i64 last_headofs;
};

struct stream_info {
	struct stream *s;
	uchar num_streams;
	int fd;
	i64 bufsize;
	i64 cur_pos;
	i64 initial_pos;
	i64 total_read;
	i64 ram_alloced;
	i64 size;
	/* Absolute end of this RCD payload (0 = unknown); last_head/total_read bound */
	i64 payload_end;
	/* Absolute archive size for last_head checks when known (0 = unknown) */
	i64 infile_size;
	struct uncomp_thread *ucthreads;
	long thread_no;
	long next_thread;
	int chunks;
	char chunk_bytes;
	char chunk_filter;
};

static inline void __attribute__((format(printf, 2, 3))) print_stuff(const rzip_control *control, const char *format, ...)
{
	va_list ap;
	if (control->msgout) {
		va_start(ap, format);
		vfprintf(control->msgout, format, ap);
		va_end(ap);
		fflush(control->msgout);
	}
}

static inline void __attribute__((format(printf, 2, 3))) print_err(const rzip_control *control, const char *format, ...)
{
	va_list ap;
	if (control->msgerr) {
		va_start(ap, format);
		vfprintf(control->msgerr, format, ap);
		va_end(ap);
		fflush(control->msgerr);
	}
}

#define print_stuff(...) do {\
	print_stuff(control, __VA_ARGS__); \
} while (0)

#define print_output(...)	do {\
	if (SHOW_OUTPUT)	\
	print_stuff(__VA_ARGS__); \
} while (0)

#define print_progress(...)	do {\
	if (SHOW_PROGRESS)	\
		print_stuff(__VA_ARGS__); \
} while (0)

#define print_verbose(...)	do {\
	if (VERBOSE)	\
		print_stuff(__VA_ARGS__); \
} while (0)

#define print_maxverbose(...)	do {\
	if (MAX_VERBOSE)	\
		print_stuff(__VA_ARGS__); \
} while (0)

#define print_err(...) do {\
	print_err(control, __VA_ARGS__); \
} while (0)
#endif
