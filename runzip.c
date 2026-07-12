/*
   Copyright (C) 2006-2016,2018,2021-2022,2026 Con Kolivas
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
/* rzip decompression algorithm */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <sys/types.h>
#ifdef HAVE_SYS_STAT_H
# include <sys/stat.h>
#endif
#ifdef HAVE_SYS_TIME_H
# include <sys/time.h>
#endif
#ifdef HAVE_UNISTD_H
# include <unistd.h>
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
#include <string.h>

#include "md5.h"
#include "runzip.h"
#include "stream.h"
#include "util.h"
#include "filters.h"
#include "lrzip_core.h"
/* needed for CRC routines */
#include "lzma/C/7zCrc.h"

/* Batched stream-0 window: token headers are tiny (1–3B + offset) and
 * millions of them thrash read_stream; fill a few KiB at a time instead. */
#define RUNZIP_S0_WIN 4096

struct runzip_s0 {
	uchar buf[RUNZIP_S0_WIN];
	unsigned pos;
	unsigned end;
};

/* Ensure at least need bytes are buffered from stream 0. */
static int s0_need(rzip_control *control, void *ss, struct runzip_s0 *s0,
		   unsigned need)
{
	unsigned have = s0->end - s0->pos;
	i64 got, space;

	if (likely(have >= need))
		return 0;
	if (s0->pos) {
		if (have)
			memmove(s0->buf, s0->buf + s0->pos, have);
		s0->pos = 0;
		s0->end = have;
	}
	while (s0->end - s0->pos < need) {
		space = RUNZIP_S0_WIN - s0->end;
		if (unlikely(space <= 0))
			return -1;
		got = read_stream(control, ss, 0, s0->buf + s0->end, space);
		if (unlikely(got < 0))
			return -1;
		if (got == 0)
			break;
		s0->end += (unsigned)got;
		/* Short read: stream 0 is drained for now. */
		if (got < space)
			break;
	}
	return (s0->end - s0->pos >= need) ? 0 : -1;
}

static inline i64 s0_vchars(rzip_control *control, void *ss,
			    struct runzip_s0 *s0, int length)
{
	i64 s = 0;

	if (unlikely(s0_need(control, ss, s0, (unsigned)length)))
		fatal_return(("Stream read of %d bytes failed\n", length), -1);
	memcpy(&s, s0->buf + s0->pos, (size_t)length);
	s0->pos += (unsigned)length;
	return le64toh(s);
}

static inline u32 s0_u32(rzip_control *control, void *ss, struct runzip_s0 *s0,
			 bool *err)
{
	u32 ret;

	if (unlikely(s0_need(control, ss, s0, 4))) {
		*err = true;
		fatal_return(("Stream read u32 failed\n"), 0);
	}
	memcpy(&ret, s0->buf + s0->pos, 4);
	s0->pos += 4;
	return le32toh(ret);
}

static i64 seekcur_fdout(rzip_control *control)
{
	if (!TMP_OUTBUF)
		return lseek(control->fd_out, 0, SEEK_CUR);
	return (control->out_relofs + control->out_ofs);
}

static i64 seekto_fdhist(rzip_control *control, i64 pos)
{
	if (!TMP_OUTBUF)
		return lseek(control->fd_hist, pos, SEEK_SET);
	control->hist_ofs = pos - control->out_relofs;
	if (control->hist_ofs > control->out_len)
		control->out_len = control->hist_ofs;
	if (unlikely(control->hist_ofs < 0 || control->hist_ofs > control->out_maxlen)) {
		print_err("Trying to seek outside tmpoutbuf to %"PRId64" in seekto_fdhist\n", control->hist_ofs);
		return -1;
	}
	return pos;
}

static i64 seekcur_fdin(rzip_control *control)
{
	if (!TMP_INBUF)
		return lseek(control->fd_in, 0, SEEK_CUR);
	return control->in_ofs;
}

static i64 seekto_fdin(rzip_control *control, i64 pos)
{
	if (!TMP_INBUF)
		return lseek(control->fd_in, pos, SEEK_SET);
	if (unlikely(pos > control->in_len || pos < 0)) {
		print_err("Trying to seek outside tmpinbuf to %"PRId64" in seekto_fdin\n", pos);
		return -1;
	}
	control->in_ofs = pos;
	return 0;
}

