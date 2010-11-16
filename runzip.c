/*
   Copyright (C) Andrew Tridgell 1998-2003
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
/* rzip decompression algorithm */

#include "rzip.h"

static inline uchar read_u8(void *ss, int stream)
{
	uchar b;

	if (unlikely(read_stream(ss, stream, &b, 1) != 1))
		fatal("Stream read u8 failed\n");
	return b;
}

static inline u32 read_u32(void *ss, int stream)
{
	u32 ret;

	if (unlikely(read_stream(ss, stream, (uchar *)&ret, 4) != 4))
		fatal("Stream read u32 failed\n");
	return ret;
}

/* Read a variable length of chars dependant on how big the chunk was */
static inline i64 read_vchars(void *ss, int stream, int length)
{
	int bytes;
	i64 s = 0;

	for (bytes = 0; bytes < length; bytes++) {
		int bits = bytes * 8;

		uchar sb = read_u8(ss, stream);
		s |= (i64)sb << bits;
	}
	return s;
}

static i64 read_header(void *ss, uchar *head)
{
	int chunk_bytes = 2;

	/* All chunks were unnecessarily encoded 8 bytes wide version 0.4x */
	if (control.major_version == 0 && control.minor_version == 4)
		chunk_bytes = 8;
	*head = read_u8(ss, 0);
	return read_vchars(ss, 0, chunk_bytes);
}

static i64 unzip_literal(void *ss, i64 len, int fd_out, uint32 *cksum)
{
	uchar *buf;

	if (unlikely(len < 0))
		fatal("len %lld is negative in unzip_literal!\n",len);

	/* We use anonymous mmap instead of malloc to allow us to allocate up
	 * to 2^44 even on 32 bits */
	buf = (uchar *)mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	if (unlikely(buf == MAP_FAILED))
		fatal("Failed to allocate literal buffer of size %lld\n", len);

	read_stream(ss, 1, buf, len);
	if (unlikely(write_1g(fd_out, buf, (size_t)len) != (ssize_t)len))
		fatal("Failed to write literal buffer of size %lld\n", len);

	*cksum = CrcUpdate(*cksum, buf, len);

	munmap(buf, len);
	return len;
}

static i64 unzip_match(void *ss, i64 len, int fd_out, int fd_hist, uint32 *cksum, int chunk_bytes)
{
	i64 offset, n, total, cur_pos;

	if (unlikely(len < 0))
		fatal("len %lld is negative in unzip_match!\n",len);

	total = 0;
	cur_pos = lseek(fd_out, 0, SEEK_CUR);
	if (unlikely(cur_pos == -1))
		fatal("Seek failed on out file in unzip_match.\n");

	/* Note the offset is in a different format v0.40+ */
	offset = read_vchars(ss, 0, chunk_bytes);
	if (unlikely(lseek(fd_hist, cur_pos - offset, SEEK_SET) == -1))
		fatal("Seek failed by %d from %d on history file in unzip_match - %s\n",
		      offset, cur_pos, strerror(errno));

	while (len) {
		uchar *buf;
		n = MIN(len, offset);

		buf = (uchar *)mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
		if (unlikely(buf == MAP_FAILED))
			fatal("Failed to allocate match buffer of size %lld\n", n);

		if (unlikely(read_1g(fd_hist, buf, (size_t)n) != (ssize_t)n))
			fatal("Failed to read %d bytes in unzip_match\n", n);

		if (unlikely(write_1g(fd_out, buf, (size_t)n) != (ssize_t)n))
			fatal("Failed to write %d bytes in unzip_match\n", n);

		*cksum = CrcUpdate(*cksum, buf, n);

		len -= n;
		munmap(buf, n);
		total += n;
	}

	return total;
}

/* decompress a section of an open file. Call fatal() on error
   return the number of bytes that have been retrieved
 */
static i64 runzip_chunk(int fd_in, int fd_out, int fd_hist, i64 expected_size, i64 tally)
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
	if (control.major_version == 0 && control.minor_version < 4)
		chunk_bytes = 4;
	else if (control.major_version == 0 && control.minor_version == 4)
		chunk_bytes = 8;
	else {
		/* Read in the stored chunk byte width from the file */
		if (unlikely(read(fd_in, &chunk_bytes, 1) != 1))
			fatal("Failed to read chunk_bytes size in runzip_chunk\n");
	}
	if (!tally)
		print_maxverbose("\nExpected size: %lld", expected_size);
	print_maxverbose("\nChunk byte width: %d\n", chunk_bytes);

	ofs = lseek(fd_in, 0, SEEK_CUR);
	if (unlikely(ofs == -1))
		fatal("Failed to seek input file in runzip_fd\n");

	if (fstat(fd_in, &st) || st.st_size - ofs == 0)
		return 0;

	ss = open_stream_in(fd_in, NUM_STREAMS);
	if (unlikely(!ss))
		fatal("Failed to open_stream_in in runzip_chunk\n");

	while ((len = read_header(ss, &head)) || head) {
		switch (head) {
			case 0:
				total += unzip_literal(ss, len, fd_out, &cksum);
				break;

			default:
				total += unzip_match(ss, len, fd_out, fd_hist, &cksum, chunk_bytes);
				break;
		}
		p = 100 * ((double)(tally + total) / (double)expected_size);
		if (p / 10 != l / 10)  {
			prog_done = (double)(tally + total) / (double)divisor[divisor_index];
			print_progress("%3d%%  %9.2f / %9.2f %s\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b",
					p, prog_done, prog_tsize, suffix[divisor_index] );
			l = p;
		}
	}

	good_cksum = read_u32(ss, 0);
	if (unlikely(good_cksum != cksum))
		fatal("Bad checksum 0x%08x - expected 0x%08x\n", cksum, good_cksum);

	if (unlikely(close_stream_in(ss)))
		fatal("Failed to close stream!\n");

	return total;
}

/* Decompress an open file. Call fatal() on error
   return the number of bytes that have been retrieved
 */
i64 runzip_fd(int fd_in, int fd_out, int fd_hist, i64 expected_size)
{
	struct timeval start,end;
	i64 total = 0;

	gettimeofday(&start,NULL);

	while (total < expected_size)
		total += runzip_chunk(fd_in, fd_out, fd_hist, expected_size, total);

	gettimeofday(&end,NULL);
	print_progress("\nAverage DeCompression Speed: %6.3fMB/s\n",
			(total / 1024 / 1024) / (double)((end.tv_sec-start.tv_sec)? : 1));
	return total;
}
