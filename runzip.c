/*
   Copyright (C) Andrew Tridgell 1998-2003
   Con Kolivas 2006-2009

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
/* rzip decompression algorithm */

#include "rzip.h"

static inline uchar read_u8(void *ss, int stream)
{
	uchar b;
	if (read_stream(ss, stream, &b, 1) != 1)
		fatal("Stream read u8 failed\n");
	return b;
}

static inline u16 read_u16(void *ss, int stream)
{
	u16 ret;

	if (read_stream(ss, stream, (uchar *)&ret, 2) != 2)
		fatal("Stream read u16 failed\n");
	return ret;
}

static inline u32 read_u32(void *ss, int stream)
{
	u32 ret;

	if (read_stream(ss, stream, (uchar *)&ret, 4) != 4)
		fatal("Stream read u32 failed\n");
	return ret;
}

static inline i64 read_i64(void *ss, int stream)
{
	i64 ret;

	if (read_stream(ss, stream, (uchar *)&ret, 8) != 8)
		fatal("Stream read i64 failed\n");
	return ret;
}

static u16 read_header_v03(void *ss, uchar *head)
{
	*head = read_u8(ss, 0);
	return read_u16(ss, 0);
}

static i64 read_header(void *ss, uchar *head)
{
	if (control.major_version == 0 && control.minor_version < 4)
		return read_header_v03(ss, head);
	*head = read_u8(ss, 0);
	return read_i64(ss, 0);
}

static i64 unzip_literal(void *ss, i64 len, int fd_out, uint32 *cksum)
{
	uchar *buf;

	if (len < 0)
		fatal("len %lld is negative in unzip_literal!\n",len);

	if (sizeof(long) == 4 && len > (1UL << 31))
		fatal("Unable to allocate a chunk this big on 32bit userspace. Can't decompress this file on this hardware\n");

	buf = malloc((size_t)len);
	if (!buf)
		fatal("Failed to allocate literal buffer of size %lld\n", len);

	read_stream(ss, 1, buf, len);
	if (write_1g(fd_out, buf, (size_t)len) != (ssize_t)len)
		fatal("Failed to write literal buffer of size %lld\n", len);

	*cksum = CrcUpdate(*cksum, buf, len);

	free(buf);
	return len;
}

static i64 unzip_match_v03(void *ss, i64 len, int fd_out, int fd_hist, uint32 *cksum)
{
	u32 offset;
	i64 n, total = 0;
	i64 cur_pos = lseek(fd_out, 0, SEEK_CUR);

	if (cur_pos == -1)
		fatal("Seek failed on out file in unzip_match.\n");

	offset = read_u32(ss, 0);

	if (lseek(fd_hist, cur_pos - offset, SEEK_SET) == -1)
		fatal("Seek failed by %d from %d on history file in unzip_match - %s\n",
		      offset, cur_pos, strerror(errno));

	while (len) {
		uchar *buf;
		n = MIN(len, offset);

		buf = malloc((size_t)n);
		if (!buf)
			fatal("Failed to allocate %d bytes in unzip_match\n", n);

		if (read_1g(fd_hist, buf, (size_t)n) != (ssize_t)n)
			fatal("Failed to read %d bytes in unzip_match\n", n);

		if (write_1g(fd_out, buf, (size_t)n) != (ssize_t)n)
			fatal("Failed to write %d bytes in unzip_match\n", n);

		*cksum = CrcUpdate(*cksum, buf, n);

		len -= n;
		free(buf);
		total += n;
	}

	return total;
}