static i64 seekto_fdinend(rzip_control *control)
{
	int tmpchar;

	if (!TMP_INBUF)
		return lseek(control->fd_in, 0, SEEK_END);
	while ((tmpchar = getchar()) != EOF) {
		control->tmp_inbuf[control->in_len++] = (char)tmpchar;
		if (unlikely(control->in_len > control->in_maxlen))
			failure_return(("Trying to read greater than max_len\n"), -1);
	}
	control->in_ofs = control->in_len;
	return control->in_ofs;
}

/* head (1) + length (control->chunk_bytes, usually 2) in one window fill. */
static i64 read_header(rzip_control *control, void *ss, struct runzip_s0 *s0,
		       uchar *head)
{
	int lb = control->chunk_bytes;
	i64 s = 0;

	if (unlikely(s0_need(control, ss, s0, (unsigned)(1 + lb))))
		return -1;
	*head = s0->buf[s0->pos++];
	memcpy(&s, s0->buf + s0->pos, (size_t)lb);
	s0->pos += (unsigned)lb;
	return le64toh(s);
}

/* Grow-only scratch for literal/match tokens — avoids malloc/free per token.
 * Token lengths are format-capped at LRZIP_MAX_TOKEN_LEN (0xFFFF). */
static uchar *runzip_get_buf(rzip_control *control, i64 len)
{
	uchar *nbuf;

	if (unlikely(!lrzip_size_ok(len, LRZIP_MAX_TOKEN_LEN)))
		return NULL;
	if (likely(len <= control->runzip_buf_len))
		return control->runzip_buf;
	nbuf = realloc(control->runzip_buf, (size_t)len);
	if (unlikely(!nbuf))
		return NULL;
	control->runzip_buf = nbuf;
	control->runzip_buf_len = len;
	return nbuf;
}

/* ---- Batched MD5 worker (same semaphore protocol as compress) ---- */
#define RUNZIP_MD5_CHUNK (1024 * 1024)

static void *runzip_md5_worker(void *data)
{
	rzip_control *control = (rzip_control *)data;

	while (42) {
		cksem_wait(control, &control->cksum_worksem);
		if (control->checksum.shutdown) {
			cksem_post(control, &control->cksumsem);
			break;
		}
		if (control->checksum.len > 0)
			md5_process_bytes(control->checksum.buf, control->checksum.len,
					  &control->ctx);
		cksem_post(control, &control->cksumsem);
	}
	return NULL;
}

static void runzip_md5_start(rzip_control *control)
{
	i64 cap = RUNZIP_MD5_CHUNK;

	round_to_page(&cap);
	control->checksum.capacity = cap;
	control->checksum.buf = malloc((size_t)cap);
	if (unlikely(!control->checksum.buf))
		failure("Failed to allocate MD5 batch buffer\n");
	control->checksum.len = 0;
	control->checksum.shutdown = 0;
	control->checksum.filling = 0;

	cksem_init(control, &control->cksumsem);
	cksem_post(control, &control->cksumsem);
	cksem_init(control, &control->cksum_worksem);

	if (unlikely(!create_pthread(control, &control->md5_thread, NULL,
				     runzip_md5_worker, control)))
		failure("Failed to start MD5 worker thread\n");
}

/* Flush any partial batch, wait for the worker to go idle, then join. */
static void runzip_md5_stop(rzip_control *control)
{
	if (control->checksum.filling) {
		if (control->checksum.len > 0)
			cksem_post(control, &control->cksum_worksem);
		else
			cksem_post(control, &control->cksumsem);
		control->checksum.filling = 0;
	}

	cksem_wait(control, &control->cksumsem);
	control->checksum.shutdown = 1;
	control->checksum.len = 0;
	cksem_post(control, &control->cksum_worksem);
	if (unlikely(!join_pthread(control, control->md5_thread, NULL)))
		failure("Failed to join MD5 worker thread\n");
	dealloc(control->checksum.buf);
	control->checksum.capacity = 0;
	control->checksum.buf = NULL;
}

