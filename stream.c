/*
   Copyright (C) 2006-2016,2018,2021-2022,2026 Con Kolivas
   Copyright (C) 2011 Serge Belyshev
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
/* multiplex N streams into a file - the streams are passed
   through different compressors */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#ifdef HAVE_SYS_TIME_H
# include <sys/time.h>
#endif
#ifdef HAVE_SYS_TYPES_H
# include <sys/types.h>
#endif
#ifdef HAVE_SYS_RESOURCE_H
# include <sys/resource.h>
#endif
#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <pthread.h>
#include <bzlib.h>
#include <zlib.h>
#include <lzo/lzoconf.h>
#include <lzo/lzo1x.h>
#include <lz4.h>
#ifdef HAVE_ERRNO_H
# include <errno.h>
#endif
#ifdef HAVE_ENDIAN_H
# include <endian.h>
#elif HAVE_SYS_ENDIAN_H
# include <sys/endian.h>
#endif
#ifdef HAVE_ARPA_INET_H
# include <arpa/inet.h>
#endif

#include <inttypes.h>
#include <stdatomic.h>
#include <string.h>

/* LZMA C Wrapper */
#include "lzma/C/LzmaLib.h"

#include "util.h"
#include "lrzip_core.h"
#include "filters.h"

#define STREAM_BUFSIZE (1024 * 1024 * 10)

static struct compress_thread {
	uchar *s_buf;	/* Uncompressed buffer -> Compressed buffer */
	uchar c_type;	/* Compression type */
	i64 s_len;	/* Data length uncompressed */
	i64 c_len;	/* Data length compressed */
	cksem_t cksem;  /* This thread's semaphore */
	struct stream_info *sinfo;
	int streamno;
	uchar salt[SALT_LEN];
} *cthreads;

typedef struct stream_thread_struct {
	int i;
	rzip_control *control;
	struct stream_info *sinfo;
} stream_thread_struct;

static long output_thread;
static pthread_mutex_t output_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t output_cond = PTHREAD_COND_INITIALIZER;

bool init_mutex(rzip_control *control, pthread_mutex_t *mutex)
{
	if (unlikely(pthread_mutex_init(mutex, NULL)))
		fatal_return(("Failed to pthread_mutex_init\n"), false);
	return true;
}

bool unlock_mutex(rzip_control *control, pthread_mutex_t *mutex)
{
	if (unlikely(pthread_mutex_unlock(mutex)))
		fatal_return(("Failed to pthread_mutex_unlock\n"), false);
	return true;
}

bool lock_mutex(rzip_control *control, pthread_mutex_t *mutex)
{
	if (unlikely(pthread_mutex_lock(mutex)))
		fatal_return(("Failed to pthread_mutex_lock\n"), false);
	return true;
}

static bool cond_wait(rzip_control *control, pthread_cond_t *cond, pthread_mutex_t *mutex)
{
	if (unlikely(pthread_cond_wait(cond, mutex)))
		fatal_return(("Failed to pthread_cond_wait\n"), false);
	return true;
}

static bool cond_broadcast(rzip_control *control, pthread_cond_t *cond)
{
	if (unlikely(pthread_cond_broadcast(cond)))
		fatal_return(("Failed to pthread_cond_broadcast\n"), false);
	return true;
}

bool create_pthread(rzip_control *control, pthread_t *thread, pthread_attr_t * attr,
	void * (*start_routine)(void *), void *arg)
{
	atomic_fetch_add(&control->thread_count, 1);
	if (unlikely(pthread_create(thread, attr, start_routine, arg))) {
		atomic_fetch_sub(&control->thread_count, 1);
		fatal_return(("Failed to pthread_create\n"), false);
	}
	return true;
}

bool detach_pthread(rzip_control *control, pthread_t *thread)
{
	if (unlikely(pthread_detach(*thread)))
		fatal_return(("Failed to pthread_detach\n"), false);
	return true;
}

bool join_pthread(rzip_control *control, pthread_t th, void **thread_return)
{
	if (pthread_join(th, thread_return))
		fatal_return(("Failed to pthread_join\n"), false);
	atomic_fetch_sub(&control->thread_count, 1);
	return true;
}

/* just to keep things clean, declare function here
 * but move body to the end since it's a work function
*/
static int lz4_compresses(rzip_control *control, uchar *s_buf, i64 s_len);

/*
  ***** COMPRESSION FUNCTIONS *****

  ZPAQ, BZIP, GZIP, LZMA, LZO

  try to compress a buffer. If compression fails for whatever reason then
  leave uncompressed. Return the compression type in c_type and resulting
  length in c_len
*/

static int zpaq_compress_buf(rzip_control *control, struct compress_thread *cthread, long thread)
{
	i64 c_len, c_size;
	uchar *c_buf;

	if (!lz4_compresses(control, cthread->s_buf, cthread->s_len))
		return 0;

	/* zpaq needs even more ram than other algorithms for relatively
	 * incompressible data. */
	c_size = round_up_page(control, (cthread->s_len + 10000) * 1.02);
	c_buf = malloc(c_size);
	if (!c_buf) {
		print_err("Unable to allocate c_buf in zpaq_compress_buf\n");
		return -1;
	}

	c_len = 0;

	zpaq_compress(c_buf, &c_len, cthread->s_buf, cthread->s_len, control->compression_level / 4 + 1,
		      control->msgout, SHOW_PROGRESS ? true: false, thread);

	if (unlikely(c_len >= cthread->c_len)) {
		print_maxverbose("Incompressible block\n");
		/* Incompressible, leave as CTYPE_NONE */
		dealloc(c_buf);
		return 0;
	}

	cthread->c_len = c_len;
	dealloc(cthread->s_buf);
	cthread->s_buf = c_buf;
	cthread->c_type = CTYPE_ZPAQ;
	return 0;
}

static int bzip2_compress_buf(rzip_control *control, struct compress_thread *cthread)
{
	u32 dlen = round_up_page(control, cthread->s_len);
	int bzip2_ret;
	uchar *c_buf;

	if (!lz4_compresses(control, cthread->s_buf, cthread->s_len))
		return 0;

	c_buf = malloc(dlen);
	if (!c_buf) {
		print_err("Unable to allocate c_buf in bzip2_compress_buf\n");
		return -1;
	}

	bzip2_ret = BZ2_bzBuffToBuffCompress((char *)c_buf, &dlen,
		(char *)cthread->s_buf, cthread->s_len,
		control->compression_level, 0, control->compression_level * 10);

	/* if compressed data is bigger then original data leave as
	 * CTYPE_NONE */

	if (bzip2_ret == BZ_OUTBUFF_FULL) {
		print_maxverbose("Incompressible block\n");
		/* Incompressible, leave as CTYPE_NONE */
		dealloc(c_buf);
		return 0;
	}

	if (unlikely(bzip2_ret != BZ_OK)) {
		dealloc(c_buf);
		print_maxverbose("BZ2 compress failed\n");
		return -1;
	}

	if (unlikely(dlen >= cthread->c_len)) {
		print_maxverbose("Incompressible block\n");
		/* Incompressible, leave as CTYPE_NONE */
		dealloc(c_buf);
		return 0;
	}

	cthread->c_len = dlen;
	dealloc(cthread->s_buf);
	cthread->s_buf = c_buf;
	cthread->c_type = CTYPE_BZIP2;
	return 0;
}

static int gzip_compress_buf(rzip_control *control, struct compress_thread *cthread)
{
	unsigned long dlen = round_up_page(control, cthread->s_len);
	uchar *c_buf;
	int gzip_ret;

	c_buf = malloc(dlen);
	if (!c_buf) {
		print_err("Unable to allocate c_buf in gzip_compress_buf\n");
		return -1;
	}

	gzip_ret = compress2(c_buf, &dlen, cthread->s_buf, cthread->s_len,
		control->compression_level);

	/* if compressed data is bigger then original data leave as
	 * CTYPE_NONE */

	if (gzip_ret == Z_BUF_ERROR) {
		print_maxverbose("Incompressible block\n");
		/* Incompressible, leave as CTYPE_NONE */
		dealloc(c_buf);
		return 0;
	}

	if (unlikely(gzip_ret != Z_OK)) {
		dealloc(c_buf);
		print_maxverbose("compress2 failed\n");
		return -1;
	}

	if (unlikely((i64)dlen >= cthread->c_len)) {
		print_maxverbose("Incompressible block\n");
		/* Incompressible, leave as CTYPE_NONE */
		dealloc(c_buf);
		return 0;
	}

	cthread->c_len = dlen;
	dealloc(cthread->s_buf);
	cthread->s_buf = c_buf;
	cthread->c_type = CTYPE_GZIP;
	return 0;
}

/* Map a block ctype back to its filter kind, or LRZ_FILTER_NONE */
static int ctype_filter_kind(int ctype)
{
	if (ctype >= CTYPE_LZMA_BCJ && ctype <= CTYPE_LZMA_DELTA4)
		return LRZ_FILTER_X86 + ctype - CTYPE_LZMA_BCJ;
	return LRZ_FILTER_NONE;
}

