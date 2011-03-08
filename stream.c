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
/* multiplex N streams into a file - the streams are passed
   through different compressors */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "rzip.h"
#include "util.h"
#include "zpipe.h"
#include "liblrzip.h"

#define STREAM_BUFSIZE (1024 * 1024 * 10)

static struct compress_thread{
	uchar *s_buf;	/* Uncompressed buffer -> Compressed buffer */
	uchar c_type;	/* Compression type */
	i64 s_len;	/* Data length uncompressed */
	i64 c_len;	/* Data length compressed */
	pthread_mutex_t mutex; /* This thread's mutex */
	struct stream_info *sinfo;
	int stream;
} *cthread;

static struct uncomp_thread{
	uchar *s_buf;
	i64 u_len, c_len;
	i64 last_head;
	uchar c_type;
	int busy;
	int stream;
} *ucthread;

static void init_mutex(pthread_mutex_t *mutex)
{
	if (unlikely(pthread_mutex_init(mutex, NULL)))
		fatal("pthread_mutex_init failed");
}

static void unlock_mutex(pthread_mutex_t *mutex)
{
	if (unlikely(pthread_mutex_unlock(mutex)))
		fatal("pthread_mutex_unlock failed");
}

static void lock_mutex(pthread_mutex_t *mutex)
{
	if (unlikely(pthread_mutex_lock(mutex)))
		fatal("pthread_mutex_lock failed");
}

static void cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
	if (unlikely(pthread_cond_wait(cond, mutex)))
		fatal("pthread_cond_wait failed");
}

static void cond_broadcast(pthread_cond_t *cond)
{
	if (unlikely(pthread_cond_broadcast(cond)))
		fatal("pthread_cond_broadcast failed");
}

void create_pthread(pthread_t  * thread, pthread_attr_t * attr,
	void * (*start_routine)(void *), void *arg)
{
	if (pthread_create(thread, attr, start_routine, arg))
		fatal("pthread_create");
}

void join_pthread(pthread_t th, void **thread_return)
{
	if (pthread_join(th, thread_return))
		fatal("pthread_join");
}

/* just to keep things clean, declare function here
 * but move body to the end since it's a work function
*/
static int lzo_compresses(rzip_control *control, uchar *s_buf, i64 s_len);

static inline FILE *fake_fmemopen(void *buf, size_t buflen, char *mode)
{
	FILE *in;

	if (unlikely(strcmp(mode, "r")))
		failure("fake_fmemopen only supports mode \"r\".");
	in = tmpfile();
	if (unlikely(!in))
		return NULL;
	if (unlikely(fwrite(buf, buflen, 1, in) != 1))
		return NULL;
	rewind(in);
        return in;
}

static inline FILE *fake_open_memstream(char **buf, size_t *length)
{
	FILE *out;

	if (unlikely(buf == NULL || length == NULL))
		failure("NULL parameter to fake_open_memstream");
	out = tmpfile();
	if (unlikely(!out))
	        return NULL;
	return out;
}

static inline int fake_open_memstream_update_buffer(FILE *fp, uchar **buf, size_t *length)
{
	long original_pos = ftell(fp);

	if (unlikely(fseek(fp, 0, SEEK_END)))
		return -1;
	*length = ftell(fp);
	rewind(fp);
	*buf = (uchar *)malloc(*length);
	if (unlikely(!*buf))
		return -1;
	if (unlikely(fread(*buf, *length, 1, fp) != 1))
		return -1;
	if (unlikely(fseek(fp, original_pos, SEEK_SET)))
		return -1;
	return 0;
}

/*
  ***** COMPRESSION FUNCTIONS *****

  ZPAQ, BZIP, GZIP, LZMA, LZO

  try to compress a buffer. If compression fails for whatever reason then
  leave uncompressed. Return the compression type in c_type and resulting
  length in c_len
*/

static int zpaq_compress_buf(rzip_control *control, struct compress_thread *cthread, long thread)
{
	uchar *c_buf = NULL;
	size_t dlen = 0;
	FILE *in, *out;

	if (!lzo_compresses(control, cthread->s_buf, cthread->s_len))
		return 0;

	in = fmemopen(cthread->s_buf, cthread->s_len, "r");
	if (unlikely(!in)) {
		print_err("Failed to fmemopen in zpaq_compress_buf\n");
		return -1;
	}
	out = open_memstream((char **)&c_buf, &dlen);
	if (unlikely(!out)) {
		fclose(in);
		print_maxverbose("Failed to open_memstream in zpaq_compress_buf\n");
		return -1;
	}

	zpipe_compress(in, out, control->msgout, cthread->s_len,
		       (int)(SHOW_PROGRESS), thread);

	if (unlikely(memstream_update_buffer(out, &c_buf, &dlen)))
	        fatal("Failed to memstream_update_buffer in zpaq_compress_buf");

	fclose(in);
	fclose(out);

	if (unlikely((i64)dlen >= cthread->c_len)) {
		print_maxverbose("Incompressible block\n");
		/* Incompressible, leave as CTYPE_NONE */
		free(c_buf);
		return 0;
	}

	cthread->c_len = dlen;
	free(cthread->s_buf);
	cthread->s_buf = c_buf;
	cthread->c_type = CTYPE_ZPAQ;
	return 0;
}