/* Append to the batch buffer; submit full buffers to the worker. */
static void runzip_md5_update(rzip_control *control, const uchar *data, i64 n)
{
	while (n > 0) {
		i64 space, c;

		if (!control->checksum.filling) {
			cksem_wait(control, &control->cksumsem);
			control->checksum.len = 0;
			control->checksum.filling = 1;
		}
		space = control->checksum.capacity - control->checksum.len;
		c = MIN(n, space);
		memcpy(control->checksum.buf + control->checksum.len, data, (size_t)c);
		control->checksum.len += c;
		data += c;
		n -= c;
		if (control->checksum.len == control->checksum.capacity) {
			cksem_post(control, &control->cksum_worksem);
			control->checksum.filling = 0;
		}
	}
}

static inline void match_cksum(rzip_control *control, uint32 *cksum,
			       const uchar *buf, i64 n)
{
	/* Chunk filtered archives are reconstructed in the filtered domain;
	 * checksums of the original bytes are computed during the unfilter
	 * pass at the end of the chunk instead. */
	if (control->chunk_filter != LRZ_FILTER_NONE)
		return;
	if (!HAS_MD5)
		*cksum = CrcUpdate(*cksum, buf, n);
	if (!NO_MD5)
		runzip_md5_update(control, buf, n);
}

static i64 unzip_literal(rzip_control *control, void *ss, i64 len,
			 uint32 *cksum, i64 *out_pos)
{
	i64 stream_read;
	uchar *buf;

	if (unlikely(len < 0))
		failure_return(("len %"PRId64" is negative in unzip_literal!\n",len), -1);

	if (!len)
		return 0;

	if (unlikely(len > LRZIP_MAX_TOKEN_LEN))
		failure_return(("Literal length %"PRId64" exceeds format max\n", len), -1);

	buf = runzip_get_buf(control, len);
	if (unlikely(!buf))
		fatal_return(("Failed to malloc literal buffer of size %"PRId64"\n", len), -1);

	stream_read = read_stream(control, ss, 1, buf, len);
	if (unlikely(stream_read == -1 ))
		fatal_return(("Failed to read_stream in unzip_literal\n"), -1);
	if (unlikely(stream_read != len))
		failure_return(("Short literal read %"PRId64" of %"PRId64" (corrupt archive)\n",
			       stream_read, len), -1);

	if (unlikely(write_all(control, buf, stream_read) != stream_read))
		fatal_return(("Failed to write literal buffer of size %"PRId64"\n", stream_read), -1);

	match_cksum(control, cksum, buf, stream_read);

	*out_pos += stream_read;
	return stream_read;
}

static i64 read_fdhist(rzip_control *control, void *buf, i64 len)
{
	if (!TMP_OUTBUF)
		return read_all(control, control->fd_hist, buf, len);
	if (unlikely(len + control->hist_ofs > control->out_maxlen)) {
		print_err("Trying to read beyond end of tmpoutbuf in read_fdhist\n");
		return -1;
	}
	memcpy(buf, control->tmp_outbuf + control->hist_ofs, len);
	return len;
}

/* Expand RLE match of period `offset` from the first `period` bytes of buf
 * out to `len` bytes (period == min(len, offset)). */
static void match_expand(uchar *buf, i64 period, i64 offset, i64 len)
{
	i64 pos = period;

	while (pos < len) {
		i64 n = MIN(len - pos, offset);

		memcpy(buf + pos, buf + pos - offset, (size_t)n);
		pos += n;
	}
}