static int lzma_compress_buf(rzip_control *control, struct compress_thread *cthread)
{
	unsigned char lzma_properties[5]; /* lzma properties, encoded */
	int lzma_level, lzma_fb, lzma_ret;
	size_t prop_size = 5; /* return value for lzma_properties */
	u32 dictsize;
	int filter;
	uchar *c_buf;
	size_t dlen;

	if (!lz4_compresses(control, cthread->s_buf, cthread->s_len))
		return 0;

	/* Convert in place with the chosen prefilter for this block, if any;
	 * incompressible or failed blocks are converted back before
	 * returning so stored bytes are always the original ones. Chunks
	 * already branch converted before rzip stay as they are: the block
	 * layer stands down for them. */
	filter = cthread->sinfo->chunk_filter != LRZ_FILTER_NONE ? LRZ_FILTER_NONE :
		 lrz_stream_filter_pick(control, cthread->s_buf, cthread->s_len);
	if (filter != LRZ_FILTER_NONE)
		lrz_filter_convert_mem(cthread->s_buf, cthread->s_len, filter, true);

	/* only 7 levels with lzma, scale them. --ultra instead uses the lzma
	 * level scale directly with an explicit large dictionary, and 273
	 * fast bytes like xz -e for maximum ratio. */
	if (ULTRA) {
		lzma_level = control->compression_level;
		if (!lzma_level)
			lzma_level = 1;
		else if (lzma_level > 9)
			lzma_level = 9;
		lzma_fb = 273;
	} else {
		lzma_level = control->compression_level * 7 / 9;
		if (!lzma_level)
			lzma_level = 1;
		lzma_fb = -1;
	}

	print_maxverbose("Starting lzma back end compression thread...\n");
retry:
	/* The dictionary size is shared so that every block is encoded with
	 * the same properties; a low memory retry shrinks it for all
	 * outstanding work rather than just this block. */
	lock_mutex(control, &control->control_lock);
	dictsize = control->lzma_dictsize;
	unlock_mutex(control, &control->control_lock);
	dlen = round_up_page(control, cthread->s_len);
	c_buf = malloc(dlen);
	if (!c_buf) {
		print_err("Unable to allocate c_buf in lzma_compress_buf\n");
		goto restore_filter_fail;
	}

	/* LZMA SDK 26.02 LzmaCompress: level + threads; props returned in
	 * lzma_properties (5 bytes). Default dict size per level is larger
	 * than the old 4.63/9.x tables. */

	lzma_ret = LzmaCompress(c_buf, &dlen, cthread->s_buf,
		(size_t)cthread->s_len, lzma_properties, &prop_size,
				lzma_level,
				dictsize, /* dict size scaled to level and ram */
				-1, -1, -1, lzma_fb, /* lc, lp, pb, fb */
				control->threads > 1 || ULTRA ? 2 : 1);
				/* ultra packs whole streams into single blocks, so
				 * keep the encoder's match finder thread. */
				/* LZMA spec has threads = 1 or 2 only. */
	if (lzma_ret != SZ_OK) {
		switch (lzma_ret) {
			case SZ_ERROR_MEM:
				break;
			case SZ_ERROR_PARAM:
				print_err("LZMA Parameter ERROR: %d. This should not happen.\n", SZ_ERROR_PARAM);
				break;
			case SZ_ERROR_OUTPUT_EOF:
				print_maxverbose("Harmless LZMA Output Buffer Overflow error: %d. Incompressible block.\n", SZ_ERROR_OUTPUT_EOF);
				break;
			case SZ_ERROR_THREAD:
				print_err("LZMA Multi Thread ERROR: %d. This should not happen.\n", SZ_ERROR_THREAD);
				break;
			default:
				print_err("Unidentified LZMA ERROR: %d. This should not happen.\n", lzma_ret);
				break;
		}
		/* can pass -1 if not compressible! Thanks Lasse Collin */
		dealloc(c_buf);
		if (lzma_ret == SZ_ERROR_MEM) {
			if (dictsize > (1 << 20)) {
				/* Shrink the shared dictionary so all blocks
				 * keep identical properties. */
				lock_mutex(control, &control->control_lock);
				if (control->lzma_dictsize >= dictsize)
					control->lzma_dictsize = dictsize >> 1;
				unlock_mutex(control, &control->control_lock);
				print_verbose("LZMA Warning: %d. Can't allocate enough RAM for compression window, trying smaller.\n", SZ_ERROR_MEM);
				goto retry;
			}
			if (lzma_level > 1) {
				lzma_level--;
				print_verbose("LZMA Warning: %d. Can't allocate enough RAM for compression window, trying smaller.\n", SZ_ERROR_MEM);
				goto retry;
			}
			/* If lzma cannot allocate any dictionary, fall back to
			 * bzip2 so the block does not remain uncompressed. */
			print_verbose("Unable to allocate enough RAM for any sized compression window, falling back to bzip2 compression.\n");
			if (filter != LRZ_FILTER_NONE)
				lrz_filter_convert_mem(cthread->s_buf, cthread->s_len, filter, false);
			return bzip2_compress_buf(control, cthread);
		} else if (lzma_ret == SZ_ERROR_OUTPUT_EOF)
			goto restore_filter_ok;
		goto restore_filter_fail;
	}

	if (unlikely((i64)dlen >= cthread->c_len)) {
		/* Incompressible, leave as CTYPE_NONE */
		print_maxverbose("Incompressible block\n");
		dealloc(c_buf);
		goto restore_filter_ok;
	}

	/* Make sure multiple threads don't race on writing lzma_properties.
	 * If a low memory retry gave some block a smaller dictionary, keep
	 * the largest so the decoder allocates enough window for every
	 * block. */
	lock_mutex(control, &control->control_lock);
	if (control->lzma_prop_set) {
		/* dict size is bytes 1-4 of the properties, little endian */
		u32 stored_dict = control->lzma_properties[1] | control->lzma_properties[2] << 8 |
			control->lzma_properties[3] << 16 | (u32)control->lzma_properties[4] << 24;
		u32 this_dict = lzma_properties[1] | lzma_properties[2] << 8 |
			lzma_properties[3] << 16 | (u32)lzma_properties[4] << 24;

		if (this_dict > stored_dict)
			memcpy(control->lzma_properties, lzma_properties, 5);
	}
	if (!control->lzma_prop_set) {
		memcpy(control->lzma_properties, lzma_properties, 5);
		control->lzma_prop_set = true;
		/* Reset the magic written flag so we write it again if we
		 * get lzma properties and haven't written them yet. Do not
		 * rewrite after the first streaming block has been flushed. */
		if (TMP_OUTBUF && !control->blocks_done)
			control->magic_written = 0;
	}
	unlock_mutex(control, &control->control_lock);

	cthread->c_len = dlen;
	dealloc(cthread->s_buf);
	cthread->s_buf = c_buf;
	cthread->c_type = filter == LRZ_FILTER_NONE ? CTYPE_LZMA :
		CTYPE_LZMA_BCJ + filter - LRZ_FILTER_X86;
	return 0;

restore_filter_ok:
	/* Blocks left uncompressed must hold the original bytes, so undo
	 * any in place filter conversion. */
	if (filter != LRZ_FILTER_NONE)
		lrz_filter_convert_mem(cthread->s_buf, cthread->s_len, filter, false);
	return 0;

restore_filter_fail:
	if (filter != LRZ_FILTER_NONE)
		lrz_filter_convert_mem(cthread->s_buf, cthread->s_len, filter, false);
	return -1;
}

static int lzo_compress_buf(rzip_control *control, struct compress_thread *cthread)
{
	lzo_uint in_len = cthread->s_len;
	lzo_uint dlen = round_up_page(control, in_len + in_len / 16 + 64 + 3);
	lzo_bytep wrkmem;
	uchar *c_buf;
	int ret = -1;

	wrkmem = (lzo_bytep) calloc(1, LZO1X_1_MEM_COMPRESS);
	if (unlikely(wrkmem == NULL)) {
		print_maxverbose("Failed to malloc wkmem\n");
		return ret;
	}

	c_buf = malloc(dlen);
	if (!c_buf) {
		print_err("Unable to allocate c_buf in lzo_compress_buf");
		goto out_free;
	}

	/* lzo1x_1_compress does not return anything but LZO_OK so we ignore
	 * the return value */
	lzo1x_1_compress(cthread->s_buf, in_len, c_buf, &dlen, wrkmem);
	ret = 0;

	if (dlen >= in_len){
		/* Incompressible, leave as CTYPE_NONE */
		print_maxverbose("Incompressible block\n");
		dealloc(c_buf);
		goto out_free;
	}

	cthread->c_len = dlen;
	dealloc(cthread->s_buf);
	cthread->s_buf = c_buf;
	cthread->c_type = CTYPE_LZO;
out_free:
	dealloc(wrkmem);
	return ret;
}

/*
  ***** DECOMPRESSION FUNCTIONS *****

  ZPAQ, BZIP, GZIP, LZMA, LZO

  try to decompress a buffer. Return 0 on success and -1 on failure.
*/
static int zpaq_decompress_buf(rzip_control *control __UNUSED__, struct uncomp_thread *ucthread, long thread)
{
	i64 dlen = ucthread->u_len;
	uchar *c_buf;
	int zd_ret, ret = 0;

	c_buf = ucthread->s_buf;
	ucthread->s_buf = malloc(round_up_page(control, dlen));
	if (unlikely(!ucthread->s_buf)) {
		print_err("Failed to allocate %ld bytes for decompression\n", dlen);
		ret = -1;
		goto out;
	}

	dlen = 0;
	zd_ret = zpaq_decompress(ucthread->s_buf, &dlen, c_buf, ucthread->c_len,
			control->msgout, SHOW_PROGRESS ? true: false, thread,
				ucthread->u_len);

	if (unlikely(zd_ret < 0)) {
		print_err("Attempted to write beyond expected output size, corrupted input.\n");
		ret = -1;
		goto out;
	}
	if (unlikely(dlen != ucthread->u_len)) {
		print_err("Inconsistent length after decompression. Got %ld bytes, expected %"PRId64"\n", dlen, ucthread->u_len);
		ret = -1;
	} else
		dealloc(c_buf);
out:
	if (ret == -1) {
		dealloc(ucthread->s_buf);
		ucthread->s_buf = c_buf;
	}
	return ret;
}

static int bzip2_decompress_buf(rzip_control *control __UNUSED__, struct uncomp_thread *ucthread)
{
	u32 dlen = ucthread->u_len;
	int ret = 0, bzerr;
	uchar *c_buf;

	c_buf = ucthread->s_buf;
	ucthread->s_buf = malloc(round_up_page(control, dlen));
	if (unlikely(!ucthread->s_buf)) {
		print_err("Failed to allocate %d bytes for decompression\n", dlen);
		ret = -1;
		goto out;
	}

	bzerr = BZ2_bzBuffToBuffDecompress((char*)ucthread->s_buf, &dlen, (char*)c_buf, ucthread->c_len, 0, 0);
	if (unlikely(bzerr != BZ_OK)) {
		print_err("Failed to decompress buffer - bzerr=%d\n", bzerr);
		ret = -1;
		goto out;
	}

	if (unlikely(dlen != ucthread->u_len)) {
		print_err("Inconsistent length after decompression. Got %d bytes, expected %"PRId64"\n", dlen, ucthread->u_len);
		ret = -1;
	} else
		dealloc(c_buf);
out:
	if (ret == -1) {
		dealloc(ucthread->s_buf);
		ucthread->s_buf = c_buf;
	}
	return ret;
}

static int gzip_decompress_buf(rzip_control *control __UNUSED__, struct uncomp_thread *ucthread)
{
	unsigned long dlen = ucthread->u_len;
	int ret = 0, gzerr;
	uchar *c_buf;

	c_buf = ucthread->s_buf;
	ucthread->s_buf = malloc(round_up_page(control, dlen));
	if (unlikely(!ucthread->s_buf)) {
		print_err("Failed to allocate %ld bytes for decompression\n", dlen);
		ret = -1;
		goto out;
	}

	gzerr = uncompress(ucthread->s_buf, &dlen, c_buf, ucthread->c_len);
	if (unlikely(gzerr != Z_OK)) {
		print_err("Failed to decompress buffer - gzerr=%d\n", gzerr);
		ret = -1;
		goto out;
	}

	if (unlikely((i64)dlen != ucthread->u_len)) {
		print_err("Inconsistent length after decompression. Got %ld bytes, expected %"PRId64"\n", dlen, ucthread->u_len);
		ret = -1;
	} else
		dealloc(c_buf);
out:
	if (ret == -1) {
		dealloc(ucthread->s_buf);
		ucthread->s_buf = c_buf;
	}
	return ret;
}

static int lzma_decompress_buf(rzip_control *control, struct uncomp_thread *ucthread)
{
	size_t dlen = ucthread->u_len;
	int ret = 0, lzmaerr;
	uchar *c_buf;
	SizeT c_len = ucthread->c_len;

	c_buf = ucthread->s_buf;
	ucthread->s_buf = malloc(round_up_page(control, dlen));
	if (unlikely(!ucthread->s_buf)) {
		print_err("Failed to allocate %"PRId64" bytes for decompression\n", (i64)dlen);
		ret = -1;
		goto out;
	}

	/* LZMA SDK: pass control->lzma_properties
	 * which is needed for proper uncompress */
	lzmaerr = LzmaUncompress(ucthread->s_buf, &dlen, c_buf, &c_len, control->lzma_properties, 5);
	if (unlikely(lzmaerr)) {
		print_err("Failed to decompress buffer - lzmaerr=%d\n", lzmaerr);
		ret = -1;
		goto out;
	}

	if (unlikely((i64)dlen != ucthread->u_len)) {
		print_err("Inconsistent length after decompression. Got %"PRId64" bytes, expected %"PRId64"\n", (i64)dlen, ucthread->u_len);
		ret = -1;
	} else
		dealloc(c_buf);
out:
	if (ret == -1) {
		dealloc(ucthread->s_buf);
		ucthread->s_buf = c_buf;
	}
	return ret;
}