static int bzip2_compress_buf(rzip_control *control, struct compress_thread *cthread)
{
	u32 dlen = cthread->s_len;
	int bzip2_ret;
	uchar *c_buf;

	if (!lzo_compresses(control, cthread->s_buf, cthread->s_len))
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
		free(c_buf);
		return 0;
	}

	if (unlikely(bzip2_ret != BZ_OK)) {
		free(c_buf);
		print_maxverbose("BZ2 compress failed\n");
		return -1;
	}

	if (unlikely(dlen >= cthread->c_len)) {
		print_maxverbose("Incompressible block\n");
		/* Incompressible, leave as CTYPE_NONE */
		free(c_buf);
		return 0;
	}

	cthread->c_len = dlen;
	free(cthread->s_buf);
	cthread->s_buf = c_buf;
	cthread->c_type = CTYPE_BZIP2;
	return 0;
}

static int gzip_compress_buf(rzip_control *control, struct compress_thread *cthread)
{
	unsigned long dlen = cthread->s_len;
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
		free(c_buf);
		return 0;
	}

	if (unlikely(gzip_ret != Z_OK)) {
		free(c_buf);
		print_maxverbose("compress2 failed\n");
		return -1;
	}

	if (unlikely((i64)dlen >= cthread->c_len)) {
		print_maxverbose("Incompressible block\n");
		/* Incompressible, leave as CTYPE_NONE */
		free(c_buf);
		return 0;
	}

	cthread->c_len = dlen;
	free(cthread->s_buf);
	cthread->s_buf = c_buf;
	cthread->c_type = CTYPE_GZIP;
	return 0;
}

static int lzma_compress_buf(rzip_control *control, struct compress_thread *cthread)
{
	int lzma_level, lzma_ret;
	size_t prop_size = 5; /* return value for lzma_properties */
	uchar *c_buf;
	size_t dlen;

	if (!lzo_compresses(control, cthread->s_buf, cthread->s_len))
		return 0;

	/* only 7 levels with lzma, scale them */
	lzma_level = control->compression_level * 7 / 9 ? : 1;

	print_verbose("Starting lzma back end compression thread...\n");
retry:
	dlen = cthread->s_len;
	c_buf = malloc(dlen);
	if (!c_buf) {
		print_err("Unable to allocate c_buf in lzma_compress_buf\n");
		return -1;
	}

	/* with LZMA SDK 4.63, we pass compression level and threads only
	 * and receive properties in control->lzma_properties */

	lzma_ret = LzmaCompress(c_buf, &dlen, cthread->s_buf,
		(size_t)cthread->s_len, control->lzma_properties, &prop_size,
				lzma_level,
				0, /* dict size. set default, choose by level */
				-1, -1, -1, -1, /* lc, lp, pb, fb */
				control->threads);
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
		free(c_buf);
		if (lzma_ret == SZ_ERROR_MEM) {
			if (lzma_level > 1) {
				lzma_level--;
				print_verbose("LZMA Warning: %d. Can't allocate enough RAM for compression window, trying smaller.\n", SZ_ERROR_MEM);
				goto retry;
			}
			/* lzma compress can be fragile on 32 bit. If it fails,
			 * fall back to bzip2 compression so the block doesn't
			 * remain uncompressed */
			print_verbose("Unable to allocate enough RAM for any sized compression window, falling back to bzip2 compression.\n");
			return bzip2_compress_buf(control, cthread);
		} else if (lzma_ret == SZ_ERROR_OUTPUT_EOF)
			return 0;
		return -1;
	}

	if (unlikely((i64)dlen >= cthread->c_len)) {
		/* Incompressible, leave as CTYPE_NONE */
		print_maxverbose("Incompressible block\n");
		free(c_buf);
		return 0;
	}

	cthread->c_len = dlen;
	free(cthread->s_buf);
	cthread->s_buf = c_buf;
	cthread->c_type = CTYPE_LZMA;
	return 0;
}

static int lzo_compress_buf(rzip_control *control, struct compress_thread *cthread)
{
	lzo_uint in_len = cthread->s_len;
	lzo_uint dlen = in_len + in_len / 16 + 64 + 3;
	lzo_int return_var;	/* lzo1x_1_compress does not return anything but LZO_OK */
	lzo_bytep wrkmem;
	uchar *c_buf;
	int ret = -1;

	wrkmem = (lzo_bytep) malloc(LZO1X_1_MEM_COMPRESS);
	if (unlikely(wrkmem == NULL)) {
		print_maxverbose("Failed to malloc wkmem\n");
		return ret;
	}

	c_buf = malloc(dlen);
	if (!c_buf) {
		print_err("Unable to allocate c_buf in lzo_compress_buf");
		goto out_free;
	}

	return_var = lzo1x_1_compress(cthread->s_buf, in_len, c_buf, &dlen, wrkmem);
	ret = 0;

	if (dlen >= in_len){
		/* Incompressible, leave as CTYPE_NONE */
		print_maxverbose("Incompressible block\n");
		free(c_buf);
		goto out_free;
	}

	cthread->c_len = dlen;
	free(cthread->s_buf);
	cthread->s_buf = c_buf;
	cthread->c_type = CTYPE_LZO;
out_free:
	free(wrkmem);
	return ret;
}

/*
  ***** DECOMPRESSION FUNCTIONS *****

  ZPAQ, BZIP, GZIP, LZMA, LZO

  try to decompress a buffer. Return 0 on success and -1 on failure.
*/