static i64 unzip_match(rzip_control *control, void *ss, struct runzip_s0 *s0,
		       i64 len, uint32 *cksum, int chunk_bytes, i64 *out_pos)
{
	i64 offset, period, cur_pos, hist;
	uchar *buf;

	if (unlikely(len < 0))
		failure_return(("len %"PRId64" is negative in unzip_match!\n",len), -1);

	if (unlikely(len > LRZIP_MAX_TOKEN_LEN))
		failure_return(("Match length %"PRId64" exceeds format max\n", len), -1);

	/* Tracked write position — avoids lseek(SEEK_CUR) every match. */
	cur_pos = *out_pos;

	/* Note the offset is in a different format v0.40+ */
	offset = s0_vchars(control, ss, s0, chunk_bytes);
	if (unlikely(offset == -1))
		return -1;

	if (unlikely(offset < 1 || offset > cur_pos))
		failure_return(("Match offset %"PRId64" out of range at pos %"PRId64"\n",
			       offset, cur_pos), -1);

	period = MIN(len, offset);
	if (unlikely(period < 1))
		fatal_return(("Failed fd history in unzip_match due to corrupt archive\n"), -1);

	/*
	 * In-RAM history (stdout / tmp buffer): expand the match directly in
	 * tmp_outbuf — no intermediate scratch, one integrity pass.
	 * Source for the first period is hist; further periods copy from the
	 * just-written output (standard RLE expand, non-overlapping chunks).
	 */
	if (TMP_OUTBUF) {
		i64 dest = control->out_ofs;

		hist = cur_pos - offset - control->out_relofs;
		if (unlikely(hist < 0 || dest != cur_pos - control->out_relofs))
			fatal_return(("Bad hist/dest in unzip_match: hist %"PRId64" dest %"PRId64"\n",
				     hist, dest), -1);

		if (dest + len <= control->out_maxlen) {
			uchar *out = control->tmp_outbuf;

			memcpy(out + dest, out + hist, (size_t)period);
			match_expand(out + dest, period, offset, len);
			match_cksum(control, cksum, out + dest, len);
			control->out_ofs = dest + len;
			if (control->out_ofs > control->out_len)
				control->out_len = control->out_ofs;
			*out_pos += len;
			return len;
		}
		/* Falls through when the match will not fit in tmp_outbuf. */
	}

	if (unlikely(seekto_fdhist(control, cur_pos - offset) == -1))
		fatal_return(("Seek failed by %"PRId64" from %"PRId64" on history file in unzip_match\n",
		      offset, cur_pos), -1);

	/* File path (or tmp overflow): pull one period, expand full match in
	 * scratch (len ≤ 0xFFFF), one write + one integrity pass. */
	buf = runzip_get_buf(control, len);
	if (unlikely(!buf))
		fatal_return(("Failed to malloc match buffer of size %"PRId64"\n", len), -1);

	if (unlikely(read_fdhist(control, buf, period) != period))
		fatal_return(("Failed to read %"PRId64" bytes in unzip_match\n", period), -1);

	match_expand(buf, period, offset, len);

	if (unlikely(write_all(control, buf, len) != (ssize_t)len))
		fatal_return(("Failed to write %"PRId64" bytes in unzip_match\n", len), -1);

	match_cksum(control, cksum, buf, len);
	*out_pos += len;
	return len;
}

/* Reverse a chunk prefilter over the reconstructed output region
 * [start, start + len) and feed the checksums with the restored original
 * bytes. Reconstruction happens in the filtered domain (matches reference
 * filtered history), so this must run after the whole chunk is written. */