static int lzo_decompress_buf(rzip_control *control __UNUSED__, struct uncomp_thread *ucthread)
{
	lzo_uint dlen = ucthread->u_len;
	int ret = 0, lzerr;
	uchar *c_buf;

	c_buf = ucthread->s_buf;
	ucthread->s_buf = malloc(round_up_page(control, dlen));
	if (unlikely(!ucthread->s_buf)) {
		print_err("Failed to allocate %lu bytes for decompression\n", (unsigned long)dlen);
		ret = -1;
		goto out;
	}

	lzerr = lzo1x_decompress_safe((uchar*)c_buf, ucthread->c_len, (uchar*)ucthread->s_buf, &dlen, NULL);
	if (unlikely(lzerr != LZO_E_OK)) {
		print_err("Failed to decompress buffer - lzerr=%d\n", lzerr);
		ret = -1;
		goto out;
	}

	if (unlikely((i64)dlen != ucthread->u_len)) {
		print_err("Inconsistent length after decompression. Got %lu bytes, expected %"PRId64"\n", (unsigned long)dlen, ucthread->u_len);
		ret = -1;
	} else
		dealloc(c_buf);
out:
	if (ret == -1) {
		dealloc(ucthread->s_buf);
		ucthread->s_buf = c_buf;
	}
	return ret;
}

/* WORK FUNCTIONS */

/* Look at whether we're writing to a ram location or physical files and write
 * the data accordingly. */
ssize_t put_fdout(rzip_control *control, void *offset_buf, ssize_t ret)
{
	if (!TMP_OUTBUF)
		return write(control->fd_out, offset_buf, (size_t)ret);

	if (unlikely(control->out_ofs + ret > control->out_maxlen)) {
		/* The data won't fit in a temporary output buffer so we have
		 * to fall back to temporary files. */
		print_verbose("Unable to %scompress entirely in ram, will use physical files\n",
			      DECOMPRESS ? "de" : "");
		if (unlikely(control->fd_out == -1)) {
			failure("Was unable to %scompress entirely in ram and no temporary file creation was possible\n",
				DECOMPRESS ? "de" : "");
		}
		/* Copy tmp_outbuf to tmpoutfile before deallocation */
		if (unlikely(!write_fdout(control, control->tmp_outbuf, control->out_len))) {
			print_err("Unable to write_fdout tmpoutbuf in put_fdout\n");
			return -1;
		}
		/* Deallocate now unused tmpoutbuf and unset tmp_outbuf flag */
		close_tmpoutbuf(control);
		return write(control->fd_out, offset_buf, (size_t)ret);
	}

	memcpy(control->tmp_outbuf + control->out_ofs, offset_buf, ret);
	control->out_ofs += ret;
	if (likely(control->out_ofs > control->out_len))
		control->out_len = control->out_ofs;
	return ret;
}

/* Write len bytes, looping on short writes. */
ssize_t write_all(rzip_control *control, void *buf, i64 len)
{
	uchar *offset_buf = buf;
	ssize_t ret;
	i64 total;

	total = 0;
	while (len > 0) {
		ret = put_fdout(control, offset_buf, (size_t)len);
		if (unlikely(ret <= 0))
			return ret;
		len -= ret;
		offset_buf += ret;
		total += ret;
	}
	return total;
}

/* Should be called only if we know the buffer will be large enough, otherwise
 * we must dump_stdin first */
static bool read_fdin(struct rzip_control *control, i64 len)
{
	int tmpchar;
	i64 i;

	for (i = 0; i < len; i++) {
		tmpchar = getchar();
		if (unlikely(tmpchar == EOF))
			failure_return(("Reached end of file on STDIN prematurely on read_fdin, asked for %"PRId64" got %"PRId64"\n",
				len, i), false);
		control->tmp_inbuf[control->in_ofs + i] = (char)tmpchar;
	}
	control->in_len = control->in_ofs + len;
	return true;
}

/* Dump STDIN into a temporary file */
static int dump_stdin(rzip_control *control)
{
	if (unlikely(!write_fdin(control)))
		return -1;
	if (unlikely(!read_tmpinfile(control, control->fd_in)))
		return -1;
	close_tmpinbuf(control);
	return 0;
}

/* Read len bytes, looping on short reads. */
ssize_t read_all(rzip_control *control, int fd, void *buf, i64 len)
{
	uchar *offset_buf = buf;
	ssize_t ret;
	i64 total;

	if (TMP_INBUF && fd == control->fd_in) {
		/* We're decompressing from STDIN */
		if (unlikely(control->in_ofs + len > control->in_maxlen)) {
			/* We're unable to fit it all into the temp buffer */
			if (dump_stdin(control)) {
				failure_return(("Inadequate ram to %scompress from STDIN and unable to create in tmpfile",
					DECOMPRESS ? "de" : ""), -1);
			}
			goto read_fd;
		}
		if (control->in_ofs + len > control->in_len) {
			if (unlikely(!read_fdin(control, control->in_ofs + len - control->in_len)))
				return false;
		}
		memcpy(buf, control->tmp_inbuf + control->in_ofs, len);
		control->in_ofs += len;
		return len;
	}

	if (TMP_OUTBUF && fd == control->fd_out) {
		if (unlikely(control->out_ofs + len > control->out_maxlen))
			failure_return(("Trying to read beyond out_ofs in tmpoutbuf\n"), -1);
		memcpy(buf, control->tmp_outbuf + control->out_ofs, len);
		control->out_ofs += len;
		return len;
	}

read_fd:
	total = 0;
	while (len > 0) {
		ret = read(fd, offset_buf, (size_t)len);
		if (unlikely(ret <= 0))
			return ret;
		len -= ret;
		offset_buf += ret;
		total += ret;
	}
	return total;
}

/* write to a file, return 0 on success and -1 on failure */
static int write_buf(rzip_control *control, uchar *p, i64 len)
{
	ssize_t ret;

	ret = write_all(control, p, (size_t)len);
	if (unlikely(ret == -1)) {
		print_err("Write of length %"PRId64" failed - %s\n", len, strerror(errno));
		return -1;
	}
	if (unlikely(ret != (ssize_t)len)) {
		print_err("Partial write!? asked for %"PRId64" bytes but got %"PRId64"\n", len, (i64)ret);
		return -1;
	}
	return 0;
}

/* write a byte */
static inline int write_u8(rzip_control *control, uchar v)
{
	return write_buf(control, &v, 1);
}

static inline int write_val(rzip_control *control, i64 v, int len)
{
	v = htole64(v);
	return write_buf(control, (uchar *)&v, len);
}

static int read_buf(rzip_control *control, int f, uchar *p, i64 len)
{
	ssize_t ret;

	ret = read_all(control, f, p, (size_t)len);
	if (unlikely(ret == -1)) {
		print_err("Read of length %"PRId64" failed - %s\n", len, strerror(errno));
		return -1;
	}
	if (unlikely(ret != (ssize_t)len)) {
		print_err("Partial read!? asked for %"PRId64" bytes but got %"PRId64"\n", len, (i64)ret);
		return -1;
	}
	return 0;
}

static inline int read_u8(rzip_control *control, int f, uchar *v)
{
	return read_buf(control, f, v, 1);
}

static inline int read_u32(rzip_control *control, int f, u32 *v)
{
	int ret = read_buf(control, f, (uchar *)v, 4);

	*v = le32toh(*v);
	return ret;
}

static inline int read_val(rzip_control *control, int f, i64 *v, int len)
{
	int ret;

	/* We only partially read all 8 bytes so have to zero v here */
	*v = 0;
	ret = read_buf(control, f, (uchar *)v, len);
	return ret;
}

static int fd_seekto(rzip_control *control, struct stream_info *sinfo, i64 spos, i64 pos)
{
	if (unlikely(lseek(sinfo->fd, spos, SEEK_SET) != spos)) {
		print_err("Failed to seek to %"PRId64" in stream\n", pos);
		return -1;
	}
	return 0;
}

/* seek to a position within a set of streams - return -1 on failure */
static int seekto(rzip_control *control, struct stream_info *sinfo, i64 pos)
{
	i64 spos = pos + sinfo->initial_pos;

	if (TMP_OUTBUF) {
		spos -= control->out_relofs;
		control->out_ofs = spos;
		if (unlikely(spos > control->out_len || spos < 0)) {
			print_err("Trying to seek to %"PRId64" outside tmp outbuf in seekto\n", spos);
			return -1;
		}
		return 0;
	}

	return fd_seekto(control, sinfo, spos, pos);
}

static int read_seekto(rzip_control *control, struct stream_info *sinfo, i64 pos)
{
	i64 spos = pos + sinfo->initial_pos;

	if (TMP_INBUF) {
		if (spos > control->in_len) {
			i64 len = spos - control->in_len;

			if (control->in_ofs + len > control->in_maxlen) {
				if (unlikely(dump_stdin(control)))
					return -1;
				goto fd_seek;
			} else {
				if (unlikely(!read_fdin(control, len)))
					return -1;
			}
		}
		control->in_ofs = spos;
		if (unlikely(spos < 0)) {
			print_err("Trying to seek to %"PRId64" outside tmp inbuf in read_seekto\n", spos);
			return -1;
		}
		return 0;
	}
fd_seek:
	return fd_seekto(control, sinfo, spos, pos);
}

static i64 get_seek(rzip_control *control, int fd)
{
	i64 ret;

	if (TMP_OUTBUF)
		return control->out_relofs + control->out_ofs;
	ret = lseek(fd, 0, SEEK_CUR);
	if (unlikely(ret == -1))
		fatal_return(("Failed to lseek in get_seek\n"), -1);
	return ret;
}

i64 get_readseek(rzip_control *control, int fd)
{
	i64 ret;

	if (TMP_INBUF)
		return control->in_ofs;
	ret = lseek(fd, 0, SEEK_CUR);
	if (unlikely(ret == -1))
		fatal_return(("Failed to lseek in get_seek\n"), -1);
	return ret;
}

bool prepare_streamout_threads(rzip_control *control)
{
	pthread_t *threads;
	int i;

	/* As we serialise the generation of threads during the rzip
	 * pre-processing stage, it's faster to have one more thread available
	 * to keep all CPUs busy. There is no point splitting up the chunks
	 * into multiple threads if there will be no compression back end. */
	if (control->threads > 1)
		++control->threads;
	if (NO_COMPRESS)
		control->threads = 1;
	threads = control->pthreads = calloc(control->threads, sizeof(pthread_t));
	if (unlikely(!threads))
		fatal_return(("Unable to calloc threads in prepare_streamout_threads\n"), false);

	cthreads = calloc(control->threads, sizeof(struct compress_thread));
	if (unlikely(!cthreads)) {
		dealloc(threads);
		fatal_return(("Unable to calloc cthreads in prepare_streamout_threads\n"), false);
	}

	for (i = 0; i < control->threads; i++) {
		cksem_init(control, &cthreads[i].cksem);
		cksem_post(control, &cthreads[i].cksem);
	}
	return true;
}


/* Wait until all compress output threads have finished their current work
 * without tearing the thread pool down. Used before progressive STDOUT flush
 * so the block is complete and LRZC compressed_size can be patched. */
bool wait_streamout_threads(rzip_control *control)
{
	int i, close_thread = output_thread;

	for (i = 0; i < control->threads; i++) {
		cksem_wait(control, &cthreads[close_thread].cksem);
		cksem_post(control, &cthreads[close_thread].cksem);
		if (++close_thread == control->threads)
			close_thread = 0;
	}
	return true;
}

bool close_streamout_threads(rzip_control *control)
{
	int i, close_thread = output_thread;

	/* Wait for the threads in the correct order in case they end up
	 * serialised */
	for (i = 0; i < control->threads; i++) {
		cksem_wait(control, &cthreads[close_thread].cksem);

		if (++close_thread == control->threads)
			close_thread = 0;
	}
	dealloc(cthreads);
	dealloc(control->pthreads);
	return true;
}

