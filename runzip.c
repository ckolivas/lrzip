/*
   Copyright (C) 2006-2011 Con Kolivas
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

#include "md5.h"
#include "runzip.h"
#include "stream.h"
#include "util.h"
#include "lrzip.h"
#include "liblrzip.h"
/* needed for CRC routines */
#include "lzma/C/7zCrc.h"
static inline uchar read_u8(rzip_control *control, void *ss, int stream)
{
	uchar b;

	if (unlikely(read_stream(control, ss, stream, &b, 1) != 1))
		fatal("Stream read u8 failed\n");
	return b;
}

static inline u32 read_u32(rzip_control *control, void *ss, int stream)
{
	u32 ret;

	if (unlikely(read_stream(control, ss, stream, (uchar *)&ret, 4) != 4))
		fatal("Stream read u32 failed\n");
	return ret;
}

/* Read a variable length of chars dependant on how big the chunk was */
static inline i64 read_vchars(rzip_control *control, void *ss, int stream, int length)
{
	int bytes;
	i64 s = 0;

	for (bytes = 0; bytes < length; bytes++) {
		int bits = bytes * 8;

		uchar sb = read_u8(control, ss, stream);
		s |= (i64)sb << bits;
	}
	return s;
}

static i64 seekcur_fdout(rzip_control *control)
{
	if (!TMP_OUTBUF)
		return lseek(control->fd_out, 0, SEEK_CUR);
	return (control->out_relofs + control->out_ofs);
}

static i64 seekto_fdout(rzip_control *control, i64 pos)
{
	if (!TMP_OUTBUF)
		return lseek(control->fd_out, pos, SEEK_SET);
	control->out_ofs = pos - control->out_relofs;
	if (control->out_ofs > control->out_len)
		control->out_len = control->out_ofs;
	if (unlikely(control->out_ofs < 0 || control->out_ofs > control->out_maxlen)) {
		print_err("Trying to seek outside tmpoutbuf to %lld in seekto_fdout\n", control->out_ofs);
		return -1;
	}
	return pos;
}

static i64 seekto_fdhist(rzip_control *control, i64 pos)
{
	if (!TMP_OUTBUF)
		return lseek(control->fd_hist, pos, SEEK_SET);
	control->hist_ofs = pos - control->out_relofs;
	if (control->hist_ofs > control->out_len)
		control->out_len = control->hist_ofs;
	if (unlikely(control->hist_ofs < 0 || control->hist_ofs > control->out_maxlen)) {
		print_err("Trying to seek outside tmpoutbuf to %lld in seekto_fdhist\n", control->hist_ofs);
		return -1;
	}
	return pos;
}

static i64 seekcur_fdin(struct rzip_control *control)
{
	if (!TMP_INBUF)
		return lseek(control->fd_in, 0, SEEK_CUR);
	return (control->in_relofs + control->in_ofs);
}

static i64 read_header(rzip_control *control, void *ss, uchar *head)
{
	int chunk_bytes = 2;

	/* All chunks were unnecessarily encoded 8 bytes wide version 0.4x */
	if (control->major_version == 0 && control->minor_version == 4)
		chunk_bytes = 8;
	*head = read_u8(control, ss, 0);
	return read_vchars(control, ss, 0, chunk_bytes);
}

static i64 unzip_literal(rzip_control *control, void *ss, i64 len, int fd_out, uint32 *cksum)
{
	i64 stream_read;
	uchar *buf;

	if (unlikely(len < 0))
		failure("len %lld is negative in unzip_literal!\n",len);

	buf = (uchar *)malloc(len);
	if (unlikely(!buf))
		fatal("Failed to malloc literal buffer of size %lld\n", len);

	stream_read = read_stream(control, ss, 1, buf, len);
	if (unlikely(stream_read == -1 ))
		fatal("Failed to read_stream in unzip_literal\n");

	if (unlikely(write_1g(control, buf, (size_t)stream_read) != (ssize_t)stream_read))
		fatal("Failed to write literal buffer of size %lld\n", stream_read);

	if (!HAS_MD5)
		*cksum = CrcUpdate(*cksum, buf, stream_read);
	if (!NO_MD5)
		md5_process_bytes(buf, stream_read, &control->ctx);

	free(buf);
	return stream_read;
}