static bool unfilter_chunk(rzip_control *control, i64 start, i64 len, uint32 *cksum)
{
	struct lrz_filter_stream fs;
	const i64 slice = 16 * 1024 * 1024;
	i64 processed = 0, carry = 0;
	uchar *buf;

	if (!len)
		return true;

	lrz_filter_stream_init(&fs, control->chunk_filter, false);

	if (TMP_OUTBUF) {
		uchar *p = control->tmp_outbuf + (start - control->out_relofs);

		if (unlikely(start - control->out_relofs < 0 ||
			     start - control->out_relofs + len > control->out_len)) {
			print_err("Chunk filter region outside tmp outbuf\n");
			return false;
		}
		lrz_filter_stream_conv(&fs, p, len, true);
		if (!HAS_MD5)
			*cksum = CrcUpdate(*cksum, p, len);
		if (!NO_MD5)
			runzip_md5_update(control, p, len);
		return true;
	}

	buf = malloc(MIN(slice, len));
	if (unlikely(!buf))
		fatal_return(("Failed to allocate unfilter buffer\n"), false);

	while (processed + carry < len) {
		i64 n = MIN(slice - carry, len - processed - carry);
		i64 total = carry + n, done;
		bool last;

		/* fd_out may be write only; fd_hist is a readable descriptor
		 * on the same output file */
		if (unlikely(pread(control->fd_hist, buf + carry, (size_t)n,
				   start + processed + carry) != (ssize_t)n)) {
			dealloc(buf);
			fatal_return(("Failed to pread in unfilter_chunk\n"), false);
		}
		last = (processed + total == len);
		done = lrz_filter_stream_conv(&fs, buf, total, last);
		if (unlikely(pwrite(control->fd_out, buf, (size_t)done,
				    start + processed) != (ssize_t)done)) {
			dealloc(buf);
			fatal_return(("Failed to pwrite in unfilter_chunk\n"), false);
		}
		if (!HAS_MD5)
			*cksum = CrcUpdate(*cksum, buf, done);
		if (!NO_MD5)
			runzip_md5_update(control, buf, done);
		carry = total - done;
		if (carry)
			memmove(buf, buf + done, (size_t)carry);
		processed += done;
	}
	dealloc(buf);
	return true;
}

/* decompress a section of an open file. Call fatal_return(() on error
   return the number of bytes that have been retrieved
 */