/* Write a v0.7 LRZC continuation header with compressed_size left as zero
 * for later patching once the block payload is fully written. */
static bool write_lrzc_header(rzip_control *control, int fd, i64 u_size, int last)
{
	uchar hdr[LRZC_LEN];
	i64 ule, cle = 0;

	memset(hdr, 0, sizeof(hdr));
	hdr[0] = 'L';
	hdr[1] = 'R';
	hdr[2] = 'Z';
	hdr[3] = 'C';
	if (!ENCRYPT) {
		ule = htole64(u_size);
		memcpy(hdr + 4, &ule, 8);
	}
	/* bytes 12-19 compressed size: patched later */
	memcpy(hdr + 12, &cle, 8);
	if (last)
		hdr[20] = 1;

	control->lrzc_pos = get_seek(control, fd);
	if (unlikely(control->lrzc_pos == -1))
		return false;
	if (unlikely(write_buf(control, hdr, LRZC_LEN)))
		failure_return(("Failed to write LRZC header\n"), false);
	print_maxverbose("Wrote LRZC header at %"PRId64" u_size=%"PRId64" last=%d\n",
			 control->lrzc_pos, ENCRYPT ? 0 : u_size, last);
	return true;
}

/* Patch LRZC compressed_size after the block payload is complete. */
bool patch_lrzc_c_size(rzip_control *control, int fd)
{
	i64 end, c_size, cle;
	i64 saved_ofs = -1;

	if (TMP_OUTBUF)
		end = control->out_relofs + control->out_len;
	else {
		end = lseek(fd, 0, SEEK_CUR);
		if (unlikely(end == -1))
			fatal_return(("Failed to lseek end in patch_lrzc_c_size\n"), false);
		saved_ofs = end;
	}

	c_size = end - control->lrzc_pos - LRZC_LEN;
	if (unlikely(c_size < 0))
		failure_return(("Invalid LRZC compressed size %"PRId64"\n", c_size), false);

	cle = htole64(c_size);
	if (TMP_OUTBUF) {
		i64 ofs = control->lrzc_pos - control->out_relofs + 12;

		if (unlikely(ofs < 0 || ofs + 8 > control->out_len))
			failure_return(("LRZC c_size patch outside tmp outbuf\n"), false);
		memcpy(control->tmp_outbuf + ofs, &cle, 8);
	} else {
		if (unlikely(lseek(fd, control->lrzc_pos + 12, SEEK_SET) == -1))
			fatal_return(("Failed to seek LRZC c_size in patch_lrzc_c_size\n"), false);
		if (unlikely(write(fd, &cle, 8) != 8))
			fatal_return(("Failed to write LRZC c_size\n"), false);
		if (unlikely(lseek(fd, saved_ofs, SEEK_SET) == -1))
			fatal_return(("Failed to restore fd offset after LRZC patch\n"), false);
	}
	print_maxverbose("Patched LRZC c_size=%"PRId64" at %"PRId64"\n", c_size, control->lrzc_pos);
	return true;
}

/* Read and validate a v0.7 LRZC header. Sets control->last_block from flags.
 * Returns compressed_size via *c_size_out when non-NULL. */
bool read_lrzc_header(rzip_control *control, int fd_in, i64 *c_size_out, i64 *u_size_out)
{
	uchar hdr[LRZC_LEN];
	i64 u_size, c_size;

	if (unlikely(read_all(control, fd_in, hdr, LRZC_LEN) != LRZC_LEN))
		fatal_return(("Failed to read LRZC header\n"), false);
	if (unlikely(strncmp((char *)hdr, "LRZC", 4)))
		failure_return(("Expected LRZC continuation header\n"), false);

	memcpy(&u_size, hdr + 4, 8);
	u_size = le64toh(u_size);
	memcpy(&c_size, hdr + 12, 8);
	c_size = le64toh(c_size);

	if (hdr[20] & ~1)
		failure_return(("Invalid LRZC flag bits\n"), false);
	if (hdr[21] || hdr[22] || hdr[23])
		failure_return(("Invalid LRZC reserved bytes\n"), false);

	control->last_block = hdr[20] & 1;
	if (unlikely(c_size <= 0))
		failure_return(("Invalid LRZC compressed size %"PRId64"\n", c_size), false);
	if (ENCRYPT && u_size != 0)
		failure_return(("Encrypted LRZC has non-zero uncompressed size\n"), false);

	print_maxverbose("LRZC u_size=%"PRId64" c_size=%"PRId64" last=%u\n",
			 u_size, c_size, control->last_block);

	if (c_size_out)
		*c_size_out = c_size;
	if (u_size_out)
		*u_size_out = u_size;
	return true;
}

/* open a set of output streams, compressing with the given
   compression level and algorithm */
void *open_stream_out(rzip_control *control, int f, unsigned int n, i64 chunk_limit, char cbytes)
{
	struct stream_info *sinfo;
	unsigned int i, testbufs;
	bool threadlimit = false, memlimit = false;
	i64 testsize, limit;
	uchar *testmalloc;

	sinfo = calloc(1, sizeof(struct stream_info));
	if (unlikely(!sinfo))
		return NULL;
	if (chunk_limit < control->page_size)
		chunk_limit = control->page_size;
	sinfo->bufsize = sinfo->size = limit = chunk_limit;

	sinfo->chunk_bytes = cbytes;
	sinfo->chunk_filter = control->chunk_filter;
	sinfo->num_streams = n;
	sinfo->fd = f;

	sinfo->s = calloc(n, sizeof(struct stream));
	if (unlikely(!sinfo->s)) {
		dealloc(sinfo);
		return NULL;
	}

	/* Find the largest we can make the window based on ability to malloc
	 * ram. We need 2 buffers for each compression thread and the overhead
	 * of each compression back end. No 2nd buf is required when there is
	 * no back end compression. We limit the total regardless to 1/3 ram
	 * for when the OS lies due to heavy overcommit. */
	if (NO_COMPRESS)
		testbufs = 1;
	else
		testbufs = 2;

	testsize = (limit * testbufs) + (control->overhead * control->threads);
	if (testsize > control->usable_ram)
		limit = (control->usable_ram - (control->overhead * control->threads)) / testbufs;

	/* If we don't have enough ram for the number of threads, decrease the
	 * number of threads till we do, or only have one thread. */
	while (limit < STREAM_BUFSIZE && limit < chunk_limit) {
		if (control->threads > 1) {
			--control->threads;
			threadlimit = true;
		} else
			break;
		limit = (control->usable_ram - (control->overhead * control->threads)) / testbufs;
		limit = MIN(limit, chunk_limit);
	}
	if (threadlimit) {
		print_output("Minimising number of threads to %d to limit memory usage\n",
			     control->threads);
	}
	/* Use a nominal minimum size should we fail all previous shrinking */
	if (limit < STREAM_BUFSIZE) {
		limit = MAX(limit, STREAM_BUFSIZE);
		if (threadlimit || memlimit)
			print_output("Warning, low memory for chosen compression settings\n");
	}
	limit = MIN(limit, chunk_limit);
retest_malloc:
	testsize = limit + (control->overhead * control->threads);
	testmalloc = malloc(testsize);
	if (!testmalloc) {
		memlimit = true;
		limit = limit / 10 * 9;
		if (limit < 100000000) {
			/* If we can't even allocate 100MB then we'll never
			 * succeed */
			print_err("Unable to allocate enough memory for operation\n");
			dealloc(sinfo->s);
			dealloc(sinfo);
			return NULL;
		}
		goto retest_malloc;
	}
	if (!NO_COMPRESS) {
		char *testmalloc2 = malloc(limit);

		if (!testmalloc2) {
			dealloc(testmalloc);
			limit = limit / 10 * 9;
			goto retest_malloc;
		}
		dealloc(testmalloc2);
	}
	dealloc(testmalloc);
	print_maxverbose("Succeeded in testing %"PRId64" sized malloc for back end compression\n", testsize);

	/* Make the bufsize no smaller than STREAM_BUFSIZE. Round up the
	 * bufsize to fit X threads into it */
	sinfo->bufsize = MIN(limit, MAX((limit + control->threads - 1) / control->threads,
					STREAM_BUFSIZE));

	if (control->threads > 1)
		print_maxverbose("Using up to %d threads to compress up to %"PRId64" bytes each.\n",
			control->threads, sinfo->bufsize);
	else
		print_maxverbose("Using only 1 thread to compress up to %"PRId64" bytes\n",
			sinfo->bufsize);

	for (i = 0; i < n; i++) {
		sinfo->s[i].buf = calloc(sinfo->bufsize , 1);
		if (unlikely(!sinfo->s[i].buf)) {
			fatal("Unable to malloc buffer of size %"PRId64" in open_stream_out\n", sinfo->bufsize);
			dealloc(sinfo->s);
			dealloc(sinfo);
			return NULL;
		}
	}

	return (void *)sinfo;
}

/* AAD for suite-3: "LRZI" + major + minor + enc_mode + context */
static void aead_fill_aad(const rzip_control *control, uchar ctx,
			  uchar aad[8], size_t *aad_len)
{
	aad[0] = 'L';
	aad[1] = 'R';
	aad[2] = 'Z';
	aad[3] = 'I';
	aad[4] = (uchar)control->major_version;
	aad[5] = (uchar)control->minor_version;
	aad[6] = ENCRYPT_AEAD ? 3 : 1;
	aad[7] = ctx; /* 1=header, 2=data, 3=md5 */
	*aad_len = 8;
}

/* Decrypt a stream header.
 *   AEAD: head is nonce||ct25||tag (53 bytes)
 *   legacy: head is salt8||ct25
 */
static bool decrypt_header(rzip_control *control, uchar *head, uchar *c_type,
			   i64 *c_len, i64 *u_len, i64 *last_head)
{
	if (ENCRYPT_AEAD) {
		uchar clear[25], aad[8];
		size_t aad_len = 0, pt_len = sizeof(clear);

		aead_fill_aad(control, 0x01, aad, &aad_len);
		if (unlikely(!lrz_aead_open(control, LRZ_AEAD_KEY_HDR, aad, aad_len,
					    head, LRZ_AEAD_NONCE_LEN + 25 + LRZ_AEAD_TAG_LEN,
					    clear, &pt_len))) {
			print_err("Header AEAD check failed (corrupt or wrong password)\n");
			return false;
		}
		memcpy(c_type, clear, 1);
		memcpy(c_len, clear + 1, 8);
		memcpy(u_len, clear + 9, 8);
		memcpy(last_head, clear + 17, 8);
		return true;
	} else {
		uchar *buf = head + SALT_LEN;

		if (unlikely(!lrz_decrypt(control, buf, 25, head)))
			return false;

		memcpy(c_type, buf, 1);
		memcpy(c_len, buf + 1, 8);
		memcpy(u_len, buf + 9, 8);
		memcpy(last_head, buf + 17, 8);
		return true;
	}
}