static i64 read_fdhist(struct rzip_control *control, void *buf, i64 len)
{
	if (!TMP_OUTBUF)
		return read_1g(control, control->fd_hist, buf, len);
	if (unlikely(len + control->hist_ofs > control->out_maxlen)) {
		print_err("Trying to read beyond end of tmpoutbuf in read_fdhist\n");
		return -1;
	}
	memcpy(buf, control->tmp_outbuf + control->hist_ofs, len);
	return len;
}

static i64 unzip_match(rzip_control *control, void *ss, i64 len, int fd_out, int fd_hist, uint32 *cksum, int chunk_bytes)
{
	i64 offset, n, total, cur_pos;
	uchar *buf, *off_buf;

	if (unlikely(len < 0))
		failure("len %lld is negative in unzip_match!\n",len);

	total = 0;
	cur_pos = seekcur_fdout(control);
	if (unlikely(cur_pos == -1))
		fatal("Seek failed on out file in unzip_match.\n");

	/* Note the offset is in a different format v0.40+ */
	offset = read_vchars(control, ss, 0, chunk_bytes);
	if (unlikely(seekto_fdhist(control, cur_pos - offset) == -1))
		fatal("Seek failed by %d from %d on history file in unzip_match\n",
		      offset, cur_pos);

	buf = (uchar *)malloc(len);
	if (unlikely(!buf))
		fatal("Failed to malloc match buffer of size %lld\n", len);
	off_buf = buf;

	while (len) {
		n = MIN(len, offset);

		if (unlikely(read_fdhist(control, off_buf, (size_t)n) != (ssize_t)n))
			fatal("Failed to read %d bytes in unzip_match\n", n);

		if (unlikely(write_1g(control, off_buf, (size_t)n) != (ssize_t)n))
			fatal("Failed to write %d bytes in unzip_match\n", n);

		if (!HAS_MD5)
			*cksum = CrcUpdate(*cksum, off_buf, n);
		if (!NO_MD5)
			md5_process_bytes(off_buf, n, &control->ctx);

		len -= n;
		off_buf += n;
		total += n;
	}

	free(buf);

	return total;
}

/* decompress a section of an open file. Call fatal() on error
   return the number of bytes that have been retrieved
 */
static i64 runzip_chunk(rzip_control *control, int fd_in, int fd_out, int fd_hist, i64 expected_size, i64 tally)
{
	uint32 good_cksum, cksum = 0;
	i64 len, ofs, total = 0;
	int l = -1, p = 0;
	char chunk_bytes;
	struct stat st;
	uchar head;
	void *ss;

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
		/* Read in the stored chunk byte width from the file */
		if (unlikely(read_1g(control, fd_in, &chunk_bytes, 1) != 1))
			fatal("Failed to read chunk_bytes size in runzip_chunk\n");
	}
	if (!tally && expected_size)
		print_maxverbose("Expected size: %lld\n", expected_size);
	print_maxverbose("Chunk byte width: %d\n", chunk_bytes);

	ofs = seekcur_fdin(control);
	if (unlikely(ofs == -1))
		fatal("Failed to seek input file in runzip_fd\n");

	if (fstat(fd_in, &st) || st.st_size - ofs == 0)
		return 0;

	ss = open_stream_in(control, fd_in, NUM_STREAMS);
	if (unlikely(!ss))
		fatal("Failed to open_stream_in in runzip_chunk\n");

	while ((len = read_header(control, ss, &head)) || head) {
		switch (head) {
			case 0:
				total += unzip_literal(control, ss, len, fd_out, &cksum);
				break;

			default:
				total += unzip_match(control, ss, len, fd_out, fd_hist, &cksum, chunk_bytes);
				break;
		}
		if (expected_size) {
			p = 100 * ((double)(tally + total) / (double)expected_size);
			if (p / 10 != l / 10)  {
				prog_done = (double)(tally + total) / (double)divisor[divisor_index];
				print_progress("%3d%%  %9.2f / %9.2f %s\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b",
						p, prog_done, prog_tsize, suffix[divisor_index] );
				l = p;
			}
		}
	}

	if (!HAS_MD5) {
		good_cksum = read_u32(control, ss, 0);
		if (unlikely(good_cksum != cksum))
			failure("Bad checksum: 0x%08x - expected: 0x%08x\n", cksum, good_cksum);
		print_maxverbose("Checksum for block: 0x%08x\n", cksum);
	}

	if (unlikely(close_stream_in(ss)))
		fatal("Failed to close stream!\n");

	return total;
}