static i64 unzip_match(void *ss, i64 len, int fd_out, int fd_hist, uint32 *cksum)
{
	i64 cur_pos;
	i64 offset, n, total;

	if (len < 0)
		fatal("len %lld is negative in unzip_match!\n",len);

	if (control.major_version == 0 && control.minor_version < 4)
		return unzip_match_v03(ss, len, fd_out, fd_hist, cksum);

	total = 0;
	cur_pos = lseek(fd_out, 0, SEEK_CUR);
	if (cur_pos == -1)
		fatal("Seek failed on out file in unzip_match.\n");

	/* Note the offset is in a different format v0.40+ */
	offset = read_i64(ss, 0);
	if (lseek(fd_hist, cur_pos - offset, SEEK_SET) == -1)
		fatal("Seek failed by %d from %d on history file in unzip_match - %s\n",
		      offset, cur_pos, strerror(errno));

	while (len) {
		uchar *buf;
		n = MIN(len, offset);

		buf = malloc((size_t)n);
		if (!buf)
			fatal("Failed to allocate %d bytes in unzip_match\n", n);

		if (read_1g(fd_hist, buf, (size_t)n) != (ssize_t)n)
			fatal("Failed to read %d bytes in unzip_match\n", n);

		if (write_1g(fd_out, buf, (size_t)n) != (ssize_t)n)
			fatal("Failed to write %d bytes in unzip_match\n", n);

		*cksum = CrcUpdate(*cksum, buf, n);

		len -= n;
		free(buf);
		total += n;
	}

	return total;
}

/* decompress a section of an open file. Call fatal() on error
   return the number of bytes that have been retrieved
 */
static i64 runzip_chunk(int fd_in, int fd_out, int fd_hist, i64 expected_size, i64 tally)
{
	uchar head;
	i64 len;
	struct stat st;
	void *ss;
	i64 ofs;
	i64 total = 0;
	uint32 good_cksum, cksum = 0;
	int l = -1, p = 0;

	/* for display of progress */
	char *suffix[] = {"","KB","MB","GB"};
	unsigned long divisor[] = {1,1024,1048576,1073741824U};
	int divisor_index;
	double prog_done, prog_tsize;

	if (expected_size > (i64)10737418240ULL)	/* > 10GB */
		divisor_index = 3;
	else if (expected_size > 10485760)	/* > 10MB */
		divisor_index = 2;
	else if (expected_size > 10240)	/* > 10KB */
		divisor_index = 1;
	else
		divisor_index = 0;

	prog_tsize = (long double)expected_size / (long double)divisor[divisor_index];

	ofs = lseek(fd_in, 0, SEEK_CUR);
	if (ofs == -1)
		fatal("Failed to seek input file in runzip_fd\n");

	if (fstat(fd_in, &st) != 0 || st.st_size-ofs == 0)
		return 0;

	ss = open_stream_in(fd_in, NUM_STREAMS);
	if (!ss)
		fatal(NULL);

	while ((len = read_header(ss, &head)) || head) {
		switch (head) {
			case 0:
				total += unzip_literal(ss, len, fd_out, &cksum);
				break;

			default:
				total += unzip_match(ss, len, fd_out, fd_hist, &cksum);
				break;
		}
		p = 100 * ((double)(tally+total) / (double)expected_size);
		if (control.flags & FLAG_SHOW_PROGRESS) {
			if ( p != l )  {
				prog_done = (double)(tally+total) / (double)divisor[divisor_index];
				fprintf(stderr, "%3d%%  %9.2f / %9.2f %s\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b",
						p, prog_done, prog_tsize, suffix[divisor_index] );
				fflush(stderr);
				l = p;
			}
		}
	}

	good_cksum = read_u32(ss, 0);
	if (good_cksum != cksum)
		fatal("Bad checksum 0x%08x - expected 0x%08x\n", cksum, good_cksum);

	if (close_stream_in(ss) != 0)
		fatal("Failed to close stream!\n");

	return total;
}

/* decompress a open file. Call fatal() on error
   return the number of bytes that have been retrieved
 */

i64 runzip_fd(int fd_in, int fd_out, int fd_hist, i64 expected_size)
{
	i64 total = 0;
	struct timeval start,end;

	gettimeofday(&start,NULL);

	while (total < expected_size)
		total += runzip_chunk(fd_in, fd_out, fd_hist, expected_size, total);

	gettimeofday(&end,NULL);
	if (control.flags & FLAG_SHOW_PROGRESS)
		fprintf(stderr, "\nAverage DeCompression Speed: %6.3fMB/s\n",
			(total / 1024 / 1024) / (double)((end.tv_sec-start.tv_sec)? : 1));
	return total;
}