static i64 runzip_chunk(rzip_control *control, int fd_in, i64 expected_size, i64 tally)
{
	uint32 good_cksum, cksum = 0;
	i64 len, ofs, total = 0, out_pos, progress_at = 0;
	int l = -1, p = 0;
	char chunk_bytes;
	struct runzip_s0 s0;
	struct stat st;
	uchar head;
	void *ss;
	bool err = false;
	/* Progress at most every 64KiB (same cadence as compress). */
	const i64 progress_bytes = 64 * 1024;

	/* for display of progress */
	unsigned long divisor[] = {1,1024,1048576,1073741824U};
	char *suffix[] = {"","KB","MB","GB"};
	double prog_done, prog_tsize;
	int divisor_index;

	if (expected_size > (i64)10737418240ULL)	/* > 10GB */
		divisor_index = 3;
	else if (expected_size > 10485760)	/* > 10MB */
		divisor_index = 2;
	else if (expected_size > 10240)	/* > 10KB */
		divisor_index = 1;
	else
		divisor_index = 0;

	prog_tsize = (long double)expected_size / (long double)divisor[divisor_index];

	/* Determine the chunk_byte width size. Versions < 0.4 used 4
	 * bytes for all offsets, version 0.4 used 8 bytes. Versions 0.5+ use
	 * a variable number of bytes depending on chunk size.*/
	if (control->major_version == 0 && control->minor_version < 4)
		chunk_bytes = 4;
	else if (control->major_version == 0 && control->minor_version == 4)
		chunk_bytes = 8;
	else {
		print_maxverbose("Reading chunk_bytes at %"PRId64"\n", get_readseek(control, fd_in));
		/* Read in the stored chunk byte width from the file */
		if (unlikely(read_all(control, fd_in, &chunk_bytes, 1) != 1))
			fatal_return(("Failed to read chunk_bytes size in runzip_chunk\n"), -1);
		if (unlikely(chunk_bytes < 1 || chunk_bytes > 8))
			failure_return(("chunk_bytes %d is invalid in runzip_chunk\n", chunk_bytes), -1);
	}
	/* 0.7 chunk headers carry a prefilter byte after chunk_bytes
	 * (rolled into the 0.7 format and cemented by the 0.7.1 release) */
	control->chunk_filter = LRZ_FILTER_NONE;
	if (control->major_version > 0 || control->minor_version > 6) {
		char chunk_filter;

		if (unlikely(read_all(control, fd_in, &chunk_filter, 1) != 1))
			fatal_return(("Failed to read chunk_filter in runzip_chunk\n"), -1);
		if (unlikely(chunk_filter < 0 || chunk_filter > LRZ_CHUNK_FILTER_MAX))
			failure_return(("chunk_filter %d is invalid in runzip_chunk\n", chunk_filter), -1);
		control->chunk_filter = chunk_filter;
		if (chunk_filter)
			print_maxverbose("Chunk prefiltered with filter %d\n", chunk_filter);
	}
	if (!tally && expected_size)
		print_maxverbose("Expected size: %"PRId64"\n", expected_size);
	print_maxverbose("Chunk byte width: %d\n", chunk_bytes);

	ofs = seekcur_fdin(control);
	if (unlikely(ofs == -1))
		fatal_return(("Failed to seek input file in runzip_fd\n"), -1);

	if (fstat(fd_in, &st) || st.st_size - ofs == 0)
		return 0;

	ss = open_stream_in(control, fd_in, NUM_STREAMS, chunk_bytes);
	if (unlikely(!ss))
		failure_return(("Failed to open_stream_in in runzip_chunk\n"), -1);

	/* All chunks were unnecessarily encoded 8 bytes wide version 0.4x */
	if (control->major_version == 0 && control->minor_version == 4)
		control->chunk_bytes = 8;
	else
		control->chunk_bytes = 2;

	/* One SEEK_CUR per chunk; matches/literals advance out_pos. */
	out_pos = seekcur_fdout(control);
	if (unlikely(out_pos == -1)) {
		close_stream_in(control, ss);
		fatal_return(("Seek failed on out file in runzip_chunk\n"), -1);
	}

	memset(&s0, 0, sizeof(s0));
	if (expected_size)
		progress_at = tally + progress_bytes;

	while ((len = read_header(control, ss, &s0, &head)) || head) {
		i64 u;
		if (unlikely(len == -1))
			return -1;
		switch (head) {
			case 0:
				u = unzip_literal(control, ss, len, &cksum, &out_pos);
				if (unlikely(u == -1)) {
					close_stream_in(control, ss);
					return -1;
				}
				total += u;
				break;

			default:
				u = unzip_match(control, ss, &s0, len, &cksum, chunk_bytes,
						&out_pos);
				if (unlikely(u == -1)) {
					close_stream_in(control, ss);
					return -1;
				}
				total += u;
				break;
		}
		/* Avoid double divide every token — check every 64KiB only. */
		if (expected_size && tally + total >= progress_at) {
			p = (int)((100 * (tally + total)) / expected_size);
			if (p > 100)
				p = 100;
			if (p / 10 != l / 10) {
				prog_done = (double)(tally + total) /
					    (double)divisor[divisor_index];
				print_progress("%3d%%  %9.2f / %9.2f %s\r",
						p, prog_done, prog_tsize,
						suffix[divisor_index]);
				l = p;
			}
			progress_at = tally + total + progress_bytes;
		}
	}

	/* Final progress for the chunk if we ended mid-interval. */
	if (expected_size && total > 0) {
		p = (int)((100 * (tally + total)) / expected_size);
		if (p > 100)
			p = 100;
		if (p / 10 != l / 10) {
			prog_done = (double)(tally + total) /
				    (double)divisor[divisor_index];
			print_progress("%3d%%  %9.2f / %9.2f %s\r",
					p, prog_done, prog_tsize,
					suffix[divisor_index]);
		}
	}

	/* Reverse any chunk prefilter now the whole chunk is reconstructed;
	 * this also computes the checksums of the original bytes. */
	if (control->chunk_filter != LRZ_FILTER_NONE) {
		if (unlikely(!unfilter_chunk(control, out_pos - total, total, &cksum))) {
			close_stream_in(control, ss);
			return -1;
		}
	}

	if (!HAS_MD5) {
		good_cksum = s0_u32(control, ss, &s0, &err);
		if (unlikely(err)) {
			close_stream_in(control, ss);
			return -1;
		}
		if (unlikely(good_cksum != cksum)) {
			close_stream_in(control, ss);
			failure_return(("Bad checksum: 0x%08x - expected: 0x%08x\n", cksum, good_cksum), -1);
		}
		print_maxverbose("Checksum for block: 0x%08x\n", cksum);
	}

	if (unlikely(close_stream_in(control, ss)))
		fatal("Failed to close stream!\n");

	return total;
}