/* Decompress an open file. Call fatal() on error
   return the number of bytes that have been retrieved
 */
i64 runzip_fd(rzip_control *control, int fd_in, int fd_out, int fd_hist, i64 expected_size)
{
	char md5_resblock[MD5_DIGEST_SIZE];
	char md5_stored[MD5_DIGEST_SIZE];
	struct timeval start,end;
	i64 total = 0;

	if (!NO_MD5)
		md5_init_ctx (&control->ctx);
	gettimeofday(&start,NULL);

	do {
		total += runzip_chunk(control, fd_in, fd_out, fd_hist, expected_size, total);
		if (TMP_OUTBUF)
			flush_tmpoutbuf(control);
		else if (STDOUT)
			dump_tmpoutfile(control, fd_out);
		if (TMP_INBUF)
			clear_tmpinbuf(control);
	} while (total < expected_size || (!expected_size && !control->eof));

	gettimeofday(&end,NULL);
	print_progress("\nAverage DeCompression Speed: %6.3fMB/s\n",
			(total / 1024 / 1024) / (double)((end.tv_sec-start.tv_sec)? : 1));

	if (!NO_MD5) {
		int i,j;

		md5_finish_ctx (&control->ctx, md5_resblock);
		if (HAS_MD5) {
#if 0
			/* Unnecessary, we should already be there */
			if (unlikely(lseek(fd_in, -MD5_DIGEST_SIZE, SEEK_END)) == -1)
				fatal("Failed to seek to md5 data in runzip_fd\n");
#endif
			if (unlikely(read_1g(control, fd_in, md5_stored, MD5_DIGEST_SIZE) != MD5_DIGEST_SIZE))
				fatal("Failed to read md5 data in runzip_fd\n");
			for (i = 0; i < MD5_DIGEST_SIZE; i++)
				if (md5_stored[i] != md5_resblock[i]) {
					print_output("MD5 CHECK FAILED.\nStored:");
					for (j = 0; j < MD5_DIGEST_SIZE; j++)
						print_output("%02x", md5_stored[j] & 0xFF);
					print_output("\nOutput file:");
					for (j = 0; j < MD5_DIGEST_SIZE; j++)
						print_output("%02x", md5_resblock[j] & 0xFF);
					failure("\n");
				}
		}

		if (HASH_CHECK || MAX_VERBOSE) {
			print_output("MD5: ");
			for (i = 0; i < MD5_DIGEST_SIZE; i++)
				print_output("%02x", md5_resblock[i] & 0xFF);
			print_output("\n");
		}

		if (CHECK_FILE) {
			FILE *md5_fstream;
			int i, j;

			if (TMP_OUTBUF)
				close_tmpoutbuf(control);
			memcpy(md5_stored, md5_resblock, MD5_DIGEST_SIZE);
			if (unlikely(seekto_fdhist(control, 0) == -1))
				fatal("Failed to seekto_fdhist in runzip_fd\n");
			if (unlikely((md5_fstream = fdopen(fd_hist, "r")) == NULL))
				fatal("Failed to fdopen fd_hist in runzip_fd\n");
			if (unlikely(md5_stream(md5_fstream, md5_resblock)))
				fatal("Failed to md5_stream in runzip_fd\n");
			/* We dont' close the file here as it's closed in main */
			for (i = 0; i < MD5_DIGEST_SIZE; i++)
				if (md5_stored[i] != md5_resblock[i]) {
					print_output("MD5 CHECK FAILED.\nStored:");
					for (j = 0; j < MD5_DIGEST_SIZE; j++)
						print_output("%02x", md5_stored[j] & 0xFF);
					print_output("\nOutput file:");
					for (j = 0; j < MD5_DIGEST_SIZE; j++)
						print_output("%02x", md5_resblock[j] & 0xFF);
					failure("\n");
				}
			print_output("MD5 integrity of written file matches archive\n");
			if (!HAS_MD5)
				print_output("Note this lrzip archive did not have a stored md5 value.\n"
				"The archive decompression was validated with crc32 and the md5 hash was "
				"calculated on decompression\n");
		}
	}

	return total;
}
