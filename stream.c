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
/* multiplex N streams into a file - the streams are passed
   through different compressors */

#include "rzip.h"

#define STREAM_BUFSIZE (1024 * 1024 * 10)

struct compress_thread{
	uchar *s_buf;	/* Uncompressed buffer -> Compressed buffer */
	uchar c_type;	/* Compression type */
	i64 s_len;	/* Data length uncompressed */
	i64 c_len;	/* Data length compressed */
	sem_t complete;	/* Signal when this thread has finished */
	sem_t free;	/* This thread no longer exists */
	int wait_on;	/* Which thread has to complete before this can write its data */
	struct stream_info *sinfo;
	int stream;
} *cthread;

void init_sem(sem_t *sem)
{
	if (unlikely(sem_init(sem, 0, 0)))
		fatal("sem_init\n");
}

static inline void post_sem(sem_t *s)
{
retry:
	if (unlikely((sem_post(s)) == -1)) {
		if (errno == EINTR)
			goto retry;
		fatal("sem_post failed");
	}
}

static inline void wait_sem(sem_t *s)
{
retry:
	if (unlikely((sem_wait(s)) == -1)) {
		if (errno == EINTR)
			goto retry;
		fatal("sem_wait failed");
	}
}

static inline void destroy_sem(sem_t *s)
{
	if (unlikely(sem_destroy(s)))
		fatal("sem_destroy failed\n");
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
static int lzo_compresses(uchar *s_buf, i64 s_len);

static inline FILE *fake_fmemopen(void *buf, size_t buflen, char *mode)
{
	FILE *in;

	if (unlikely(strcmp(mode, "r")))
		fatal("fake_fmemopen only supports mode \"r\".");
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
		fatal("NULL parameter to fake_open_memstream");
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

static void zpaq_compress_buf(struct compress_thread *cthread)
{
	uchar *c_buf = NULL;
	size_t dlen = 0;
	FILE *in, *out;

	if (!lzo_compresses(cthread->s_buf, cthread->s_len))
		return;

	in = fmemopen(cthread->s_buf, cthread->s_len, "r");
	if (unlikely(!in))
		fatal("Failed to fmemopen in zpaq_compress_buf\n");
	out = open_memstream((char **)&c_buf, &dlen);
	if (unlikely(!out))
		fatal("Failed to open_memstream in zpaq_compress_buf\n");

	zpipe_compress(in, out, control.msgout, cthread->s_len, (int)(SHOW_PROGRESS));

	if (unlikely(memstream_update_buffer(out, &c_buf, &dlen)))
	        fatal("Failed to memstream_update_buffer in zpaq_compress_buf");

	fclose(in);
	fclose(out);

	if ((i64)dlen >= cthread->c_len) {
		print_maxverbose("Incompressible block\n");
		/* Incompressible, leave as CTYPE_NONE */
		free(c_buf);
		return;
	}

	cthread->c_len = dlen;
	free(cthread->s_buf);
	cthread->s_buf = c_buf;
	cthread->c_type = CTYPE_ZPAQ;
}

static void bzip2_compress_buf(struct compress_thread *cthread)
{
	u32 dlen = cthread->s_len;
	uchar *c_buf;

	if (!lzo_compresses(cthread->s_buf, cthread->s_len))
		return;

	c_buf = malloc(dlen);
	if (!c_buf)
		return;

	if (BZ2_bzBuffToBuffCompress((char *)c_buf, &dlen,
		(char *)cthread->s_buf, cthread->s_len,
		control.compression_level, 0, control.compression_level * 10) != BZ_OK) {
			free(c_buf);
			return;
	}

	if (dlen >= cthread->c_len) {
		print_maxverbose("Incompressible block\n");
		/* Incompressible, leave as CTYPE_NONE */
		free(c_buf);
		return;
	}

	cthread->c_len = dlen;
	free(cthread->s_buf);
	cthread->s_buf = c_buf;
	cthread->c_type = CTYPE_BZIP2;
}

static void gzip_compress_buf(struct compress_thread *cthread)
{
	unsigned long dlen = cthread->s_len;
	uchar *c_buf;

	c_buf = malloc(dlen);
	if (!c_buf)
		return;

	if (compress2(c_buf, &dlen, cthread->s_buf, cthread->s_len,
		control.compression_level) != Z_OK) {
			free(c_buf);
			return;
	}

	if ((i64)dlen >= cthread->c_len) {
		print_maxverbose("Incompressible block\n");
		/* Incompressible, leave as CTYPE_NONE */
		free(c_buf);
		return;
	}

	cthread->c_len = dlen;
	free(cthread->s_buf);
	cthread->s_buf = c_buf;
	cthread->c_type = CTYPE_GZIP;
}

static void lzma_compress_buf(struct compress_thread *cthread)
{
	size_t prop_size = 5; /* return value for lzma_properties */
	uchar *c_buf;
	size_t dlen;
	int lzma_ret;

	if (!lzo_compresses(cthread->s_buf, cthread->s_len))
		return;

	dlen = cthread->s_len;
	c_buf = malloc(dlen);
	if (!c_buf)
		return;

	print_progress("Starting lzma back end compression thread...\n");
	/* with LZMA SDK 4.63, we pass compression level and threads only
	 * and receive properties in control->lzma_properties */

	lzma_ret = LzmaCompress(c_buf, &dlen, cthread->s_buf,
		(size_t)cthread->s_len, control.lzma_properties, &prop_size, control.compression_level,
			      0, /* dict size. set default */
			      -1, -1, -1, -1, /* lc, lp, pb, fb */
			      control.threads);
	if (lzma_ret != SZ_OK) {
		switch (lzma_ret) {
			case SZ_ERROR_MEM:
				print_err("\nLZMA ERROR: %d. Try a smaller compression window.\n", SZ_ERROR_MEM);
				break;
			case SZ_ERROR_PARAM:
				print_err("\nLZMA Parameter ERROR: %d. This should not happen.\n", SZ_ERROR_PARAM);
				break;
			case SZ_ERROR_OUTPUT_EOF:
				print_maxverbose("\nHarmless LZMA Output Buffer Overflow error: %d. Incompressible block.\n", SZ_ERROR_OUTPUT_EOF);
				break;
			case SZ_ERROR_THREAD:
				print_err("\nLZMA Multi Thread ERROR: %d. This should not happen.\n", SZ_ERROR_THREAD);
				break;
			default:
				print_err("Unidentified LZMA ERROR: %d. This should not happen.\n", lzma_ret);
				break;
		}
		/* can pass -1 if not compressible! Thanks Lasse Collin */
		print_maxverbose("Incompressible block\n");
		free(c_buf);
		return;
	}
	if ((i64)dlen >= cthread->c_len) {
		/* Incompressible, leave as CTYPE_NONE */
		print_maxverbose("Incompressible block\n");
		free(c_buf);
		return;
	}

	cthread->c_len = dlen;
	free(cthread->s_buf);
	cthread->s_buf = c_buf;
	cthread->c_type = CTYPE_LZMA;
}

static void lzo_compress_buf(struct compress_thread *cthread)
{
	lzo_uint in_len = cthread->s_len;
	lzo_uint dlen = in_len + in_len / 16 + 64 + 3;
	lzo_int return_var;	/* lzo1x_1_compress does not return anything but LZO_OK */
	lzo_bytep wrkmem;
	uchar *c_buf;

	wrkmem = (lzo_bytep) malloc(LZO1X_1_MEM_COMPRESS);
	if (wrkmem == NULL)
		return;

	c_buf = malloc(dlen);
	if (!c_buf)
		goto out_free;

	return_var = lzo1x_1_compress(cthread->s_buf, in_len, c_buf, &dlen, wrkmem);

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
}

/*
  ***** DECOMPRESSION FUNCTIONS *****

  ZPAQ, BZIP, GZIP, LZMA, LZO

  try to decompress a buffer. Return 0 on success and -1 on failure.
*/

static int zpaq_decompress_buf(struct stream *s)
{
	uchar *c_buf = NULL;
	size_t dlen = 0;
	FILE *in, *out;

	in = fmemopen(s->buf, s->buflen, "r");
	if (unlikely(!in)) {
		print_err("Failed to fmemopen in zpaq_decompress_buf\n");
		return -1;
	}
	out = open_memstream((char **)&c_buf, &dlen);
	if (unlikely(!out)) {
		print_err("Failed to open_memstream in zpaq_decompress_buf\n");
		return -1;
	}

	zpipe_decompress(in, out, control.msgout, s->buflen, (int)(SHOW_PROGRESS));

	if (unlikely(memstream_update_buffer(out, &c_buf, &dlen)))
	        fatal("Failed to memstream_update_buffer in zpaq_decompress_buf");

	fclose(in);
	fclose(out);
	free(s->buf);
	s->buf = c_buf;

	if (unlikely((i64)dlen != s->buflen)) {
		print_err("Inconsistent length after decompression. Got %lld bytes, expected %lld\n", (i64)dlen, s->buflen);
		return -1;
	}

	return 0;
}

static int bzip2_decompress_buf(struct stream *s, i64 c_len)
{
	u32 dlen = s->buflen;
	uchar *c_buf;
	int bzerr;

	c_buf = s->buf;
	s->buf = malloc(dlen);
	if (unlikely(!s->buf)) {
		print_err("Failed to allocate %d bytes for decompression\n", dlen);
		return -1;
	}

	bzerr = BZ2_bzBuffToBuffDecompress((char*)s->buf, &dlen, (char*)c_buf, c_len, 0, 0);
	if (unlikely(bzerr != BZ_OK)) {
		print_err("Failed to decompress buffer - bzerr=%d\n", bzerr);
		return -1;
	}

	if (unlikely(dlen != s->buflen)) {
		print_err("Inconsistent length after decompression. Got %d bytes, expected %lld\n", dlen, s->buflen);
		return -1;
	}

	free(c_buf);
	return 0;
}

static int gzip_decompress_buf(struct stream *s, i64 c_len)
{
	unsigned long dlen = s->buflen;
	uchar *c_buf;
	int gzerr;

	c_buf = s->buf;
	s->buf = malloc(dlen);
	if (unlikely(!s->buf)) {
		print_err("Failed to allocate %ld bytes for decompression\n", dlen);
		return -1;
	}

	gzerr = uncompress(s->buf, &dlen, c_buf, c_len);
	if (unlikely(gzerr != Z_OK)) {
		print_err("Failed to decompress buffer - bzerr=%d\n", gzerr);
		return -1;
	}

	if (unlikely((i64)dlen != s->buflen)) {
		print_err("Inconsistent length after decompression. Got %ld bytes, expected %lld\n", dlen, s->buflen);
		return -1;
	}

	free(c_buf);
	return 0;
}

static int lzma_decompress_buf(struct stream *s, size_t c_len)
{
	size_t dlen = (size_t)s->buflen;
	uchar *c_buf;
	int lzmaerr;

	c_buf = s->buf;
	s->buf = malloc(dlen);
	if (unlikely(!s->buf)) {
		print_err("Failed to allocate %lldd bytes for decompression\n", (i64)dlen);
		return -1;
	}

	/* With LZMA SDK 4.63 we pass control.lzma_properties
	 * which is needed for proper uncompress */
	lzmaerr = LzmaUncompress(s->buf, &dlen, c_buf, &c_len, control.lzma_properties, 5);
	if (unlikely(lzmaerr)) {
		print_err("Failed to decompress buffer - lzmaerr=%d\n", lzmaerr);
		return -1;
	}

	if (unlikely((i64)dlen != s->buflen)) {
		print_err("Inconsistent length after decompression. Got %lld bytes, expected %lld\n", (i64)dlen, s->buflen);
		return -1;
	}

	free(c_buf);
	return 0;
}

static int lzo_decompress_buf(struct stream *s, i64 c_len)
{
	lzo_uint dlen = s->buflen;
	uchar *c_buf;
	int lzerr;

	c_buf = s->buf;
	s->buf = malloc(dlen);
	if (unlikely(!s->buf)) {
		print_err("Failed to allocate %lu bytes for decompression\n", (unsigned long)dlen);
		return -1;
	}

	lzerr = lzo1x_decompress((uchar*)c_buf,c_len,(uchar*)s->buf,&dlen,NULL);
	if (unlikely(lzerr != LZO_E_OK)) {
		print_err("Failed to decompress buffer - lzerr=%d\n", lzerr);
		return -1;
	}

	if (unlikely((i64)dlen != s->buflen)) {
		print_err("Inconsistent length after decompression. Got %lu bytes, expected %lld\n", (unsigned long)dlen, s->buflen);
		return -1;
	}

	free(c_buf);
	return 0;
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
		if (unlikely(ret < 0))
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
		if (unlikely(ret < 0))
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

/* open a set of output streams, compressing with the given
   compression level and algorithm */
void *open_stream_out(int f, int n, i64 limit)
{
	unsigned cwindow = control.window;
	struct stream_info *sinfo;
	uchar *testmalloc;
	int i;

	sinfo = malloc(sizeof(*sinfo));
	if (unlikely(!sinfo))
		return NULL;

	sinfo->bufsize = limit;
	sinfo->thread_no = 0;

	cthread = calloc(sizeof(struct compress_thread), control.threads);
	if (unlikely(!cthread))
		fatal("Unable to calloc cthread in open_stream_out\n");

	for (i = 0; i < control.threads; i++) {
		init_sem(&cthread[i].complete);
		init_sem(&cthread[i].free);
		post_sem(&cthread[i].free);
	}

	/* Threads need to wait on the thread before them before dumping their
	 * data. This is done in a circle up to control.threads */
	cthread[0].wait_on = control.threads - 1;
	for (i = 1; i < control.threads; i++)
		cthread[i].wait_on = i - 1;

	/* Signal thread 0 that it can start */
	if (control.threads > 1)
		post_sem(&cthread[control.threads - 1].complete);

	sinfo->num_streams = n;
	sinfo->cur_pos = 0;
	sinfo->fd = f;

	if (BITS32) {
		/* Largest window supported on 32bit is 600MB */
		if (!cwindow || cwindow > 6)
			cwindow = 6;
	}

	/* No point making the stream larger than the amount of data */
	if (cwindow)
		sinfo->bufsize = MIN(STREAM_BUFSIZE * 10 * cwindow, limit);
	else
		sinfo->bufsize = limit;

	sinfo->initial_pos = lseek(f, 0, SEEK_CUR);

	sinfo->s = (struct stream *)calloc(sizeof(sinfo->s[0]), n);
	if (unlikely(!sinfo->s)) {
		free(sinfo);
		return NULL;
	}

	/* Find the largest we can make the window based on ability to malloc
	 * ram. We need enough for the 2 streams and for the compression
	 * backend at most, being conservative. */
retest_malloc:
	testmalloc = malloc(sinfo->bufsize * (n + 1));
	if (!testmalloc) {
		sinfo->bufsize = sinfo->bufsize / 10 * 9;
		goto retest_malloc;
	}
	free(testmalloc);

	/* Make the bufsize no smaller than STREAM_BUFSIZE. Round up the
	 * bufsize to fit X threads into it */
	sinfo->bufsize = MIN(sinfo->bufsize,
			     MAX((sinfo->bufsize + control.threads - 1) / control.threads, STREAM_BUFSIZE));

	if (control.threads > 1)
		print_maxverbose("Using %d threads to compress up to %lld bytes each.\n",
			control.threads, sinfo->bufsize);
	else
		print_maxverbose("Using 1 thread to compress up to %lld bytes\n",
			sinfo->bufsize);

	for (i = 0; i < n; i++) {
		sinfo->s[i].buf = malloc(sinfo->bufsize);
		if (unlikely(!sinfo->s[i].buf))
			fatal("Unable to malloc buffer of size %lld in open_stream_out\n", sinfo->bufsize);
	}

	/* write the initial headers */
	for (i = 0; i < n; i++) {
		sinfo->s[i].last_head = sinfo->cur_pos + 17;
		write_u8(sinfo->fd, CTYPE_NONE);
		write_i64(sinfo->fd, 0);
		write_i64(sinfo->fd, 0);
		write_i64(sinfo->fd, 0);
		sinfo->cur_pos += 25;
	}
	return (void *)sinfo;
}

/* prepare a set of n streams for reading on file descriptor f */
void *open_stream_in(int f, int n)
{
	struct stream_info *sinfo;
	i64 header_length;
	int i;

	sinfo = calloc(sizeof(*sinfo), 1);
	if (unlikely(!sinfo))
		return NULL;

	sinfo->num_streams = n;
	sinfo->fd = f;
	sinfo->initial_pos = lseek(f, 0, SEEK_CUR);

	sinfo->s = (struct stream *)calloc(sizeof(sinfo->s[0]), n);
	if (unlikely(!sinfo->s)) {
		free(sinfo);
		return NULL;
	}

	for (i = 0; i < n; i++) {
		uchar c;
		i64 v1, v2;

again:
		if (unlikely(read_u8(f, &c)))
			goto failed;

		/* Compatibility crap for versions < 0.40 */
		if (control.major_version == 0 && control.minor_version < 4) {
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
void *compthread(void *t)
{
	long i = (long)t;
	struct compress_thread *cti = &cthread[i];

	cti->c_type = CTYPE_NONE;
	cti->c_len = cti->s_len;

	if (!NO_COMPRESS && cti->c_len) {
		if (LZMA_COMPRESS)
			lzma_compress_buf(cti);
		else if (LZO_COMPRESS)
			lzo_compress_buf(cti);
		else if (BZIP2_COMPRESS)
			bzip2_compress_buf(cti);
		else if (ZLIB_COMPRESS)
			gzip_compress_buf(cti);
		else if (ZPAQ_COMPRESS)
			zpaq_compress_buf(cti);
		else fatal("Dunno wtf compression to use!\n");
	}

	if (control.threads > 1)
		wait_sem(&cthread[cti->wait_on].complete);

	if (unlikely(seekto(cti->sinfo, cti->sinfo->s[cti->stream].last_head)))
		fatal("Failed to seekto in compthread %d\n", i);

	if (unlikely(write_i64(cti->sinfo->fd, cti->sinfo->cur_pos)))
		fatal("Failed to write_i64 in compthread %d\n", i);

	cti->sinfo->s[cti->stream].last_head = cti->sinfo->cur_pos + 17;
	if (unlikely(seekto(cti->sinfo, cti->sinfo->cur_pos)))
		fatal("Failed to seekto cur_pos in compthread %d\n", i);

	print_maxverbose("Writing %lld compressed bytes from thread %ld\n", cti->c_len, i);
	if (unlikely(write_u8(cti->sinfo->fd, cti->c_type) ||
		write_i64(cti->sinfo->fd, cti->c_len) ||
		write_i64(cti->sinfo->fd, cti->s_len) ||
		write_i64(cti->sinfo->fd, 0))) {
			fatal("Failed write in compthread %d\n", i);
	}
	cti->sinfo->cur_pos += 25;

	if (unlikely(write_buf(cti->sinfo->fd, cti->s_buf, cti->c_len)))
		fatal("Failed to write_buf in compthread %d\n", i);

	cti->sinfo->cur_pos += cti->c_len;
	free(cti->s_buf);

	fsync(cti->sinfo->fd);

	post_sem(&cti->complete);
	post_sem(&cti->free);

	return 0;
}

/* flush out any data in a stream buffer */
void flush_buffer(struct stream_info *sinfo, int stream)
{
	pthread_t threads[control.threads];
	long i = sinfo->thread_no;

	/* Make sure this thread doesn't already exist */
	wait_sem(&cthread[i].free);

	cthread[i].sinfo = sinfo;
	cthread[i].stream = stream;
	cthread[i].s_buf = sinfo->s[stream].buf;
	cthread[i].s_len = sinfo->s[stream].buflen;

	print_maxverbose("Starting thread %ld to compress %lld bytes from stream %d\n",
		i, cthread[i].s_len, stream);
	create_pthread(&threads[i], NULL, compthread, (void *)i);

	/* The stream buffer has been given to the thread, allocate a new one */
	sinfo->s[stream].buf = malloc(sinfo->bufsize);
	if (unlikely(!sinfo->s[stream].buf))
		fatal("Unable to malloc buffer of size %lld in flush_buffer\n", sinfo->bufsize);
	sinfo->s[stream].buflen = 0;

	if (++sinfo->thread_no == control.threads)
		sinfo->thread_no = 0;
}

/* fill a buffer from a stream - return -1 on failure */
static int fill_buffer(struct stream_info *sinfo, int stream)
{
	i64 header_length, u_len, c_len;
	uchar c_type;

	if (unlikely(seekto(sinfo, sinfo->s[stream].last_head)))
		return -1;

	if (unlikely(read_u8(sinfo->fd, &c_type)))
		return -1;
	/* Compatibility crap for versions < 0.4 */
	if (control.major_version == 0 && control.minor_version < 4) {
		u32 c_len32, u_len32, last_head32;

		if (unlikely(read_u32(sinfo->fd, &c_len32)))
			return -1;
		if (unlikely(read_u32(sinfo->fd, &u_len32)))
			return -1;
		if (unlikely(read_u32(sinfo->fd, &last_head32)))
			return -1;
		c_len = c_len32;
		u_len = u_len32;
		sinfo->s[stream].last_head = last_head32;
		header_length = 13;
	} else {
		if (unlikely(read_i64(sinfo->fd, &c_len)))
			return -1;
		if (unlikely(read_i64(sinfo->fd, &u_len)))
			return -1;
		if (unlikely(read_i64(sinfo->fd, &sinfo->s[stream].last_head)))
			return -1;
		header_length = 25;
	}

	sinfo->total_read += header_length;

	if (sinfo->s[stream].buf)
		sinfo->s[stream].buf = realloc(sinfo->s[stream].buf, u_len);
	else
		sinfo->s[stream].buf = malloc(u_len);
	if (unlikely(u_len && !sinfo->s[stream].buf))
		fatal("Unable to malloc buffer of size %lld in fill_buffer\n", u_len);

	if (unlikely(read_buf(sinfo->fd, sinfo->s[stream].buf, c_len)))
		return -1;

	sinfo->total_read += c_len;

	sinfo->s[stream].buflen = u_len;
	sinfo->s[stream].bufp = 0;

	if (c_type != CTYPE_NONE) {
		if (c_type == CTYPE_LZMA) {
			if (unlikely(lzma_decompress_buf(&sinfo->s[stream], (size_t)c_len)))
				return -1;
		} else if (c_type == CTYPE_LZO) {
			if (unlikely(lzo_decompress_buf(&sinfo->s[stream], c_len)))
				return -1;
		} else if (c_type == CTYPE_BZIP2) {
			if (unlikely(bzip2_decompress_buf(&sinfo->s[stream], c_len)))
				return -1;
		} else if (c_type == CTYPE_GZIP) {
			if (unlikely(gzip_decompress_buf(&sinfo->s[stream], c_len)))
				return -1;
		} else if (c_type == CTYPE_ZPAQ) {
			if (unlikely(zpaq_decompress_buf(&sinfo->s[stream])))
				return -1;
		} else fatal("Dunno wtf decompression type to use!\n");
	}

	return 0;
}

/* write some data to a stream. Return -1 on failure */
int write_stream(void *ss, int stream, uchar *p, i64 len)
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
			flush_buffer(sinfo, stream);
	}
	return 0;
}

/* read some data from a stream. Return number of bytes read, or -1
   on failure */
i64 read_stream(void *ss, int stream, uchar *p, i64 len)
{
	struct stream_info *sinfo = ss;
	i64 ret = 0;

	while (len) {
		i64 n;

		n = MIN(sinfo->s[stream].buflen-sinfo->s[stream].bufp, len);

		if (n > 0) {
			memcpy(p, sinfo->s[stream].buf + sinfo->s[stream].bufp, n);
			sinfo->s[stream].bufp += n;
			p += n;
			len -= n;
			ret += n;
		}

		if (len && sinfo->s[stream].bufp == sinfo->s[stream].buflen) {
			if (unlikely(fill_buffer(sinfo, stream)))
				return -1;
			if (sinfo->s[stream].bufp == sinfo->s[stream].buflen)
				break;
		}
	}

	return ret;
}

/* flush and close down a stream. return -1 on failure */
int close_stream_out(void *ss)
{
	struct stream_info *sinfo = ss;
	int i;

	for (i = 0; i < sinfo->num_streams; i++) {
		if (sinfo->s[i].buflen)
			flush_buffer(sinfo, i);
	}

	for (i = 0; i < control.threads; i++) {
		wait_sem(&cthread[i].free);
		destroy_sem(&cthread[i].complete);
		destroy_sem(&cthread[i].free);
	}

	for (i = 0; i < sinfo->num_streams; i++)
		free(sinfo->s[i].buf);

	free(cthread);
	free(sinfo->s);
	free(sinfo);

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

	free(sinfo->s);
	free(sinfo);
	return 0;
}

/* As others are slow and lzo very fast, it is worth doing a quick lzo pass
   to see if there is any compression at all with lzo first. It is unlikely
   that others will be able to compress if lzo is unable to drop a single byte
   so do not compress any block that is incompressible by lzo. */
static int lzo_compresses(uchar *s_buf, i64 s_len)
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

	if (control.threshold > 1)
		return 1;
	wrkmem = (lzo_bytep) malloc(LZO1X_1_MEM_COMPRESS);
	if (unlikely(wrkmem == NULL))
		fatal("Unable to allocate wrkmem in lzo_compresses\n");

	in_len = MIN(test_len, buftest_size);
	dlen = STREAM_BUFSIZE + STREAM_BUFSIZE / 16 + 64 + 3;

	c_buf = malloc(dlen);
	if (unlikely(!c_buf))
		fatal("Unable to allocate c_buf in lzo_compresses\n");

	print_progress("lzo testing for incompressible data...\n");

	/* Test progressively larger blocks at a time and as soon as anything
	   compressible is found, jump out as a success */
	while (test_len > 0) {
		workcounter++;
		return_var = lzo1x_1_compress(test_buf, in_len, (uchar *)c_buf, &dlen, wrkmem);

		if (dlen < best_dlen)
			best_dlen = dlen;	/* save best value */

		if ((double) dlen < (double)in_len * control.threshold) {
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
	if (MAX_VERBOSE)
		print_output("%s for chunk %ld. Compressed size = %5.2F%% of chunk, %d Passes\n",
			(ret == 0? "FAILED - below threshold" : "OK"), save_len,
			100 * ((double) best_dlen / (double) in_len), workcounter);
	else if (VERBOSE)
		print_output("%s\n", (ret == 0? "FAILED - below threshold" : "OK"));

	free(wrkmem);
	free(c_buf);

	return ret;
}