static int zpaq_decompress_buf(rzip_control *control, struct uncomp_thread *ucthread, long thread)
{
	uchar *c_buf = NULL;
	size_t dlen = 0;
	FILE *in, *out;

	in = fmemopen(ucthread->s_buf, ucthread->u_len, "r");
	if (unlikely(!in)) {
		print_err("Failed to fmemopen in zpaq_decompress_buf\n");
		return -1;
	}
	out = open_memstream((char **)&c_buf, &dlen);
	if (unlikely(!out)) {
		print_err("Failed to open_memstream in zpaq_decompress_buf\n");
		return -1;
	}

	zpipe_decompress(in, out, control->msgout, ucthread->u_len, (int)(SHOW_PROGRESS), thread);

	if (unlikely(memstream_update_buffer(out, &c_buf, &dlen)))
	        fatal("Failed to memstream_update_buffer in zpaq_decompress_buf");

	fclose(in);
	fclose(out);

	if (unlikely((i64)dlen != ucthread->u_len)) {
		print_err("Inconsistent length after decompression. Got %lld bytes, expected %lld\n", (i64)dlen, ucthread->u_len);
		return -1;
	}

	free(ucthread->s_buf);
	ucthread->s_buf = c_buf;

	return 0;
}

static int bzip2_decompress_buf(rzip_control *control __UNUSED__, struct uncomp_thread *ucthread)
{
	u32 dlen = ucthread->u_len;
	int ret = 0, bzerr;
	uchar *c_buf;

	c_buf = ucthread->s_buf;
	ucthread->s_buf = malloc(dlen);
	if (unlikely(!ucthread->s_buf)) {
		print_err("Failed to allocate %d bytes for decompression\n", dlen);
		ret = -1;
		goto out;
	}

	bzerr = BZ2_bzBuffToBuffDecompress((char*)ucthread->s_buf, &dlen, (char*)c_buf, ucthread->c_len, 0, 0);
	if (unlikely(bzerr != BZ_OK)) {
		print_err("Failed to decompress buffer - bzerr=%d\n", bzerr);
		ret = -1;
		goto out_free;
	}

	if (unlikely(dlen != ucthread->u_len)) {
		print_err("Inconsistent length after decompression. Got %d bytes, expected %lld\n", dlen, ucthread->u_len);
		ret = -1;
	}

out_free:
	free(c_buf);
out:
	if (ret == -1)
		ucthread->s_buf = c_buf;
	return ret;
}

static int gzip_decompress_buf(rzip_control *control __UNUSED__, struct uncomp_thread *ucthread)
{
	unsigned long dlen = ucthread->u_len;
	int ret = 0, gzerr;
	uchar *c_buf;

	c_buf = ucthread->s_buf;
	ucthread->s_buf = malloc(dlen);
	if (unlikely(!ucthread->s_buf)) {
		print_err("Failed to allocate %ld bytes for decompression\n", dlen);
		ret = -1;
		goto out;
	}

	gzerr = uncompress(ucthread->s_buf, &dlen, c_buf, ucthread->c_len);
	if (unlikely(gzerr != Z_OK)) {
		print_err("Failed to decompress buffer - bzerr=%d\n", gzerr);
		ret = -1;
		goto out_free;
	}

	if (unlikely((i64)dlen != ucthread->u_len)) {
		print_err("Inconsistent length after decompression. Got %ld bytes, expected %lld\n", dlen, ucthread->u_len);
		ret = -1;
	}

out_free:
	free(c_buf);
out:
	if (ret == -1)
		ucthread->s_buf = c_buf;
	return ret;
}

static int lzma_decompress_buf(rzip_control *control, struct uncomp_thread *ucthread)
{
	size_t dlen = (size_t)ucthread->u_len;
	int ret = 0, lzmaerr;
	uchar *c_buf;

	c_buf = ucthread->s_buf;
	ucthread->s_buf = malloc(dlen);
	if (unlikely(!ucthread->s_buf)) {
		print_err("Failed to allocate %lldd bytes for decompression\n", (i64)dlen);
		ret = -1;
		goto out;
	}

	/* With LZMA SDK 4.63 we pass control->lzma_properties
	 * which is needed for proper uncompress */
	lzmaerr = LzmaUncompress(ucthread->s_buf, &dlen, c_buf, (SizeT *)&ucthread->c_len, control->lzma_properties, 5);
	if (unlikely(lzmaerr)) {
		print_err("Failed to decompress buffer - lzmaerr=%d\n", lzmaerr);
		ret = -1;
		goto out_free;
	}

	if (unlikely((i64)dlen != ucthread->u_len)) {
		print_err("Inconsistent length after decompression. Got %lld bytes, expected %lld\n", (i64)dlen, ucthread->u_len);
		ret = -1;
	}

out_free:
	free(c_buf);
out:
	if (ret == -1)
		ucthread->s_buf = c_buf;
	return ret;
}