/* prepare a set of n streams for reading on file descriptor f */
void *open_stream_in(rzip_control *control, int f, int n, char chunk_bytes)
{
	struct uncomp_thread *ucthreads;
	struct stream_info *sinfo;
	int total_threads, i;
	pthread_t *threads;
	i64 header_length;

	sinfo = calloc(1, sizeof(struct stream_info));
	if (unlikely(!sinfo))
		return NULL;

	/* We have one thread dedicated to stream 0, and one more thread than
	 * CPUs to keep them busy, unless we're running single-threaded. */
	if (control->threads > 1)
		total_threads = control->threads + 2;
	else
		total_threads = control->threads + 1;
	threads = control->pthreads = calloc(total_threads, sizeof(pthread_t));
	if (unlikely(!threads))
		return NULL;

	sinfo->ucthreads = ucthreads = calloc(total_threads, sizeof(struct uncomp_thread));
	if (unlikely(!ucthreads)) {
		dealloc(sinfo);
		dealloc(threads);
		fatal_return(("Unable to calloc ucthreads in open_stream_in\n"), NULL);
	}

	sinfo->num_streams = n;
	sinfo->fd = f;
	sinfo->chunk_bytes = chunk_bytes;

	sinfo->s = calloc(n, sizeof(struct stream));
	if (unlikely(!sinfo->s)) {
		dealloc(sinfo);
		dealloc(threads);
		dealloc(ucthreads);
		return NULL;
	}

	sinfo->s[0].total_threads = 1;
	sinfo->s[1].total_threads = total_threads - 1;

	if (control->major_version == 0 && control->minor_version > 5) {
		/* Read in flag that tells us if there are more chunks after
		 * this. Ignored if we know the final file size */
		print_maxverbose("Reading eof flag at %"PRId64"\n", get_readseek(control, f));
		if (unlikely(read_u8(control, f, &control->eof))) {
			print_err("Failed to read eof flag in open_stream_in\n");
			goto failed;
		}
		print_maxverbose("EOF: %d\n", control->eof);

		/* Read in the expected chunk size */
		if (!ENCRYPT) {
			print_maxverbose("Reading expected chunksize at %"PRId64"\n", get_readseek(control, f));
			if (unlikely(read_val(control, f, &sinfo->size, sinfo->chunk_bytes))) {
				print_err("Failed to read in chunk size in open_stream_in\n");
				goto failed;
			}
			sinfo->size = le64toh(sinfo->size);
			print_maxverbose("Chunk size: %"PRId64"\n", sinfo->size);
			control->st_size += sinfo->size;
			if (unlikely(sinfo->chunk_bytes < 1 || sinfo->chunk_bytes > 8 || sinfo->size < 0)) {
				print_err("Invalid chunk data size %"PRId64" bytes %d\n", sinfo->size, sinfo->chunk_bytes);
				goto failed;
			}
		}
	}
	sinfo->initial_pos = get_readseek(control, f);
	if (unlikely(sinfo->initial_pos == -1))
		goto failed;

	/* Frame end from LRZC c_size for this RCD (set by runzip_fd). */
	if (control->block_c_size > 0 && control->rcd_start >= 0)
		sinfo->payload_end = control->rcd_start + control->block_c_size;
	else
		sinfo->payload_end = 0;

	/* Only trust fstat on regular files. TMP_INBUF / pipes do not have a
	 * reliable final size yet; last_head is still bounded by payload_end. */
	if (!TMP_INBUF) {
		struct stat st;

		if (fstat(f, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0)
			sinfo->infile_size = st.st_size;
	}

	for (i = 0; i < n; i++) {
		/* Max header wire size: AEAD 53 or salt+25 */
		uchar c, enc_head[LRZ_AEAD_NONCE_LEN + 25 + LRZ_AEAD_TAG_LEN];
		i64 v1, v2;

		sinfo->s[i].base_thread = i;
		sinfo->s[i].uthread_no = sinfo->s[i].base_thread;
		sinfo->s[i].unext_thread = sinfo->s[i].base_thread;

		if (ENCRYPT) {
			i64 hlen = lrz_enc_header_disk_len(control);

			if (unlikely(hlen > (i64)sizeof(enc_head)))
				goto failed;
			if (unlikely(read_buf(control, f, enc_head, hlen)))
				goto failed;
			sinfo->total_read += hlen;
			if (unlikely(!decrypt_header(control, enc_head, &c, &v1, &v2,
						     &sinfo->s[i].last_head)))
				goto failed;
			header_length = 0; /* already counted */
		} else {
again:
		if (unlikely(read_u8(control, f, &c)))
			goto failed;

		/* Compatibility crap for versions < 0.40 */
		if (control->major_version == 0 && control->minor_version < 4) {
			u32 v132, v232, last_head32;

			if (unlikely(read_u32(control, f, &v132)))
				goto failed;
			if (unlikely(read_u32(control, f, &v232)))
				goto failed;
			if (unlikely(read_u32(control, f, &last_head32)))
				goto failed;

			v1 = v132;
			v2 = v232;
			sinfo->s[i].last_head = last_head32;
			header_length = 13;
		} else {
			int read_len;

			print_maxverbose("Reading stream %d header at %"PRId64"\n", i, get_readseek(control, f));
			if (control->major_version == 0 && control->minor_version < 6)
					read_len = 8;
			else
				read_len = sinfo->chunk_bytes;
			if (unlikely(read_val(control, f, &v1, read_len)))
				goto failed;
			if (unlikely(read_val(control, f, &v2, read_len)))
				goto failed;
			if (unlikely(read_val(control, f, &sinfo->s[i].last_head, read_len)))
				goto failed;
			header_length = 1 + (read_len * 3);
		}
		sinfo->total_read += header_length;
		}

		v1 = le64toh(v1);
		v2 = le64toh(v2);
		sinfo->s[i].last_head = le64toh(sinfo->s[i].last_head);

		if (unlikely(!ENCRYPT && c == CTYPE_NONE && v1 == 0 && v2 == 0 &&
			     sinfo->s[i].last_head == 0 && i == 0)) {
			print_err("Enabling stream close workaround\n");
			sinfo->initial_pos += header_length;
			goto again;
		}

		if (unlikely(c != CTYPE_NONE)) {
			print_err("Unexpected initial tag %d in streams\n", c);
			if (ENCRYPT)
				print_err("Wrong password?\n");
			goto failed;
		}
		if (unlikely(v1)) {
			print_err("Unexpected initial c_len %"PRId64" in streams %"PRId64"\n", v1, v2);
			goto failed;
		}
		if (unlikely(v2)) {
			print_err("Unexpected initial u_len %"PRId64" in streams\n", v2);
			goto failed;
		}
	}

	return (void *)sinfo;

failed:
	dealloc(sinfo->s);
	dealloc(sinfo);
	dealloc(threads);
	dealloc(ucthreads);
	return NULL;
}

#define MIN_SIZE (ENCRYPT_AEAD ? 0 : (ENCRYPT ? CBC_LEN : 0))

/* Once the final data has all been written to the block header, we go back
 * and write salt/nonce before it, and encrypt the header in place.
 * ofs is the start of the 25-byte clear header relative to initial_pos. */
static bool rewrite_encrypted(rzip_control *control, struct stream_info *sinfo, i64 ofs)
{
	uchar *buf, *head;
	i64 cur_ofs, prefix;

	cur_ofs = get_seek(control, sinfo->fd) - sinfo->initial_pos;
	if (unlikely(cur_ofs == -1))
		return false;

	if (ENCRYPT_AEAD) {
		uchar clear[25], sealed[LRZ_AEAD_NONCE_LEN + 25 + LRZ_AEAD_TAG_LEN];
		uchar aad[8];
		size_t aad_len = 0, slen = sizeof(sealed);

		if (unlikely(seekto(control, sinfo, ofs)))
			failure_return(("Failed to seek clear header in rewrite_encrypted\n"), false);
		if (unlikely(read_buf(control, sinfo->fd, clear, 25)))
			failure_return(("Failed to read clear header in rewrite_encrypted\n"), false);
		aead_fill_aad(control, 0x01, aad, &aad_len);
		if (unlikely(!lrz_aead_seal(control, LRZ_AEAD_KEY_HDR, aad, aad_len,
					    clear, 25, sealed, &slen)))
			failure_return(("Failed to AEAD-seal header\n"), false);
		if (unlikely(seekto(control, sinfo, ofs - LRZ_AEAD_NONCE_LEN)))
			failure_return(("Failed to seek sealed header in rewrite_encrypted\n"), false);
		if (unlikely(write_buf(control, sealed, (i64)slen)))
			failure_return(("Failed to write sealed header\n"), false);
		seekto(control, sinfo, cur_ofs);
		return true;
	}

	prefix = SALT_LEN;
	head = malloc(25 + SALT_LEN);
	if (unlikely(!head))
		fatal_return(("Failed to malloc head in rewrite_encrypted\n"), false);
	buf = head + SALT_LEN;
	if (unlikely(!get_rand(control, head, SALT_LEN)))
		goto error;
	if (unlikely(seekto(control, sinfo, ofs - prefix)))
		failure_goto(("Failed to seekto buf ofs in rewrite_encrypted\n"), error);
	if (unlikely(write_buf(control, head, SALT_LEN)))
		failure_goto(("Failed to write_buf head in rewrite_encrypted\n"), error);
	if (unlikely(read_buf(control, sinfo->fd, buf, 25)))
		failure_goto(("Failed to read_buf buf in rewrite_encrypted\n"), error);

	if (unlikely(!lrz_encrypt(control, buf, 25, head)))
		goto error;

	if (unlikely(seekto(control, sinfo, ofs)))
		failure_goto(("Failed to seek back to ofs in rewrite_encrypted\n"), error);
	if (unlikely(write_buf(control, buf, 25)))
		failure_goto(("Failed to write_buf encrypted buf in rewrite_encrypted\n"), error);

	dealloc(head);
	seekto(control, sinfo, cur_ofs);
	return true;
error:
	dealloc(head);
	return false;
}

/* Enter with s_buf allocated; s_buf points to the data (possibly compressed)
 * and is freed here. Output to the archive is strictly ordered by
 * output_thread so last_head links stay consistent. Every exit path must
 * claim and release that ordering slot so other workers cannot hang. */
static void *compthread(void *data)
{
	stream_thread_struct *s = data;
	rzip_control *control = s->control;
	long i = s->i;
	struct compress_thread *cti;
	struct stream_info *ctis;
	int waited = 0, ret = 0, turn_released = 0;
	const char *fatal_msg = NULL;
	i64 padded_len;
	int write_len;

	dealloc(data);
	cti = &cthreads[i];
	ctis = cti->sinfo;

	if (unlikely(setpriority(PRIO_PROCESS, 0, control->nice_val) == -1)) {
		print_err("Warning, unable to set thread nice value %d...Resetting to %d\n", control->nice_val, control->current_priority);
		setpriority(PRIO_PROCESS, 0, (control->nice_val=control->current_priority));
	}
	cti->c_type = CTYPE_NONE;
	cti->c_len = cti->s_len;

	/* Cludge for STDOUT: default lc/lp/pb byte to 93 if magic must be
	 * written before any LZMA job has published real properties. Guard
	 * with control_lock so we do not race other workers or write_magic. */
	if (TMP_OUTBUF && LZMA_COMPRESS) {
		lock_mutex(control, &control->control_lock);
		if (!control->lzma_prop_set)
			control->lzma_properties[0] = 93;
		unlock_mutex(control, &control->control_lock);
	}
retry:
	/* Very small buffers have issues to do with minimum amounts of ram
	 * allocatable to a buffer combined with the MINIMUM_MATCH of rzip
	 * being 31 bytes so don't bother trying to compress anything less
	 * than 64 bytes. */
	if (!NO_COMPRESS && cti->c_len >= 64) {
		if (LZMA_COMPRESS)
			ret = lzma_compress_buf(control, cti);
		else if (LZO_COMPRESS)
			ret = lzo_compress_buf(control, cti);
		else if (BZIP2_COMPRESS)
			ret = bzip2_compress_buf(control, cti);
		else if (ZLIB_COMPRESS)
			ret = gzip_compress_buf(control, cti);
		else if (ZPAQ_COMPRESS)
			ret = zpaq_compress_buf(control, cti, i);
		else {
			fatal_msg = "Dunno wtf compression to use!\n";
			goto out;
		}
	}

	padded_len = cti->c_len;
	if (!ret && padded_len < MIN_SIZE) {
		/* We need to pad out each block to at least be CBC_LEN bytes
		 * long or encryption cannot work. We pad it with random
		 * data */
		padded_len = MIN_SIZE;
		cti->s_buf = realloc(cti->s_buf, MIN_SIZE);
		if (unlikely(!cti->s_buf)) {
			fatal_msg = "Failed to realloc s_buf in compthread\n";
			goto out;
		}
		if (unlikely(!get_rand(control, cti->s_buf + cti->c_len, MIN_SIZE - cti->c_len)))
			goto out;
	}

	/* If compression fails for whatever reason multithreaded, then wait
	 * for the previous thread to finish, serialising the work to decrease
	 * the memory requirements, increasing the chance of success */
	if (unlikely(ret && waited)) {
		fatal_msg = "Failed to compress in compthread\n";
		goto out;
	}

	if (!waited) {
		lock_mutex(control, &output_lock);
		while (output_thread != i)
			cond_wait(control, &output_cond, &output_lock);
		unlock_mutex(control, &output_lock);
		waited = 1;
	}
	if (unlikely(ret)) {
		print_maxverbose("Unable to compress in parallel, waiting for previous thread to complete before trying again\n");
		goto retry;
	}

	/* Need to be big enough to fill one CBC_LEN */
	if (ENCRYPT)
		write_len = 8;
	else
		write_len = ctis->chunk_bytes;

	if (!ctis->chunks++) {
		int j;

		if (STDOUT) {
			lock_mutex(control, &control->control_lock);
			/* Magic only for the first block; may be rewritten until
			 * that block is flushed (lzma props). */
			if (!control->magic_written && !control->blocks_done)
				write_magic(control);
			/* Continuation streaming block: LRZC before RCD */
			if (control->blocks_done > 0) {
				if (unlikely(!write_lrzc_header(control, ctis->fd,
						ctis->size, control->eof))) {
					unlock_mutex(control, &control->control_lock);
					goto out;
				}
			}
			unlock_mutex(control, &control->control_lock);
		}

		print_maxverbose("Writing initial chunk bytes value %d at %"PRId64"\n",
				 ctis->chunk_bytes, get_seek(control, ctis->fd));
		/* Write chunk bytes of this block */
		write_u8(control, ctis->chunk_bytes);

		/* 0.7 chunk headers carry a prefilter byte
		 * (LRZ_FILTER_NONE/X86/ARM64) */
		write_u8(control, ctis->chunk_filter);

		/* Write whether this is the last chunk, followed by the size
		 * of this chunk. In streaming mode this matches block-last. */
		print_maxverbose("Writing EOF flag as %d\n", control->eof);
		write_u8(control, control->eof);
		if (!ENCRYPT)
			write_val(control, ctis->size, ctis->chunk_bytes);

		/* First chunk of this stream, write headers */
		ctis->initial_pos = get_seek(control, ctis->fd);
		if (unlikely(ctis->initial_pos == -1))
			goto out;

		print_maxverbose("Writing initial header at %"PRId64"\n", ctis->initial_pos);
		for (j = 0; j < ctis->num_streams; j++) {
			i64 pref = lrz_enc_prefix_len(control);
			i64 suf = lrz_enc_suffix_len(control);

			/* Room for salt (legacy/HMAC) or nonce (AEAD) before body */
			if (ENCRYPT) {
				if (unlikely(write_val(control, 0, pref))) {
					fatal_msg = "Failed to write blank salt/nonce in compthread\n";
					goto out;
				}
				ctis->cur_pos += pref;
			}
			ctis->s[j].last_head = ctis->cur_pos + 1 + (write_len * 2);
			write_u8(control, CTYPE_NONE);
			write_val(control, 0, write_len);
			write_val(control, 0, write_len);
			write_val(control, 0, write_len);
			ctis->cur_pos += 1 + (write_len * 3);
			/* Placeholder for HMAC or GCM tag after 25-byte body */
			if (ENCRYPT && suf) {
				if (unlikely(write_val(control, 0, suf))) {
					fatal_msg = "Failed to write blank header auth tag in compthread\n";
					goto out;
				}
				ctis->cur_pos += suf;
			}
		}
	}

	print_maxverbose("Compthread %ld seeking to %"PRId64" to store length %d\n", i, ctis->s[cti->streamno].last_head, write_len);

	if (unlikely(seekto(control, ctis, ctis->s[cti->streamno].last_head))) {
		fatal_msg = "Failed to seekto in compthread\n";
		goto out;
	}

	if (unlikely(write_val(control, ctis->cur_pos, write_len))) {
		fatal_msg = "Failed to write_val cur_pos in compthread\n";
		goto out;
	}

	if (ENCRYPT)
		rewrite_encrypted(control, ctis, ctis->s[cti->streamno].last_head - 17);

	ctis->s[cti->streamno].last_head = ctis->cur_pos + 1 + (write_len * 2) +
					   lrz_enc_prefix_len(control);

	print_maxverbose("Compthread %ld seeking to %"PRId64" to write header\n", i, ctis->cur_pos);

	if (unlikely(seekto(control, ctis, ctis->cur_pos))) {
		fatal_msg = "Failed to seekto cur_pos in compthread\n";
		goto out;
	}

	print_maxverbose("Thread %ld writing %"PRId64"/%"PRId64" compressed bytes from stream %d\n",
			 i, cti->c_len, cti->s_len, cti->streamno);

	if (ENCRYPT) {
		i64 pref = lrz_enc_prefix_len(control);

		if (unlikely(write_val(control, 0, pref))) {
			fatal_msg = "Failed to write header salt/nonce in compthread\n";
			goto out;
		}
		ctis->cur_pos += pref;
		ctis->s[cti->streamno].last_headofs = ctis->cur_pos;
	}
	/* We store the actual c_len even though we might pad it out */
	if (unlikely(write_u8(control, cti->c_type) ||
		write_val(control, cti->c_len, write_len) ||
		write_val(control, cti->s_len, write_len) ||
		write_val(control, 0, write_len))) {
			fatal_msg = "Failed write in compthread\n";
			goto out;
	}
	ctis->cur_pos += 1 + (write_len * 3);

	/* Reserve space for GCM tag (filled in rewrite_encrypted). */
	if (ENCRYPT && lrz_enc_suffix_len(control)) {
		i64 suf = lrz_enc_suffix_len(control);

		if (unlikely(write_val(control, 0, suf))) {
			fatal_msg = "Failed to write blank header auth tag in compthread\n";
			goto out;
		}
		ctis->cur_pos += suf;
	}

	if (ENCRYPT_AEAD) {
		size_t slen = LRZ_AEAD_NONCE_LEN + (size_t)padded_len + LRZ_AEAD_TAG_LEN;
		uchar *sealed, aad[8];
		size_t aad_len = 0;

		sealed = malloc(slen);
		if (unlikely(!sealed)) {
			fatal_msg = "Failed to malloc AEAD payload buffer in compthread\n";
			goto out;
		}
		aead_fill_aad(control, 0x02, aad, &aad_len);
		if (unlikely(!lrz_aead_seal(control, LRZ_AEAD_KEY_DATA, aad, aad_len,
					    cti->s_buf, (size_t)padded_len, sealed, &slen))) {
			dealloc(sealed);
			fatal_msg = "Failed to AEAD-seal payload in compthread\n";
			goto out;
		}
		if (unlikely(write_buf(control, sealed, (i64)slen))) {
			dealloc(sealed);
			fatal_msg = "Failed to write AEAD payload in compthread\n";
			goto out;
		}
		ctis->cur_pos += (i64)slen;
		dealloc(sealed);
	} else if (ENCRYPT) {
		if (unlikely(!get_rand(control, cti->salt, SALT_LEN)))
			goto out;
		if (unlikely(write_buf(control, cti->salt, SALT_LEN))) {
			fatal_msg = "Failed to write block salt in compthread\n";
			goto out;
		}
		if (unlikely(!lrz_encrypt(control, cti->s_buf, padded_len, cti->salt)))
			goto out;
		ctis->cur_pos += SALT_LEN;

		print_maxverbose("Compthread %ld writing data at %"PRId64"\n", i, ctis->cur_pos);

		if (unlikely(write_buf(control, cti->s_buf, padded_len))) {
			fatal_msg = "Failed to write_buf s_buf in compthread\n";
			goto out;
		}
		ctis->cur_pos += padded_len;
	} else {
		print_maxverbose("Compthread %ld writing data at %"PRId64"\n", i, ctis->cur_pos);

		if (unlikely(write_buf(control, cti->s_buf, padded_len))) {
			fatal_msg = "Failed to write_buf s_buf in compthread\n";
			goto out;
		}
		ctis->cur_pos += padded_len;
	}

	dealloc(cti->s_buf);

out:
	/*
	 * Always take and release our place in the output order.
	 * - Success path already waited (waited==1) and wrote.
	 * - Failure before wait: wait now (no write), then advance.
	 * - Failure after wait: advance without a successful write.
	 * Without this, other workers block forever on output_thread.
	 */
	if (!turn_released) {
		lock_mutex(control, &output_lock);
		if (!waited) {
			while (output_thread != i)
				cond_wait(control, &output_cond, &output_lock);
			waited = 1;
		}
		/* We own the slot when output_thread == i */
		if (output_thread == i) {
			if (++output_thread == control->threads)
				output_thread = 0;
			cond_broadcast(control, &output_cond);
		}
		unlock_mutex(control, &output_lock);
		turn_released = 1;
	}

	if (cti->s_buf)
		dealloc(cti->s_buf);

	cksem_post(control, &cti->cksem);

	/* Fatal after releasing the chain so peers are not left blocked if
	 * fatal_exit is ever changed to not terminate the process. */
	if (fatal_msg)
		failure("%s", fatal_msg);

	return NULL;
}

static void clear_buffer(rzip_control *control, struct stream_info *sinfo, int streamno, int newbuf)
{
	pthread_t *threads = control->pthreads;
	stream_thread_struct *s;
	static int i = 0;

	/* Make sure this thread doesn't already exist */
	cksem_wait(control, &cthreads[i].cksem);

	cthreads[i].sinfo = sinfo;
	cthreads[i].streamno = streamno;
	cthreads[i].s_buf = sinfo->s[streamno].buf;
	cthreads[i].s_len = sinfo->s[streamno].buflen;

	print_maxverbose("Starting thread %d to compress %"PRId64" bytes from stream %d\n",
			 i, cthreads[i].s_len, streamno);

	s = malloc(sizeof(stream_thread_struct));
	if (unlikely(!s)) {
		cksem_post(control, &cthreads[i].cksem);
		failure("Unable to malloc in clear_buffer");
	}
	s->i = i;
	s->control = control;
	if (unlikely((!create_pthread(control, &threads[i], NULL, compthread, s)) ||
	             (!detach_pthread(control, &threads[i]))))
		failure("Unable to create compthread in clear_buffer");

	if (newbuf) {
		/* The stream buffer has been given to the thread, allocate a
		 * new one. */
		sinfo->s[streamno].buf = malloc(sinfo->bufsize);
		if (unlikely(!sinfo->s[streamno].buf))
			failure("Unable to malloc buffer of size %"PRId64" in flush_buffer\n", sinfo->bufsize);
		sinfo->s[streamno].buflen = 0;
	}

	if (++i == control->threads)
		i = 0;
}

/* flush out any data in a stream buffer */
void flush_buffer(rzip_control *control, struct stream_info *sinfo, int streamno)
{
	clear_buffer(control, sinfo, streamno, 1);
}

static void *ucompthread(void *data)
{
	stream_thread_struct *sts = data;
	rzip_control *control = sts->control;
	int waited = 0, ret = 0, i = sts->i;
	struct uncomp_thread *uci = &sts->sinfo->ucthreads[i];

	dealloc(data);

	if (unlikely(setpriority(PRIO_PROCESS, 0, control->nice_val) == -1)) {
		print_err("Warning, unable to set thread nice value %d...Resetting to %d\n", control->nice_val, control->current_priority);
		setpriority(PRIO_PROCESS, 0, (control->nice_val=control->current_priority));
	}

retry:
	if (uci->c_type != CTYPE_NONE) {
		switch (uci->c_type) {
			case CTYPE_LZMA:
				ret = lzma_decompress_buf(control, uci);
				break;
			case CTYPE_LZMA_BCJ:
			case CTYPE_LZMA_BCJ_ARM64:
			case CTYPE_LZMA_DELTA1:
			case CTYPE_LZMA_DELTA2:
			case CTYPE_LZMA_DELTA3:
			case CTYPE_LZMA_DELTA4:
				ret = lzma_decompress_buf(control, uci);
				if (!ret)
					lrz_filter_convert_mem(uci->s_buf, uci->u_len,
							       ctype_filter_kind(uci->c_type), false);
				break;
			case CTYPE_LZO:
				ret = lzo_decompress_buf(control, uci);
				break;
			case CTYPE_BZIP2:
				ret = bzip2_decompress_buf(control, uci);
				break;
			case CTYPE_GZIP:
				ret = gzip_decompress_buf(control, uci);
				break;
			case CTYPE_ZPAQ:
				ret = zpaq_decompress_buf(control, uci, i);
				break;
			default:
				failure_return(("Dunno wtf decompression type to use!\n"), NULL);
				break;
		}
	}

	/* As per compression, serialise the decompression if it fails in
	 * parallel */
	if (unlikely(ret)) {
		if (unlikely(waited))
			failure_return(("Failed to decompress in ucompthread\n"), (void*)1);
		print_maxverbose("Unable to decompress in parallel, waiting for previous thread to complete before trying again\n");
		/* We do not strictly need to wait for this, so it's used when
		 * decompression fails due to inadequate memory to try again
		 * serialised. */
		lock_mutex(control, &output_lock);
		while (output_thread != i)
			cond_wait(control, &output_cond, &output_lock);
		unlock_mutex(control, &output_lock);
		waited = 1;
		goto retry;
	}

	print_maxverbose("Thread %d decompressed %"PRId64" bytes from stream %d\n", i, uci->u_len, uci->streamno);

	return NULL;
}

/* fill a buffer from a stream - return -1 on failure */
static int fill_buffer(rzip_control *control, struct stream_info *sinfo, struct stream *s, int streamno)
{
	i64 u_len, c_len, last_head, padded_len, header_length = 0, max_len;
	uchar enc_head[LRZ_AEAD_NONCE_LEN + 25 + LRZ_AEAD_TAG_LEN], blocksalt[SALT_LEN];
	struct uncomp_thread *ucthreads = sinfo->ucthreads;
	pthread_t *threads = control->pthreads;
	stream_thread_struct *sts;
	uchar c_type, *s_buf;
	void *thr_return;

	dealloc(s->buf);
	s->buf = NULL;
	s->buflen = 0;
	s->bufp = 0;
	/*
	 * eos means no further compressed blocks will be started, but
	 * prefetched ucomp threads may still hold decompressed data.
	 * Drain those while busy; only then return empty. Returning empty
	 * as soon as eos is set drops the rest of the stream (literals
	 * short-read and match offsets go past the truncated output).
	 */
	if (s->eos) {
		if (!ucthreads[s->unext_thread].busy)
			return 0;
		goto out;
	}
fill_another:
	if (unlikely(ucthreads[s->uthread_no].busy))
		failure_return(("Trying to start a busy thread, this shouldn't happen!\n"), -1);

	if (unlikely(read_seekto(control, sinfo, s->last_head)))
		return -1;

	if (ENCRYPT) {
		i64 hlen = lrz_enc_header_disk_len(control);

		/* AEAD: nonce||ct||tag; legacy: salt||ct [||HMAC] */
		if (unlikely(hlen > (i64)sizeof(enc_head)))
			return -1;
		if (unlikely(read_buf(control, sinfo->fd, enc_head, hlen)))
			return -1;
		sinfo->total_read += hlen;
		if (unlikely(!decrypt_header(control, enc_head, &c_type, &c_len,
					     &u_len, &last_head)))
			return -1;
		if (!ENCRYPT_AEAD) {
			if (unlikely(read_buf(control, sinfo->fd, blocksalt, SALT_LEN)))
				return -1;
			sinfo->total_read += SALT_LEN;
		}
	} else if (control->major_version == 0 && control->minor_version < 4) {
		u32 c_len32, u_len32, last_head32;

		if (unlikely(read_u8(control, sinfo->fd, &c_type)))
			return -1;
		if (unlikely(read_u32(control, sinfo->fd, &c_len32)))
			return -1;
		if (unlikely(read_u32(control, sinfo->fd, &u_len32)))
			return -1;
		if (unlikely(read_u32(control, sinfo->fd, &last_head32)))
			return -1;
		c_len = c_len32;
		u_len = u_len32;
		last_head = last_head32;
		header_length = 13;
		sinfo->total_read += header_length;
	} else {
		int read_len;

		if (unlikely(read_u8(control, sinfo->fd, &c_type)))
			return -1;
		print_maxverbose("Reading ucomp header at %"PRId64"\n", get_readseek(control, sinfo->fd));
		if (control->major_version == 0 && control->minor_version < 6)
			read_len = 8;
		else
			read_len = sinfo->chunk_bytes;
		if (unlikely(read_val(control, sinfo->fd, &c_len, read_len)))
			return -1;
		if (unlikely(read_val(control, sinfo->fd, &u_len, read_len)))
			return -1;
		if (unlikely(read_val(control, sinfo->fd, &last_head, read_len)))
			return -1;
		header_length = 1 + (read_len * 3);
		sinfo->total_read += header_length;
	}
	c_len = le64toh(c_len);
	u_len = le64toh(u_len);
	last_head = le64toh(last_head);
	print_maxverbose("Fill_buffer stream %d c_len %"PRId64" u_len %"PRId64" last_head %"PRId64"\n", streamno, c_len, u_len, last_head);

	/* It is possible for there to be an empty match block at the end of
	 * incompressible data. Encrypted writers still emit data salt +
	 * CBC_LEN pad, or AEAD nonce||ct||tag; consume them so the next
	 * RCD stays aligned. */
	if (unlikely(c_len == 0 && u_len == 0 && streamno == 1 && last_head == 0)) {
		print_maxverbose("Skipping empty match block\n");
		if (ENCRYPT_AEAD) {
			/* payload already not read; wire is nonce||0||tag */
			i64 rem = LRZ_AEAD_NONCE_LEN + LRZ_AEAD_TAG_LEN;
			uchar *throw = malloc((size_t)rem);

			if (unlikely(!throw))
				fatal_return(("Failed to malloc empty AEAD pad\n"), -1);
			if (unlikely(read_buf(control, sinfo->fd, throw, rem))) {
				dealloc(throw);
				return -1;
			}
			sinfo->total_read += rem;
			dealloc(throw);
		} else if (ENCRYPT) {
			i64 pad = MIN_SIZE; /* CBC_LEN when encrypting */
			uchar *throw;

			/* blocksalt already read into blocksalt[] above */
			throw = malloc((size_t)pad);
			if (unlikely(!throw))
				fatal_return(("Failed to malloc empty-block pad\n"), -1);
			if (unlikely(read_buf(control, sinfo->fd, throw, pad))) {
				dealloc(throw);
				return -1;
			}
			sinfo->total_read += pad;
			dealloc(throw);
		}
		goto skip_empty;
	}

	/* Check for invalid data and that the last_head is actually moving
	 * forward correctly. */
	if (unlikely(c_len < 1 || u_len < 1 || last_head < 0 || (last_head && last_head <= s->last_head))) {
		fatal_return(("Invalid data compressed len %"PRId64" uncompressed %"PRId64" last_head %"PRId64"\n",
			     c_len, u_len, last_head), -1);
	}
	/* last_head is relative to initial_pos; must stay inside the frame. */
	if (last_head) {
		i64 abs_head = sinfo->initial_pos + last_head;

		if (sinfo->payload_end > 0 && abs_head >= sinfo->payload_end) {
			fatal_return(("last_head %"PRId64" past block end %"PRId64"\n",
				     abs_head, sinfo->payload_end), -1);
		}
		if (sinfo->infile_size > 0 && abs_head >= sinfo->infile_size) {
			fatal_return(("last_head %"PRId64" past archive size %"PRId64"\n",
				     abs_head, sinfo->infile_size), -1);
		}
	}

	/* Reject unknown types and CTYPE_NONE length mismatches (uninit leak). */
	if (unlikely(c_type != CTYPE_NONE && c_type != CTYPE_BZIP2 &&
		     c_type != CTYPE_LZO && c_type != CTYPE_LZMA &&
		     c_type != CTYPE_GZIP && c_type != CTYPE_ZPAQ &&
		     !(c_type >= CTYPE_LZMA_BCJ && c_type <= CTYPE_LZMA_DELTA4))) {
		fatal_return(("Invalid compression type %d in stream block\n", c_type), -1);
	}
	if (unlikely(c_type == CTYPE_NONE && c_len != u_len)) {
		fatal_return(("Stored block c_len %"PRId64" != u_len %"PRId64"\n",
			     c_len, u_len), -1);
	}

	/* Cap both compressed and uncompressed sizes (malicious archives). */
	if (unlikely(!lrzip_size_ok(c_len, control->maxram) ||
		     !lrzip_size_ok(u_len, control->maxram))) {
		fatal_return(("Block size c_len %"PRId64" u_len %"PRId64" exceeds limits (maxram %"PRId64")\n",
			     c_len, u_len, control->maxram), -1);
	}
	/* bzip2 API uses 32-bit lengths */
	if (unlikely(c_type == CTYPE_BZIP2 &&
		     (u_len > (i64)UINT32_MAX || c_len > (i64)UINT32_MAX))) {
		fatal_return(("bzip2 block length exceeds 32-bit API limit\n"), -1);
	}

	padded_len = MAX(c_len, MIN_SIZE);
	/* total_read already includes this header (+ header salt if any).
	 * Remaining for this block depends on encrypt mode. */
	{
		i64 rem = padded_len;

		if (ENCRYPT_AEAD)
			rem = LRZ_AEAD_NONCE_LEN + padded_len + LRZ_AEAD_TAG_LEN;
		else if (ENCRYPT)
			rem += SALT_LEN;
		if (sinfo->payload_end > 0 &&
		    sinfo->initial_pos + sinfo->total_read + rem > sinfo->payload_end) {
			fatal_return(("Stream block would exceed framed c_size\n"), -1);
		}
	}
	/* No fsync of fd_out here: same-inode writes are visible to fd_hist via
	 * the page cache without forcing dirty data to disk each block. */

	max_len = MAX(u_len, MIN_SIZE);
	max_len = MAX(max_len, c_len);
	if (unlikely(!lrzip_size_ok(max_len, control->maxram))) {
		fatal_return(("Unable to allocate enough memory for %"PRId64" specified in possibly corrupt archive\n", max_len), -1);
	}
	s_buf = malloc((size_t)max_len);
	if (unlikely(!s_buf))
		fatal_return(("Unable to malloc buffer of size %"PRId64" in fill_buffer\n", max_len), -1);
	/* Count full allocation toward prefetch budget (not just u_len). */
	sinfo->ram_alloced += max_len;

	if (ENCRYPT_AEAD) {
		size_t slen = LRZ_AEAD_NONCE_LEN + (size_t)padded_len + LRZ_AEAD_TAG_LEN;
		size_t pt_len = (size_t)max_len;
		uchar *sealed, aad[8];
		size_t aad_len = 0;

		sealed = malloc(slen);
		if (unlikely(!sealed)) {
			dealloc(s_buf);
			sinfo->ram_alloced -= max_len;
			fatal_return(("Unable to malloc AEAD ciphertext in fill_buffer\n"), -1);
		}
		if (unlikely(read_buf(control, sinfo->fd, sealed, (i64)slen))) {
			dealloc(sealed);
			dealloc(s_buf);
			sinfo->ram_alloced -= max_len;
			return -1;
		}
		sinfo->total_read += (i64)slen;
		aead_fill_aad(control, 0x02, aad, &aad_len);
		if (unlikely(!lrz_aead_open(control, LRZ_AEAD_KEY_DATA, aad, aad_len,
					    sealed, slen, s_buf, &pt_len))) {
			dealloc(sealed);
			dealloc(s_buf);
			sinfo->ram_alloced -= max_len;
			failure_return(("Payload AEAD check failed (corrupt or wrong password)\n"), -1);
		}
		dealloc(sealed);
	} else {
		if (unlikely(read_buf(control, sinfo->fd, s_buf, padded_len))) {
			dealloc(s_buf);
			sinfo->ram_alloced -= max_len;
			return -1;
		}
		sinfo->total_read += padded_len;

		if (unlikely(ENCRYPT && !lrz_decrypt(control, s_buf, padded_len, blocksalt))) {
			dealloc(s_buf);
			sinfo->ram_alloced -= max_len;
			return -1;
		}
	}

	ucthreads[s->uthread_no].s_buf = s_buf;
	ucthreads[s->uthread_no].c_len = c_len;
	ucthreads[s->uthread_no].u_len = u_len;
	ucthreads[s->uthread_no].m_alloced = max_len;
	ucthreads[s->uthread_no].c_type = c_type;
	ucthreads[s->uthread_no].streamno = streamno;
	s->last_head = last_head;

	/* List this thread as busy */
	ucthreads[s->uthread_no].busy = 1;
	print_maxverbose("Starting thread %ld to decompress %"PRId64" bytes from stream %d\n",
			 s->uthread_no, padded_len, streamno);

	sts = malloc(sizeof(stream_thread_struct));
	if (unlikely(!sts)) {
		ucthreads[s->uthread_no].busy = 0;
		ucthreads[s->uthread_no].s_buf = NULL;
		ucthreads[s->uthread_no].m_alloced = 0;
		dealloc(s_buf);
		sinfo->ram_alloced -= max_len;
		fatal_return(("Unable to malloc in fill_buffer"), -1);
	}
	sts->i = s->uthread_no;
	sts->control = control;
	sts->sinfo = sinfo;
	if (unlikely(!create_pthread(control, &threads[s->uthread_no], NULL, ucompthread, sts))) {
		ucthreads[s->uthread_no].busy = 0;
		ucthreads[s->uthread_no].s_buf = NULL;
		ucthreads[s->uthread_no].m_alloced = 0;
		dealloc(sts);
		dealloc(s_buf);
		sinfo->ram_alloced -= max_len;
		return -1;
	}

	if (++s->uthread_no == s->base_thread + s->total_threads)
		s->uthread_no = s->base_thread;
skip_empty:
	/* Reached the end of this stream, no more data to read in, otherwise
	 * see if the next thread is free to grab more data. We also check that
	 * we're not going to be allocating too much ram to generate all these
	 * threads. */
	if (!last_head)
		s->eos = 1;
	else if (s->uthread_no != s->unext_thread && !ucthreads[s->uthread_no].busy &&
		 sinfo->ram_alloced < control->maxram)
			goto fill_another;
out:
	lock_mutex(control, &output_lock);
	output_thread = s->unext_thread;
	cond_broadcast(control, &output_cond);
	unlock_mutex(control, &output_lock);

	/* join_pthread here will make it wait till the data is ready */
	thr_return = NULL;
	if (unlikely(!join_pthread(control, threads[s->unext_thread], &thr_return) || !!thr_return))
		return -1;
	ucthreads[s->unext_thread].busy = 0;

	print_maxverbose("Taking decompressed data from thread %ld\n", s->unext_thread);
	s->buf = ucthreads[s->unext_thread].s_buf;
	ucthreads[s->unext_thread].s_buf = NULL;
	s->buflen = ucthreads[s->unext_thread].u_len;
	sinfo->ram_alloced -= ucthreads[s->unext_thread].m_alloced;
	ucthreads[s->unext_thread].m_alloced = 0;
	s->bufp = 0;

	if (++s->unext_thread == s->base_thread + s->total_threads)
		s->unext_thread = s->base_thread;

	return 0;
}

/* write some data to a stream. Return -1 on failure */
void write_stream(rzip_control *control, void *ss, int streamno, uchar *p, i64 len)
{
	struct stream_info *sinfo = ss;

	while (len) {
		i64 n;

		n = MIN(sinfo->bufsize - sinfo->s[streamno].buflen, len);

		memcpy(sinfo->s[streamno].buf + sinfo->s[streamno].buflen, p, n);
		sinfo->s[streamno].buflen += n;
		p += n;
		len -= n;

		/* Flush the buffer every sinfo->bufsize into one thread */
		if (sinfo->s[streamno].buflen == sinfo->bufsize)
			flush_buffer(control, sinfo, streamno);
	}
}

/* read some data from a stream. Return number of bytes read, or -1
   on failure */
i64 read_stream(rzip_control *control, void *ss, int streamno, uchar *p, i64 len)
{
	struct stream_info *sinfo = ss;
	struct stream *s = &sinfo->s[streamno];
	i64 ret = 0;

	while (len) {
		i64 n;

		n = MIN(s->buflen - s->bufp, len);

		if (n > 0) {
			if (unlikely(!s->buf))
				failure_return(("Stream ran out prematurely, likely corrupt archive\n"), -1);
			memcpy(p, s->buf + s->bufp, n);
			s->bufp += n;
			p += n;
			len -= n;
			ret += n;
		}

		if (len && s->bufp == s->buflen) {
			if (unlikely(fill_buffer(control, sinfo, s, streamno)))
				return -1;
			if (s->bufp == s->buflen)
				break;
		}
	}

	return ret;
}

/* flush and close down a stream. return -1 on failure */
int close_stream_out(rzip_control *control, void *ss)
{
	struct stream_info *sinfo = ss;
	int i;

	for (i = 0; i < sinfo->num_streams; i++)
		clear_buffer(control, sinfo, i, 0);

	if (ENCRYPT) {
		/* Last two compressed blocks do not have an offset written
		 * to them so we have to go back and encrypt them now, but we
		 * must wait till the threads return. */
		int close_thread = output_thread;

		for (i = 0; i < control->threads; i++) {
			cksem_wait(control, &cthreads[close_thread].cksem);
			cksem_post(control, &cthreads[close_thread].cksem);
			if (++close_thread == control->threads)
				close_thread = 0;
		}
		for (i = 0; i < sinfo->num_streams; i++)
			rewrite_encrypted(control, sinfo, sinfo->s[i].last_headofs);
	}

	/* Note that sinfo->s and sinfo are not released here but after compression
	 * has completed as they cannot be freed immediately because their values
	 * are read after the next stream has started.
	 */

	return 0;
}

/* Add to an runzip list to safely deallocate memory after all threads have
 * returned. */
static void add_to_rulist(rzip_control *control, struct stream_info *sinfo)
{
	struct runzip_node *node = calloc(1, sizeof(struct runzip_node));

	if (unlikely(!node))
		failure("Failed to calloc struct node in add_rulist\n");
	node->sinfo = sinfo;
	node->pthreads = control->pthreads;

	lock_mutex(control, &control->control_lock);
	node->prev = control->ruhead;
	control->ruhead = node;
	unlock_mutex(control, &control->control_lock);
}

/* close down an input stream */
int close_stream_in(rzip_control *control, void *ss)
{
	struct stream_info *sinfo = ss;
	int i;

	print_maxverbose("Closing stream at %"PRId64", want to seek to %"PRId64"\n",
			 get_readseek(control, control->fd_in),
			 sinfo->initial_pos + sinfo->total_read);
	if (unlikely(read_seekto(control, sinfo, sinfo->total_read)))
		return -1;

	/* LRZC c_size must match bytes consumed from RCD start through streams. */
	if (sinfo->payload_end > 0) {
		i64 end = sinfo->initial_pos + sinfo->total_read;

		if (unlikely(end != sinfo->payload_end)) {
			print_err("Block frame size mismatch: end %"PRId64" expected %"PRId64"\n",
				  end, sinfo->payload_end);
			return -1;
		}
	}

	for (i = 0; i < sinfo->num_streams; i++)
		dealloc(sinfo->s[i].buf);

	output_thread = 0;
	/* We cannot safely release the sinfo and pthread data here till all
	 * threads are shut down. */
	add_to_rulist(control, sinfo);

	return 0;
}

/* As others are slow and lz4 very fast, it is worth doing a quick lz4 pass
   to see if there is any compression at all with lz4 first. It is unlikely
   that others will be able to compress if lz4 is unable to drop a single byte
   so do not compress any block that is incompressible by lz4.
   The test runs only on the first backend block that reaches here; the
   result is reused for all later blocks in this compression job. */
static int lz4_compresses(rzip_control *control, uchar *s_buf, i64 s_len)
{
	int dlen, test_len;
	char *c_buf = NULL, *test_buf = (char *)s_buf;
	int ret = 0;
	int workcounter = 0;	/* count # of passes */
	int best_dlen = INT_MAX; /* save best compression estimate */

	if (!LZ4_TEST)
		return 1;

	/* Serialise the one-shot test so only a single block is measured. */
	lock_mutex(control, &control->control_lock);
	if (control->lz4_test_done) {
		ret = control->lz4_compressible ? 1 : 0;
		unlock_mutex(control, &control->control_lock);
		return ret;
	}

	dlen = MIN(s_len, STREAM_BUFSIZE);
	test_len = MIN(dlen, STREAM_BUFSIZE >> 8);
	c_buf = malloc(dlen);
	if (unlikely(!c_buf)) {
		unlock_mutex(control, &control->control_lock);
		fatal_return(("Unable to allocate c_buf in lz4_compresses\n"), 0);
	}

	/* Test progressively larger blocks at a time and as soon as anything
	   compressible is found, jump out as a success */
	do {
		int lz4_ret;

		workcounter++;
		lz4_ret = LZ4_compress_default((const char *)test_buf, c_buf, test_len, dlen);
		if (!lz4_ret) // Bigger than dlen
			lz4_ret = test_len;
		if (lz4_ret < best_dlen)
			best_dlen = lz4_ret;
		if (lz4_ret < test_len) {
			ret = 1;
			break;
		}
		/* expand test length */
		test_len <<= 1;
	} while (test_len <= dlen);

	if (!ret)
		print_maxverbose("lz4 testing FAILED for chunk %ld. %d Passes (applied to all blocks)\n",
				 s_len, workcounter);
	else {
		print_maxverbose("lz4 testing OK for chunk %ld. Compressed size = %5.2F%% of chunk, %d Passes (applied to all blocks)\n",
				s_len, 100 * ((double) best_dlen / (double) test_len), workcounter);
	}

	dealloc(c_buf);

	control->lz4_compressible = ret ? true : false;
	control->lz4_test_done = true;
	unlock_mutex(control, &control->control_lock);

	return ret;
}