/* Decompress an open file. Call fatal_return(() on error
   return the number of bytes that have been retrieved
 */
i64 runzip_fd(rzip_control *control, int fd_in, int fd_hist, i64 expected_size)
{
	uchar md5_stored[MD5_DIGEST_SIZE];
	struct timeval start,end;
	i64 total = 0, u;
	double tdiff;
	int md5_live = 0;

	if (!NO_MD5) {
		md5_init_ctx (&control->ctx);
		runzip_md5_start(control);
		md5_live = 1;
	}
	gettimeofday(&start,NULL);

	control->next_block_c_size = 0;
	control->block_c_size = 0;
	control->rcd_start = -1;

	do {
		/* Apply framed size from prior LRZC (0 for first / unframed). */
		control->block_c_size = control->next_block_c_size;
		control->next_block_c_size = 0;
		control->rcd_start = seekcur_fdin(control);

		u = runzip_chunk(control, fd_in, expected_size, total);
		if (u < 1) {
			if (u < 0 || total < expected_size) {
				print_err("Failed to runzip_chunk in runzip_fd\n");
				if (md5_live)
					runzip_md5_stop(control);
				return -1;
			}
		}
		total += u;
		control->blocks_done++;

		/* v0.7 streaming mode B: after a non-final block, consume the
		 * LRZC continuation header before the next RCD. RCD eof and
		 * LRZC last-bit must agree. */
		if (STREAMING_BLOCKS && !control->eof) {
			i64 c_size = 0, u_size = 0;

			if (unlikely(!read_lrzc_header(control, fd_in, &c_size, &u_size))) {
				print_err("Failed to read LRZC in runzip_fd\n");
				if (md5_live)
					runzip_md5_stop(control);
				return -1;
			}
			/* c_size frames the next RCD + stream payload. */
			control->next_block_c_size = c_size;
			/* Next RCD eof must match this header's last flag when
			 * that chunk is read; if LRZC says last already, the
			 * following chunk must set eof. */
			if (control->last_block)
				print_maxverbose("LRZC marks final block; next RCD must be last\n");
		}

		if (unlikely(!flush_tmpout(control))) {
			print_err("Failed to flush_tmpout in runzip_fd\n");
			if (md5_live)
				runzip_md5_stop(control);
			return -1;
		}

		if (TMP_INBUF)
			clear_tmpinbuf(control);
		else if (STDIN && !DECOMPRESS) {
			if (unlikely(!clear_tmpinfile(control))) {
				print_err("Failed to clear_tmpinfile in runzip_fd\n");
				if (md5_live)
					runzip_md5_stop(control);
				return -1;
			}
		}
	} while (total < expected_size || (!expected_size && !control->eof));

	/* Streaming multi-block with last never seen is truncated. */
	if (STREAMING_BLOCKS && !control->eof && !control->last_block) {
		print_err("Truncated streaming archive: no final block\n");
		if (md5_live)
			runzip_md5_stop(control);
		return -1;
	}

	gettimeofday(&end,NULL);
	if (!ENCRYPT) {
		tdiff = end.tv_sec - start.tv_sec;
		if (!tdiff)
			tdiff = 1;
		print_output("\nAverage DeCompression Speed: %6.3fMB/s\n",
			       (total / 1024 / 1024) / tdiff);
	}

	if (!NO_MD5) {
		int i,j;

		/* Flush final batch and join worker before finishing the digest. */
		runzip_md5_stop(control);
		md5_live = 0;
		md5_finish_ctx (&control->ctx, control->md5_resblock);
		if (HAS_MD5) {
			i64 fdinend = seekto_fdinend(control);
			i64 md5_wire = MD5_DIGEST_SIZE;

			if (ENCRYPT_AEAD)
				md5_wire = LRZ_AEAD_NONCE_LEN + MD5_DIGEST_SIZE + LRZ_AEAD_TAG_LEN;

			if (unlikely(fdinend == -1))
				failure_return(("Failed to seekto_fdinend in rzip_fd\n"), -1);
			if (unlikely(seekto_fdin(control, fdinend - md5_wire) == -1))
				failure_return(("Failed to seekto_fdin in rzip_fd\n"), -1);

			if (ENCRYPT_AEAD) {
				uchar sealed[LRZ_AEAD_NONCE_LEN + MD5_DIGEST_SIZE + LRZ_AEAD_TAG_LEN];
				uchar aad[8];
				size_t aad_len = 8, pt_len = MD5_DIGEST_SIZE;

				if (unlikely(read_all(control, fd_in, sealed, md5_wire) != md5_wire))
					fatal_return(("Failed to read AEAD md5 data in runzip_fd\n"), -1);
				aad[0] = 'L'; aad[1] = 'R'; aad[2] = 'Z'; aad[3] = 'I';
				aad[4] = (uchar)control->major_version;
				aad[5] = (uchar)control->minor_version;
				aad[6] = 3;
				aad[7] = 0x03;
				if (unlikely(!lrz_aead_open(control, LRZ_AEAD_KEY_DATA, aad, aad_len,
							    sealed, (size_t)md5_wire,
							    md5_stored, &pt_len)))
					return -1;
			} else {
				if (unlikely(read_all(control, fd_in, md5_stored, MD5_DIGEST_SIZE) != MD5_DIGEST_SIZE))
					fatal_return(("Failed to read md5 data in runzip_fd\n"), -1);
				if (ENCRYPT)
					if (unlikely(!lrz_decrypt(control, md5_stored, MD5_DIGEST_SIZE, control->salt_pass)))
						return -1;
			}
			for (i = 0; i < MD5_DIGEST_SIZE; i++)
				if (md5_stored[i] != control->md5_resblock[i]) {
					print_output("MD5 CHECK FAILED.\nStored:");
					for (j = 0; j < MD5_DIGEST_SIZE; j++)
						print_output("%02x", md5_stored[j] & 0xFF);
					print_output("\nOutput file:");
					for (j = 0; j < MD5_DIGEST_SIZE; j++)
						print_output("%02x", control->md5_resblock[j] & 0xFF);
					failure_return(("\n"), -1);
				}
		}

		if (HASH_CHECK || MAX_VERBOSE) {
			print_output("MD5: ");
			for (i = 0; i < MD5_DIGEST_SIZE; i++)
				print_output("%02x", control->md5_resblock[i] & 0xFF);
			print_output("\n");
		}

		if (CHECK_FILE) {
			FILE *md5_fstream;
			int i, j;

			if (TMP_OUTBUF)
				close_tmpoutbuf(control);
			memcpy(md5_stored, control->md5_resblock, MD5_DIGEST_SIZE);
			if (unlikely(seekto_fdhist(control, 0) == -1))
				fatal_return(("Failed to seekto_fdhist in runzip_fd\n"), -1);
			if (unlikely((md5_fstream = fdopen(fd_hist, "r")) == NULL))
				fatal_return(("Failed to fdopen fd_hist in runzip_fd\n"), -1);
			if (unlikely(md5_stream(md5_fstream, control->md5_resblock)))
				fatal_return(("Failed to md5_stream in runzip_fd\n"), -1);
			/* We don't close the file here as it's closed in main */
			for (i = 0; i < MD5_DIGEST_SIZE; i++)
				if (md5_stored[i] != control->md5_resblock[i]) {
					print_output("MD5 CHECK FAILED.\nStored:");
					for (j = 0; j < MD5_DIGEST_SIZE; j++)
						print_output("%02x", md5_stored[j] & 0xFF);
					print_output("\nOutput file:");
					for (j = 0; j < MD5_DIGEST_SIZE; j++)
						print_output("%02x", control->md5_resblock[j] & 0xFF);
					failure_return(("\n"), -1);
				}
			print_output("MD5 integrity of written file matches archive\n");
			if (!HAS_MD5)
				print_output("Note this lrzip archive did not have a stored md5 value.\n"
				"The archive decompression was validated with crc32 and the md5 hash was "
				"calculated on decompression\n");
		}
	}

	dealloc(control->runzip_buf);
	control->runzip_buf = NULL;
	control->runzip_buf_len = 0;
	return total;
}