static int lzo_decompress_buf(rzip_control *control __UNUSED__, struct uncomp_thread *ucthread)
{
	lzo_uint dlen = ucthread->u_len;
	int ret = 0, lzerr;
	uchar *c_buf;

	c_buf = ucthread->s_buf;
	ucthread->s_buf = malloc(dlen);
	if (unlikely(!ucthread->s_buf)) {
		print_err("Failed to allocate %lu bytes for decompression\n", (unsigned long)dlen);
		ret = -1;
		goto out;
	}

	lzerr = lzo1x_decompress((uchar*)c_buf, ucthread->c_len, (uchar*)ucthread->s_buf, &dlen,NULL);
	if (unlikely(lzerr != LZO_E_OK)) {
		print_err("Failed to decompress buffer - lzerr=%d\n", lzerr);
		ret = -1;
		goto out_free;
	}

	if (unlikely((i64)dlen != ucthread->u_len)) {
		print_err("Inconsistent length after decompression. Got %lu bytes, expected %lld\n", (unsigned long)dlen, ucthread->u_len);
		ret = -1;
	}

out_free:
	free(c_buf);
out:
	if (ret == -1)
		ucthread->s_buf = c_buf;
	return ret;
}

/* WORK FUNCTIONS */

const i64 one_g = 1000 * 1024 * 1024;

/* This is a custom version of write() which writes in 1GB chunks to avoid
   the overflows at the >= 2GB mark thanks to 32bit fuckage. This should help
   even on the rare occasion write() fails to write 1GB as well. */
ssize_t write_1g(int fd, void *buf, i64 len)
{
	uchar *offset_buf = buf;
	i64 total, offset;
	ssize_t ret;

	total = offset = 0;
	while (len > 0) {
		if (len > one_g)
			ret = one_g;
		else
			ret = len;
		ret = write(fd, offset_buf, (size_t)ret);
		if (unlikely(ret <= 0))
			return ret;
		len -= ret;
		offset_buf += ret;
		total += ret;
	}
	return total;
}

/* Ditto for read */
ssize_t read_1g(int fd, void *buf, i64 len)
{
	uchar *offset_buf = buf;
	i64 total, offset;
	ssize_t ret;

	total = offset = 0;
	while (len > 0) {
		if (len > one_g)
			ret = one_g;
		else
			ret = len;
		ret = read(fd, offset_buf, (size_t)ret);
		if (unlikely(ret <= 0))
			return ret;
		len -= ret;
		offset_buf += ret;
		total += ret;
	}
	return total;
}

/* write to a file, return 0 on success and -1 on failure */
static int write_buf(int f, uchar *p, i64 len)
{
	ssize_t ret;

	ret = write_1g(f, p, (size_t)len);
	if (unlikely(ret == -1)) {
		print_err("Write of length %lld failed - %s\n", len, strerror(errno));
		return -1;
	}
	if (unlikely(ret != (ssize_t)len)) {
		print_err("Partial write!? asked for %lld bytes but got %lld\n", len, (i64)ret);
		return -1;
	}
	return 0;
}

/* write a byte */
static int write_u8(int f, uchar v)
{
	return write_buf(f, &v, 1);
}

/* write a i64 */
static int write_i64(int f, i64 v)
{
	if (unlikely(write_buf(f, (uchar *)&v, 8)))
		return -1;

	return 0;
}

static int read_buf(int f, uchar *p, i64 len)
{
	ssize_t ret;

	ret = read_1g(f, p, (size_t)len);
	if (unlikely(ret == -1)) {
		print_err("Read of length %lld failed - %s\n", len, strerror(errno));
		return -1;
	}
	if (unlikely(ret != (ssize_t)len)) {
		print_err("Partial read!? asked for %lld bytes but got %lld\n", len, (i64)ret);
		return -1;
	}
	return 0;
}

static int read_u8(int f, uchar *v)
{
	return read_buf(f, v, 1);
}

static int read_u32(int f, u32 *v)
{
	if (unlikely(read_buf(f, (uchar *)v, 4)))
		return -1;
	return 0;
}

static int read_i64(int f, i64 *v)
{
	if (unlikely(read_buf(f, (uchar *)v, 8)))
		return -1;
	return 0;
}

/* seek to a position within a set of streams - return -1 on failure */
static int seekto(struct stream_info *sinfo, i64 pos)
{
	i64 spos = pos + sinfo->initial_pos;

	if (unlikely(lseek(sinfo->fd, spos, SEEK_SET) != spos)) {
		print_err("Failed to seek to %lld in stream\n", pos);
		return -1;
	}
	return 0;
}

static pthread_t *threads;

void prepare_streamout_threads(rzip_control *control)
{
	int i;

	/* As we serialise the generation of threads during the rzip
	 * pre-processing stage, it's faster to have one more thread available
	 * to keep all CPUs busy. There is no point splitting up the chunks
	 * into multiple threads if there will be no compression back end. */
	if (control->threads > 1)
		++control->threads;
	if (NO_COMPRESS)
		control->threads = 1;
	threads = calloc(sizeof(pthread_t), control->threads);
	if (unlikely(!threads))
		fatal("Unable to calloc threads in prepare_streamout_threads\n");

	cthread = calloc(sizeof(struct compress_thread), control->threads);
	if (unlikely(!cthread))
		fatal("Unable to calloc cthread in prepare_streamout_threads\n");

	for (i = 0; i < control->threads; i++) 
		init_mutex(&cthread[i].mutex);
}

static long output_thread;
static pthread_mutex_t output_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t output_cond = PTHREAD_COND_INITIALIZER;

void close_streamout_threads(rzip_control *control)
{
	int i, close_thread = output_thread;

	/* Wait for the threads in the correct order in case they end up
	 * serialised */
	for (i = 0; i < control->threads; i++) {
		lock_mutex(&cthread[close_thread].mutex);
		if (++close_thread == control->threads)
			close_thread = 0;
	}
	free(cthread);
	free(threads);
}

/* open a set of output streams, compressing with the given
   compression level and algorithm */
void *open_stream_out(rzip_control *control, int f, unsigned int n, i64 chunk_limit, char cbytes)
{
	struct stream_info *sinfo;
	i64 testsize, limit;
	uchar *testmalloc;
	unsigned int i, testbufs;

	sinfo = calloc(sizeof(struct stream_info), 1);
	if (unlikely(!sinfo))
		return NULL;

	sinfo->bufsize = limit = chunk_limit;

	sinfo->chunk_bytes = cbytes;
	sinfo->num_streams = n;
	sinfo->fd = f;

	sinfo->s = calloc(sizeof(struct stream), n);
	if (unlikely(!sinfo->s)) {
		free(sinfo);
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

	/* Serious limits imposed on 32 bit capabilities */
	if (BITS32)
		limit = MIN((unsigned long long)limit, (two_gig / testbufs) -
			(control->overhead * control->threads));

	testsize = (limit * testbufs) + (control->overhead * control->threads);
	if (testsize > control->maxram)
		limit = (control->maxram - (control->overhead * control->threads)) / testbufs;

	/* If we don't have enough ram for the number of threads, decrease the
	 * number of threads till we do, or only have one thread. */
	while (limit < STREAM_BUFSIZE && limit < chunk_limit) {
		if (control->threads > 1)
			--control->threads;
		else
			break;
		limit = (control->maxram - (control->overhead * control->threads)) / testbufs;
		limit = MIN(limit, chunk_limit);
	}
retest_malloc:
	testsize = (limit * testbufs) + (control->overhead * control->threads);
	testmalloc = malloc(testsize);
	if (!testmalloc) {
		limit = limit / 10 * 9;
		goto retest_malloc;
	}
	free(testmalloc);
	print_maxverbose("Succeeded in testing %lld sized malloc for back end compression\n", testsize);

	/* Make the bufsize no smaller than STREAM_BUFSIZE. Round up the
	 * bufsize to fit X threads into it */
	sinfo->bufsize = MIN(limit, MAX((limit + control->threads - 1) / control->threads,
					STREAM_BUFSIZE));

	if (control->threads > 1)
		print_maxverbose("Using up to %d threads to compress up to %lld bytes each.\n",
			control->threads, sinfo->bufsize);
	else
		print_maxverbose("Using only 1 thread to compress up to %lld bytes\n",
			sinfo->bufsize);

	for (i = 0; i < n; i++) {
		sinfo->s[i].buf = malloc(sinfo->bufsize);
		if (unlikely(!sinfo->s[i].buf))
			fatal("Unable to malloc buffer of size %lld in open_stream_out\n", sinfo->bufsize);
	}

	return (void *)sinfo;
}

/* prepare a set of n streams for reading on file descriptor f */
void *open_stream_in(rzip_control *control, int f, int n)
{
	struct stream_info *sinfo;
	int total_threads, i;
	i64 header_length;

	sinfo = calloc(sizeof(struct stream_info), 1);
	if (unlikely(!sinfo))
		return NULL;

	/* We have one thread dedicated to stream 0, and one more thread than
	 * CPUs to keep them busy, unless we're running single-threaded. */
	if (control->threads > 1)
		total_threads = control->threads + 2;
	else
		total_threads = control->threads + 1;
	threads = calloc(sizeof(pthread_t), total_threads);
	if (unlikely(!threads))
		return NULL;

	ucthread = calloc(sizeof(struct uncomp_thread), total_threads);
	if (unlikely(!ucthread))
		fatal("Unable to calloc cthread in open_stream_in\n");

	sinfo->num_streams = n;
	sinfo->fd = f;
	sinfo->initial_pos = lseek(f, 0, SEEK_CUR);

	sinfo->s = calloc(sizeof(struct stream), n);
	if (unlikely(!sinfo->s)) {
		free(sinfo);
		return NULL;
	}

	sinfo->s[0].total_threads = 1;
	sinfo->s[1].total_threads = total_threads - 1;

	for (i = 0; i < n; i++) {
		uchar c;
		i64 v1, v2;

		sinfo->s[i].base_thread = i;
		sinfo->s[i].uthread_no = sinfo->s[i].base_thread;
		sinfo->s[i].unext_thread = sinfo->s[i].base_thread;

again:
		if (unlikely(read_u8(f, &c)))
			goto failed;

		/* Compatibility crap for versions < 0.40 */
		if (control->major_version == 0 && control->minor_version < 4) {
			u32 v132, v232, last_head32;

			if (unlikely(read_u32(f, &v132)))
				goto failed;
			if (unlikely(read_u32(f, &v232)))
				goto failed;
			if ((read_u32(f, &last_head32)))
				goto failed;

			v1 = v132;
			v2 = v232;
			sinfo->s[i].last_head = last_head32;
			header_length = 13;
		} else {
			if (unlikely(read_i64(f, &v1)))
				goto failed;
			if (unlikely(read_i64(f, &v2)))
				goto failed;
			if (unlikely(read_i64(f, &sinfo->s[i].last_head)))
				goto failed;
			header_length = 25;
		}

		if (unlikely(c == CTYPE_NONE && v1 == 0 && v2 == 0 && sinfo->s[i].last_head == 0 && i == 0)) {
			print_err("Enabling stream close workaround\n");
			sinfo->initial_pos += header_length;
			goto again;
		}

		sinfo->total_read += header_length;

		if (unlikely(c != CTYPE_NONE)) {
			print_err("Unexpected initial tag %d in streams\n", c);
			goto failed;
		}
		if (unlikely(v1)) {
			print_err("Unexpected initial c_len %lld in streams %lld\n", v1, v2);
			goto failed;
		}
		if (unlikely(v2)) {
			print_err("Unexpected initial u_len %lld in streams\n", v2);
			goto failed;
		}
	}

	return (void *)sinfo;

failed:
	free(sinfo->s);
	free(sinfo);
	return NULL;
}

/* Enter with s_buf allocated,s_buf points to the compressed data after the
 * backend compression and is then freed here */
static void *compthread(void *data)
{
	rzip_control *control = data;
	long i = (long)control->data;
	control->data = NULL;
	struct compress_thread *cti = &cthread[i];
	struct stream_info *ctis = cti->sinfo;
	int waited = 0, ret = 0;

	if (unlikely(setpriority(PRIO_PROCESS, 0, control->nice_val) == -1))
		print_err("Warning, unable to set nice value on thread\n");

	cti->c_type = CTYPE_NONE;
	cti->c_len = cti->s_len;

	/* Flushing writes to disk frees up any dirty ram, improving chances
	 * of succeeding in allocating more ram */
	fsync(ctis->fd);
retry:
	if (!NO_COMPRESS && cti->c_len) {
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
		else failure("Dunno wtf compression to use!\n");
	}

	/* If compression fails for whatever reason multithreaded, then wait
	 * for the previous thread to finish, serialising the work to decrease
	 * the memory requirements, increasing the chance of success */
	if (unlikely(ret && waited))
		failure("Failed to compress in compthread\n");

	if (!waited) {
		lock_mutex(&output_lock);
		while (output_thread != i)
			cond_wait(&output_cond, &output_lock);
		unlock_mutex(&output_lock);
		waited = 1;
	}
	if (unlikely(ret)) {
		print_maxverbose("Unable to compress in parallel, waiting for previous thread to complete before trying again\n");
		goto retry;
	}

	if (!ctis->chunks++) {
		int j;

		/* Write chunk bytes of this block */
		write_u8(ctis->fd, ctis->chunk_bytes);

		/* First chunk of this stream, write headers */
		ctis->initial_pos = lseek(ctis->fd, 0, SEEK_CUR);

		for (j = 0; j < ctis->num_streams; j++) {
			ctis->s[j].last_head = ctis->cur_pos + 17;
			write_u8(ctis->fd, CTYPE_NONE);
			write_i64(ctis->fd, 0);
			write_i64(ctis->fd, 0);
			write_i64(ctis->fd, 0);
			ctis->cur_pos += 25;
		}
	}

	if (unlikely(seekto(ctis, ctis->s[cti->stream].last_head)))
		fatal("Failed to seekto in compthread %d\n", i);

	if (unlikely(write_i64(ctis->fd, ctis->cur_pos)))
		fatal("Failed to write_i64 in compthread %d\n", i);

	ctis->s[cti->stream].last_head = ctis->cur_pos + 17;
	if (unlikely(seekto(ctis, ctis->cur_pos)))
		fatal("Failed to seekto cur_pos in compthread %d\n", i);

	print_maxverbose("Thread %ld writing %lld compressed bytes from stream %d\n", i, cti->c_len, cti->stream);
	if (unlikely(write_u8(ctis->fd, cti->c_type) ||
		write_i64(ctis->fd, cti->c_len) ||
		write_i64(ctis->fd, cti->s_len) ||
		write_i64(ctis->fd, 0))) {
			fatal("Failed write in compthread %d\n", i);
	}
	ctis->cur_pos += 25;

	if (unlikely(write_buf(ctis->fd, cti->s_buf, cti->c_len)))
		fatal("Failed to write_buf in compthread %d\n", i);

	ctis->cur_pos += cti->c_len;
	free(cti->s_buf);

	lock_mutex(&output_lock);
	if (++output_thread == control->threads)
		output_thread = 0;
	cond_broadcast(&output_cond);
	unlock_mutex(&output_lock);

	unlock_mutex(&cti->mutex);

	return 0;
}

static void clear_buffer(rzip_control *control, struct stream_info *sinfo, int stream, int newbuf)
{
	static long i = 0;

	/* Make sure this thread doesn't already exist */
	lock_mutex(&cthread[i].mutex);

	cthread[i].sinfo = sinfo;
	cthread[i].stream = stream;
	cthread[i].s_buf = sinfo->s[stream].buf;
	cthread[i].s_len = sinfo->s[stream].buflen;

	print_maxverbose("Starting thread %ld to compress %lld bytes from stream %d\n",
			 i, cthread[i].s_len, stream);
	control->data = (void*)i;
	create_pthread(&threads[i], NULL, compthread, control);

	if (newbuf) {
		/* The stream buffer has been given to the thread, allocate a new one */
		sinfo->s[stream].buf = malloc(sinfo->bufsize);
		if (unlikely(!sinfo->s[stream].buf))
			fatal("Unable to malloc buffer of size %lld in flush_buffer\n", sinfo->bufsize);
		sinfo->s[stream].buflen = 0;
	}

	if (++i == control->threads)
		i = 0;
}

/* flush out any data in a stream buffer */
void flush_buffer(rzip_control *control, struct stream_info *sinfo, int stream)
{
	clear_buffer(control, sinfo, stream, 1);
}

static void *ucompthread(void *data)
{
	rzip_control *control = data;
	long i = (long)control->data;
	control->data = NULL;
	struct uncomp_thread *uci = &ucthread[i];
	int waited = 0, ret = 0;

	if (unlikely(setpriority(PRIO_PROCESS, 0, control->nice_val) == -1))
		print_err("Warning, unable to set nice value on thread\n");

retry:
	if (uci->c_type != CTYPE_NONE) {
		switch (uci->c_type) {
			case CTYPE_LZMA:
				ret = lzma_decompress_buf(control, uci);
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
				failure("Dunno wtf decompression type to use!\n");
				break;
		}
	}

	/* As per compression, serialise the decompression if it fails in
	 * parallel */
	if (unlikely(ret)) {
		if (unlikely(waited))
			failure("Failed to decompress in ucompthread\n");
		print_maxverbose("Unable to decompress in parallel, waiting for previous thread to complete before trying again\n");
		/* We do not strictly need to wait for this, so it's used when
		 * decompression fails due to inadequate memory to try again
		 * serialised. */
		lock_mutex(&output_lock);
		while (output_thread != i)
			cond_wait(&output_cond, &output_lock);
		unlock_mutex(&output_lock);
		waited = 1;
		goto retry;
	}

	print_maxverbose("Thread %ld decompressed %lld bytes from stream %d\n", i, uci->u_len, uci->stream);

	return 0;
}

/* fill a buffer from a stream - return -1 on failure */
static int fill_buffer(rzip_control *control, struct stream_info *sinfo, int stream)
{
	i64 header_length, u_len, c_len, last_head;
	struct stream *s = &sinfo->s[stream];
	uchar c_type, *s_buf;

	if (s->buf)
		free(s->buf);
	if (s->eos)
		goto out;
fill_another:
	if (unlikely(ucthread[s->uthread_no].busy))
		failure("Trying to start a busy thread, this shouldn't happen!\n");

	if (unlikely(seekto(sinfo, s->last_head)))
		return -1;

	if (unlikely(read_u8(sinfo->fd, &c_type)))
		return -1;

	/* Compatibility crap for versions < 0.4 */
	if (control->major_version == 0 && control->minor_version < 4) {
		u32 c_len32, u_len32, last_head32;

		if (unlikely(read_u32(sinfo->fd, &c_len32)))
			return -1;
		if (unlikely(read_u32(sinfo->fd, &u_len32)))
			return -1;
		if (unlikely(read_u32(sinfo->fd, &last_head32)))
			return -1;
		c_len = c_len32;
		u_len = u_len32;
		last_head = last_head32;
		header_length = 13;
	} else {
		if (unlikely(read_i64(sinfo->fd, &c_len)))
			return -1;
		if (unlikely(read_i64(sinfo->fd, &u_len)))
			return -1;
		if (unlikely(read_i64(sinfo->fd, &last_head)))
			return -1;
		header_length = 25;
	}

	sinfo->total_read += header_length;

	fsync(control->fd_out);

	s_buf = malloc(u_len);
	if (unlikely(u_len && !s_buf))
		fatal("Unable to malloc buffer of size %lld in fill_buffer\n", u_len);
	sinfo->ram_alloced += u_len;

	if (unlikely(read_buf(sinfo->fd, s_buf, c_len)))
		return -1;

	sinfo->total_read += c_len;

	ucthread[s->uthread_no].s_buf = s_buf;
	ucthread[s->uthread_no].c_len = c_len;
	ucthread[s->uthread_no].u_len = u_len;
	ucthread[s->uthread_no].c_type = c_type;
	ucthread[s->uthread_no].stream = stream;
	s->last_head = last_head;

	/* List this thread as busy */
	ucthread[s->uthread_no].busy = 1;
	print_maxverbose("Starting thread %ld to decompress %lld bytes from stream %d\n",
			 s->uthread_no, c_len, stream);
	control->data = (void*)s->uthread_no;
	create_pthread(&threads[s->uthread_no], NULL, ucompthread, control);

	if (++s->uthread_no == s->base_thread + s->total_threads)
		s->uthread_no = s->base_thread;

	/* Reached the end of this stream, no more data to read in, otherwise
	 * see if the next thread is free to grab more data. We also check that
	 * we're not going to be allocating too much ram to generate all these
	 * threads. */
	if (!last_head)
		s->eos = 1;
	else if (s->uthread_no != s->unext_thread && !ucthread[s->uthread_no].busy &&
		 sinfo->ram_alloced < control->ramsize / 3)
			goto fill_another;
out:
	lock_mutex(&output_lock);
	output_thread = s->unext_thread;
	cond_broadcast(&output_cond);
	unlock_mutex(&output_lock);

	/* join_pthread here will make it wait till the data is ready */
	join_pthread(threads[s->unext_thread], NULL);
	ucthread[s->unext_thread].busy = 0;

	print_maxverbose("Taking decompressed data from thread %ld\n", s->unext_thread);
	s->buf = ucthread[s->unext_thread].s_buf;
	s->buflen = ucthread[s->unext_thread].u_len;
	sinfo->ram_alloced -= s->buflen;
	s->bufp = 0;

	if (++s->unext_thread == s->base_thread + s->total_threads)
		s->unext_thread = s->base_thread;

	return 0;
}

/* write some data to a stream. Return -1 on failure */
int write_stream(rzip_control *control, void *ss, int stream, uchar *p, i64 len)
{
	struct stream_info *sinfo = ss;

	while (len) {
		i64 n;

		n = MIN(sinfo->bufsize - sinfo->s[stream].buflen, len);

		memcpy(sinfo->s[stream].buf + sinfo->s[stream].buflen, p, n);
		sinfo->s[stream].buflen += n;
		p += n;
		len -= n;

		/* Flush the buffer every sinfo->bufsize into one thread */
		if (sinfo->s[stream].buflen == sinfo->bufsize)
			flush_buffer(control, sinfo, stream);
	}
	return 0;
}

/* read some data from a stream. Return number of bytes read, or -1
   on failure */
i64 read_stream(rzip_control *control, void *ss, int stream, uchar *p, i64 len)
{
	struct stream_info *sinfo = ss;
	i64 ret = 0;

	while (len) {
		i64 n;

		n = MIN(sinfo->s[stream].buflen - sinfo->s[stream].bufp, len);

		if (n > 0) {
			memcpy(p, sinfo->s[stream].buf + sinfo->s[stream].bufp, n);
			sinfo->s[stream].bufp += n;
			p += n;
			len -= n;
			ret += n;
		}

		if (len && sinfo->s[stream].bufp == sinfo->s[stream].buflen) {
			if (unlikely(fill_buffer(control, sinfo, stream)))
				return -1;
			if (sinfo->s[stream].bufp == sinfo->s[stream].buflen)
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

	for (i = 0; i < sinfo->num_streams; i++) {
		if (sinfo->s[i].buflen)
			clear_buffer(control, sinfo, i, 0);
	}

#if 0
	/* These cannot be freed because their values are read after the next
	 * stream has started so they're not properly freed and just dropped on
	 * program exit! FIXME */
	free(sinfo->s);
	free(sinfo);
#endif
	return 0;
}

/* close down an input stream */
int close_stream_in(void *ss)
{
	struct stream_info *sinfo = ss;
	int i;

	if (unlikely(lseek(sinfo->fd, sinfo->initial_pos + sinfo->total_read,
		  SEEK_SET) != sinfo->initial_pos + sinfo->total_read))
			return -1;
	for (i = 0; i < sinfo->num_streams; i++)
		free(sinfo->s[i].buf);

	output_thread = 0;
	free(ucthread);
	free(threads);
	free(sinfo->s);
	free(sinfo);

	return 0;
}

/* As others are slow and lzo very fast, it is worth doing a quick lzo pass
   to see if there is any compression at all with lzo first. It is unlikely
   that others will be able to compress if lzo is unable to drop a single byte
   so do not compress any block that is incompressible by lzo. */
static int lzo_compresses(rzip_control *control, uchar *s_buf, i64 s_len)
{
	lzo_bytep wrkmem = NULL;
	lzo_uint in_len, test_len = s_len, save_len = s_len;
	lzo_uint dlen;
	lzo_int return_var;	/* lzo1x_1_compress does not return anything but LZO_OK */
	uchar *c_buf = NULL, *test_buf = s_buf;
	/* set minimum buffer test size based on the length of the test stream */
	unsigned long buftest_size = (test_len > 5 * STREAM_BUFSIZE ? STREAM_BUFSIZE : STREAM_BUFSIZE / 4096);
	int ret = 0;
	int workcounter = 0;	/* count # of passes */
	lzo_uint best_dlen = UINT_MAX; /* save best compression estimate */

	if (!LZO_TEST)
		return 1;
	wrkmem = (lzo_bytep) malloc(LZO1X_1_MEM_COMPRESS);
	if (unlikely(wrkmem == NULL))
		fatal("Unable to allocate wrkmem in lzo_compresses\n");

	in_len = MIN(test_len, buftest_size);
	dlen = STREAM_BUFSIZE + STREAM_BUFSIZE / 16 + 64 + 3;

	c_buf = malloc(dlen);
	if (unlikely(!c_buf))
		fatal("Unable to allocate c_buf in lzo_compresses\n");

	/* Test progressively larger blocks at a time and as soon as anything
	   compressible is found, jump out as a success */
	while (test_len > 0) {
		workcounter++;
		return_var = lzo1x_1_compress(test_buf, in_len, (uchar *)c_buf, &dlen, wrkmem);

		if (dlen < best_dlen)
			best_dlen = dlen;	/* save best value */

		if (dlen < in_len) {
			ret = 1;
			break;
		}
		/* expand and move buffer */
		test_len -= in_len;
		if (test_len) {
			test_buf += (ptrdiff_t)in_len;
			if (buftest_size < STREAM_BUFSIZE)
				buftest_size <<= 1;
			in_len = MIN(test_len, buftest_size);
		}
	}
	print_maxverbose("lzo testing %s for chunk %ld. Compressed size = %5.2F%% of chunk, %d Passes\n",
			(ret == 0? "FAILED" : "OK"), save_len,
			100 * ((double) best_dlen / (double) in_len), workcounter);

	free(wrkmem);
	free(c_buf);

	return ret;
}
