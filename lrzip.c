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

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <sys/types.h>
#ifdef HAVE_SYS_STAT_H
# include <sys/stat.h>
#endif
#include <fcntl.h>
#include <sys/statvfs.h>
#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif
#include <arpa/inet.h>
#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif
#include <sys/time.h>

#include "md5.h"
#include "rzip.h"
#include "runzip.h"
#include "util.h"
#include "liblrzip.h" /* flag defines */

void write_magic(rzip_control *control, int fd_in, int fd_out)
{
	struct timeval tv;
	struct stat st;
	char magic[40];
	int i;

	memset(magic, 0, sizeof(magic));
	strcpy(magic, "LRZI");
	magic[4] = LRZIP_MAJOR_VERSION;
	magic[5] = LRZIP_MINOR_VERSION;

	if (unlikely(fstat(fd_in, &st)))
		fatal("bad magic file descriptor!?\n");

	/* File size is stored as zero for streaming STDOUT blocks when the
	 * file size is unknown. */
	if (!STDIN || !STDOUT || control->eof)
		memcpy(&magic[6], &control->st_size, 8);

	/* save LZMA compression flags */
	if (LZMA_COMPRESS) {
		for (i = 0; i < 5; i++)
			magic[i + 16] = (char)control->lzma_properties[i];
	}

	/* This is a flag that the archive contains an md5 sum at the end
	 * which can be used as an integrity check instead of crc check.
	 * crc is still stored for compatibility with 0.5 versions.
	 */
	magic[21] = 1;
	if (control->encrypt)
		magic[22] = 1;

	if (unlikely(gettimeofday(&tv, NULL)))
		fatal("Failed to gettimeofday in write_magic\n");
	control->secs = tv.tv_sec;
	control->usecs = tv.tv_usec;
	memcpy(&magic[23], &control->secs, 8);
	memcpy(&magic[31], &control->usecs, 8);

	if (unlikely(lseek(fd_out, 0, SEEK_SET)))
		fatal("Failed to seek to BOF to write Magic Header\n");

	if (unlikely(write(fd_out, magic, sizeof(magic)) != sizeof(magic)))
		fatal("Failed to write magic header\n");
}

void read_magic(rzip_control *control, int fd_in, i64 *expected_size)
{
	char magic[40];
	uint32_t v;
	int md5, i;

	memset(magic, 0, 40);
	if (unlikely(read(fd_in, magic, sizeof(magic)) != sizeof(magic)))
		fatal("Failed to read magic header\n");

	*expected_size = 0;

	if (unlikely(strncmp(magic, "LRZI", 4)))
		failure("Not an lrzip file\n");

	memcpy(&control->major_version, &magic[4], 1);
	memcpy(&control->minor_version, &magic[5], 1);

	/* Support the convoluted way we described size in versions < 0.40 */
	if (control->major_version == 0 && control->minor_version < 4) {
		memcpy(&v, &magic[6], 4);
		*expected_size = ntohl(v);
		memcpy(&v, &magic[10], 4);
		*expected_size |= ((i64)ntohl(v)) << 32;
	} else {
		memcpy(expected_size, &magic[6], 8);
		if (control->major_version == 0 && control->minor_version > 5) {
			if (magic[22] == 1)
				control->encrypt = 1;
			memcpy(&control->secs, &magic[23], 8);
			memcpy(&control->usecs, &magic[31], 8);
			print_maxverbose("Seconds %lld\n", control->secs);
			print_maxverbose("Microseconds %lld\n", control->usecs);
		}
	}

	/* restore LZMA compression flags only if stored */
	if ((int) magic[16]) {
		for (i = 0; i < 5; i++)
			control->lzma_properties[i] = magic[i + 16];
	}

	/* Whether this archive contains md5 data at the end or not */
	md5 = magic[21];
	if (md5 == 1)
		control->flags |= FLAG_MD5;

	print_verbose("Detected lrzip version %d.%d file.\n", control->major_version, control->minor_version);
	if (control->major_version > LRZIP_MAJOR_VERSION ||
	    (control->major_version == LRZIP_MAJOR_VERSION && control->minor_version > LRZIP_MINOR_VERSION))
		print_output("Attempting to work with file produced by newer lrzip version %d.%d file.\n", control->major_version, control->minor_version);
}

/* preserve ownership and permissions where possible */
void preserve_perms(rzip_control *control, int fd_in, int fd_out)
{
	struct stat st;

	if (unlikely(fstat(fd_in, &st)))
		fatal("Failed to fstat input file\n");
	if (unlikely(fchmod(fd_out, (st.st_mode & 0777))))
		print_err("Warning, unable to set permissions on %s\n", control->outfile);

	/* chown fail is not fatal */
	if (unlikely(fchown(fd_out, st.st_uid, st.st_gid)))
		print_err("Warning, unable to set owner on %s\n", control->outfile);
}

/* Open a temporary outputfile to emulate stdout */
int open_tmpoutfile(rzip_control *control)
{
	int fd_out;

	if (STDOUT && !TEST_ONLY)
		print_verbose("Outputting to stdout.\n");
	if (control->tmpdir) {
		control->outfile = realloc(NULL, strlen(control->tmpdir) + 16);
		if (unlikely(!control->outfile))
			fatal("Failed to allocate outfile name\n");
		strcpy(control->outfile, control->tmpdir);
		strcat(control->outfile, "lrzipout.XXXXXX");
	} else {
		control->outfile = realloc(NULL, 16);
		if (unlikely(!control->outfile))
			fatal("Failed to allocate outfile name\n");
		strcpy(control->outfile, "lrzipout.XXXXXX");
	}

	fd_out = mkstemp(control->outfile);
	if (unlikely(fd_out == -1))
		fatal("Failed to create out tmpfile: %s\n", control->outfile);
	register_outfile(control->outfile, TEST_ONLY || STDOUT || !KEEP_BROKEN);
	return fd_out;
}

/* Dump temporary outputfile to perform stdout */
void dump_tmpoutfile(rzip_control *control, int fd_out)
{
	FILE *tmpoutfp;
	int tmpchar;

	/* flush anything not yet in the temporary file */
	fsync(fd_out);
	tmpoutfp = fdopen(fd_out, "r");
	if (unlikely(tmpoutfp == NULL))
		fatal("Failed to fdopen out tmpfile\n");
	rewind(tmpoutfp);

	if (!TEST_ONLY) {
		print_verbose("Dumping temporary file to stdout.\n");
		while ((tmpchar = fgetc(tmpoutfp)) != EOF)
			putchar(tmpchar);
		fflush(stdout);
		rewind(tmpoutfp);
	}

	if (unlikely(ftruncate(fd_out, 0)))
		fatal("Failed to ftruncate fd_out in dump_tmpoutfile\n");
}

/* Open a temporary inputfile to perform stdin decompression */
int open_tmpinfile(rzip_control *control)
{
	int fd_in;

	if (control->tmpdir) {
		control->infile = malloc(strlen(control->tmpdir) + 15);
		if (unlikely(!control->infile))
			fatal("Failed to allocate infile name\n");
		strcpy(control->infile, control->tmpdir);
		strcat(control->infile, "lrzipin.XXXXXX");
	} else {
		control->infile = malloc(15);
		if (unlikely(!control->infile))
			fatal("Failed to allocate infile name\n");
		strcpy(control->infile, "lrzipin.XXXXXX");
	}

	fd_in = mkstemp(control->infile);
	if (unlikely(fd_in == -1))
		fatal("Failed to create in tmpfile: %s\n", control->infile);
	register_infile(control->infile, (DECOMPRESS || TEST_ONLY) && STDIN);
	/* Unlink temporary file immediately to minimise chance of files left
	 * lying around in cases of failure. */
	if (unlikely(unlink(control->infile)))
		fatal("Failed to unlink tmpfile: %s\n", control->infile);
	return fd_in;
}

/* Read data from stdin into temporary inputfile */
void read_tmpinfile(rzip_control *control, int fd_in)
{
	FILE *tmpinfp;
	int tmpchar;

	if (control->flags & FLAG_SHOW_PROGRESS)
		fprintf(control->msgout, "Copying from stdin.\n");
	tmpinfp = fdopen(fd_in, "w+");
	if (unlikely(tmpinfp == NULL))
		fatal("Failed to fdopen in tmpfile\n");

	while ((tmpchar = getchar()) != EOF)
		fputc(tmpchar, tmpinfp);

	fflush(tmpinfp);
	rewind(tmpinfp);
}

/*
  decompress one file from the command line
*/
void decompress_file(rzip_control *control)
{
	char *tmp, *tmpoutfile, *infilecopy = NULL;
	int fd_in, fd_out = -1, fd_hist = -1;
	i64 expected_size, free_space;
	struct statvfs fbuf;

	if (!STDIN) {
		if ((tmp = strrchr(control->infile, '.')) && strcmp(tmp,control->suffix)) {
			/* make sure infile has an extension. If not, add it
			  * because manipulations may be made to input filename, set local ptr
			*/
			infilecopy = malloc(strlen(control->infile) + strlen(control->suffix) + 1);
			if (unlikely(infilecopy == NULL))
				fatal("Failed to allocate memory for infile suffix\n");
			else {
				strcpy(infilecopy, control->infile);
				strcat(infilecopy, control->suffix);
			}
		} else
			infilecopy = strdup(control->infile);
		/* regardless, infilecopy has the input filename */
	}

	if (!STDOUT && !TEST_ONLY) {
		/* if output name already set, use it */
		if (control->outname) {
			control->outfile = strdup(control->outname);
		} else {
			/* default output name from infilecopy
			 * test if outdir specified. If so, strip path from filename of
			 * infilecopy, then remove suffix.
			*/
			if (control->outdir && (tmp = strrchr(infilecopy, '/')))
				tmpoutfile = strdup(tmp + 1);
			else
				tmpoutfile = strdup(infilecopy);

			/* remove suffix to make outfile name */
			if ((tmp = strrchr(tmpoutfile, '.')) && !strcmp(tmp, control->suffix))
				*tmp='\0';

			control->outfile = malloc((control->outdir == NULL? 0: strlen(control->outdir)) + strlen(tmpoutfile) + 1);
			if (unlikely(!control->outfile))
				fatal("Failed to allocate outfile name\n");

			if (control->outdir) {	/* prepend control->outdir */
				strcpy(control->outfile, control->outdir);
				strcat(control->outfile, tmpoutfile);
			} else
				strcpy(control->outfile, tmpoutfile);
			free(tmpoutfile);
		}

		if (!STDOUT)
			print_progress("Output filename is: %s...Decompressing...\n", control->outfile);
	}

	if (STDIN) {
		fd_in = open_tmpinfile(control);
		read_tmpinfile(control, fd_in);
	} else {
		fd_in = open(infilecopy, O_RDONLY);
		if (unlikely(fd_in == -1)) {
			fatal("Failed to open %s\n", infilecopy);
		}
	}

	if (!(TEST_ONLY | STDOUT)) {
		if (FORCE_REPLACE)
			fd_out = open(control->outfile, O_WRONLY | O_CREAT | O_TRUNC, 0666);
		else
			fd_out = open(control->outfile, O_WRONLY | O_CREAT | O_EXCL, 0666);
		if (unlikely(fd_out == -1)) {
			/* We must ensure we don't delete a file that already
			 * exists just because we tried to create a new one */
			control->flags |= FLAG_KEEP_BROKEN;
			fatal("Failed to create %s\n", control->outfile);
		}

		preserve_perms(control, fd_in, fd_out);
	} else
		fd_out = open_tmpoutfile(control);
	control->fd_out = fd_out;

        read_magic(control, fd_in, &expected_size);

        if (!STDOUT) {
                /* Check if there's enough free space on the device chosen to fit the
                * decompressed file. */
                if (unlikely(fstatvfs(fd_out, &fbuf)))
                        fatal("Failed to fstatvfs in decompress_file\n");
                free_space = fbuf.f_bsize * fbuf.f_bavail;
                if (free_space < expected_size) {
                        if (FORCE_REPLACE)
                                print_err("Warning, inadequate free space detected, but attempting to decompress due to -f option being used.\n");
                        else
                                failure("Inadequate free space to decompress file, use -f to override.\n");
                }
        }

	fd_hist = open(control->outfile, O_RDONLY);
	if (unlikely(fd_hist == -1))
		fatal("Failed to open history file %s\n", control->outfile);

	/* Unlink temporary file as soon as possible */
	if (unlikely((STDOUT || TEST_ONLY) && unlink(control->outfile)))
		fatal("Failed to unlink tmpfile: %s\n", control->outfile);



	if (NO_MD5)
		print_verbose("Not performing MD5 hash check\n");
	if (HAS_MD5)
		print_verbose("MD5 ");
	else
		print_verbose("CRC32 ");
	print_verbose("being used for integrity testing.\n");

	print_progress("Decompressing...");

	runzip_fd(control, fd_in, fd_out, fd_hist, expected_size);

	if (STDOUT)
		dump_tmpoutfile(control, fd_out);

	/* if we get here, no fatal errors during decompression */
	print_progress("\r");
	if (!(STDOUT | TEST_ONLY))
		print_output("Output filename is: %s: ", control->outfile);
        print_progress("[OK] - %lld bytes                                \n", expected_size);

	if (unlikely(close(fd_hist) || close(fd_out)))
		fatal("Failed to close files\n");

	close(fd_in);

	if (!KEEP_FILES) {
		if (unlikely(unlink(control->infile)))
			fatal("Failed to unlink %s\n", infilecopy);
	}

	free(control->outfile);
	free(infilecopy);
}

void get_header_info(rzip_control *control, int fd_in, uchar *ctype, i64 *c_len, i64 *u_len, i64 *last_head)
{
	if (unlikely(read(fd_in, ctype, 1) != 1))
		fatal("Failed to read in get_header_info\n");

	if (control->major_version == 0 && control->minor_version < 4) {
		u32 c_len32, u_len32, last_head32;

		if (unlikely(read(fd_in, &c_len32, 4) != 4))
			fatal("Failed to read in get_header_info");
		if (unlikely(read(fd_in, &u_len32, 4) != 4))
			fatal("Failed to read in get_header_info");
		if (unlikely(read(fd_in, &last_head32, 4) != 4))
			fatal("Failed to read in get_header_info");
		*c_len = c_len32;
		*u_len = u_len32;
		*last_head = last_head32;
	} else {
		if (unlikely(read(fd_in, c_len, 8) != 8))
			fatal("Failed to read in get_header_info");
		if (unlikely(read(fd_in, u_len, 8) != 8))
			fatal("Failed to read in get_header_info");
		if (unlikely(read(fd_in, last_head, 8) != 8))
			fatal("Failed to read_i64 in get_header_info");
	}
}

void get_fileinfo(rzip_control *control)
{
	i64 expected_size, infile_size;
	int seekspot, fd_in;
	char chunk_byte = 0;
	long double cratio;
	uchar ctype = 0;
	struct stat st;

	char *tmp, *infilecopy = NULL;

	if (!STDIN) {
		if ((tmp = strrchr(control->infile, '.')) && strcmp(tmp,control->suffix)) {
			infilecopy = malloc(strlen(control->infile) + strlen(control->suffix) + 1);
			if (unlikely(infilecopy == NULL))
				fatal("Failed to allocate memory for infile suffix\n");
			else {
				strcpy(infilecopy, control->infile);
				strcat(infilecopy, control->suffix);
			}
		} else
			infilecopy = strdup(control->infile);
	}

	if (STDIN)
		fd_in = 0;
	else {
		fd_in = open(infilecopy, O_RDONLY);
		if (unlikely(fd_in == -1))
			fatal("Failed to open %s\n", infilecopy);
	}

	/* Get file size */
	if (unlikely(fstat(fd_in, &st)))
		fatal("bad magic file descriptor!?\n");
	memcpy(&infile_size, &st.st_size, 8);

	/* Get decompressed size */
	read_magic(control, fd_in, &expected_size);

	if (control->major_version == 0 && control->minor_version > 4) {
		if (unlikely(read(fd_in, &chunk_byte, 1) != 1))
			fatal("Failed to read chunk_byte in get_fileinfo\n");
	}

	/* Version < 0.4 had different file format */
	if (control->major_version == 0 && control->minor_version < 4)
		seekspot = 50;
	else if (control->major_version == 0 && control->minor_version == 4)
		seekspot = 74;
	else if (control->major_version == 0 && control->minor_version == 5)
		seekspot = 75;
	if (unlikely(lseek(fd_in, seekspot, SEEK_SET) == -1))
		fatal("Failed to lseek in get_fileinfo\n");

	/* Read the compression type of the first block. It's possible that
	   not all blocks are compressed so this may not be accurate.
	 */
	if (unlikely(read(fd_in, &ctype, 1) != 1))
		fatal("Failed to read in get_fileinfo\n");

	cratio = (long double)expected_size / (long double)infile_size;

	print_output("%s:\nlrzip version: %d.%d file\n", infilecopy, control->major_version, control->minor_version);
	print_output("Compression: ");
	if (ctype == CTYPE_NONE)
		print_output("rzip alone\n");
	else if (ctype == CTYPE_BZIP2)
		print_output("rzip + bzip2\n");
	else if (ctype == CTYPE_LZO)
		print_output("rzip + lzo\n");
	else if (ctype == CTYPE_LZMA)
		print_output("rzip + lzma\n");
	else if (ctype == CTYPE_GZIP)
		print_output("rzip + gzip\n");
	else if (ctype == CTYPE_ZPAQ)
		print_output("rzip + zpaq\n");
	else
		print_output("Dunno wtf\n");
	print_output("Decompressed file size: %llu\n", expected_size);
	print_output("Compressed file size: %llu\n", infile_size);
	print_output("Compression ratio: %.3Lf\n", cratio);

	if (HAS_MD5) {
		char md5_stored[MD5_DIGEST_SIZE];
		int i;

		print_output("MD5 used for integrity testing\n");
		if (unlikely(lseek(fd_in, -MD5_DIGEST_SIZE, SEEK_END)) == -1)
			fatal("Failed to seek to md5 data in runzip_fd\n");
		if (unlikely(read(fd_in, md5_stored, MD5_DIGEST_SIZE) != MD5_DIGEST_SIZE))
			fatal("Failed to read md5 data in runzip_fd\n");
		print_output("MD5: ");
		for (i = 0; i < MD5_DIGEST_SIZE; i++)
			print_output("%02x", md5_stored[i] & 0xFF);
		print_output("\n");
	} else
		print_output("CRC32 used for integrity testing\n");

	if (VERBOSE || MAX_VERBOSE) {
		i64 u_len, c_len, last_head, utotal = 0, ctotal = 0, ofs = 25,
		    stream_head[2];
		int header_length = 25, stream = 0, chunk = 0;

		if (control->major_version == 0 && control->minor_version < 4) {
			ofs = 24;
			header_length = 13;
		}
		if (control->major_version == 0 && control->minor_version == 4)
			ofs = 24;
next_chunk:
		stream = 0;
		stream_head[0] = 0;
		stream_head[1] = stream_head[0] + header_length;

		print_output("Rzip chunk %d:\n", ++chunk);
		if (chunk_byte)
			print_verbose("Chunk byte width: %d\n", chunk_byte);
		while (stream < NUM_STREAMS) {
			int block = 1;

			if (unlikely(lseek(fd_in, stream_head[stream] + ofs, SEEK_SET)) == -1)
				fatal("Failed to seek to header data in get_fileinfo\n");
			get_header_info(control, fd_in, &ctype, &c_len, &u_len, &last_head);

			print_output("Stream: %d\n", stream);
			print_maxverbose("Offset: %lld\n", ofs);
			print_output("Block\tComp\tPercent\tSize\n");
			do {
				i64 head_off;

				if (unlikely(last_head + ofs > infile_size))
					failure("Offset greater than archive size, likely corrupted/truncated archive.\n");
				if (unlikely(head_off = lseek(fd_in, last_head + ofs, SEEK_SET)) == -1)
					fatal("Failed to seek to header data in get_fileinfo\n");
				get_header_info(control, fd_in, &ctype, &c_len, &u_len, &last_head);
				if (unlikely(last_head < 0 || c_len < 0 || u_len < 0))
					failure("Entry negative, likely corrupted archive.\n");
				print_output("%d\t", block);
				if (ctype == CTYPE_NONE)
					print_output("none");
				else if (ctype == CTYPE_BZIP2)
					print_output("bzip2");
				else if (ctype == CTYPE_LZO)
					print_output("lzo");
				else if (ctype == CTYPE_LZMA)
					print_output("lzma");
				else if (ctype == CTYPE_GZIP)
					print_output("gzip");
				else if (ctype == CTYPE_ZPAQ)
					print_output("zpaq");
				else
					print_output("Dunno wtf");
				utotal += u_len;
				ctotal += c_len;
				print_output("\t%.1f%%\t%lld / %lld", (double)c_len / (double)(u_len / 100), c_len, u_len);
				print_maxverbose("\tOffset: %lld\tHead: %lld", head_off, last_head);
				print_output("\n");
				block++;
			} while (last_head);
			++stream;
		}
		if (unlikely((ofs = lseek(fd_in, c_len, SEEK_CUR)) == -1))
			fatal("Failed to lseek c_len in get_fileinfo\n");
		/* Chunk byte entry */
		if (control->major_version == 0 && control->minor_version > 4) {
			if (unlikely(read(fd_in, &chunk_byte, 1) != 1))
				fatal("Failed to read chunk_byte in get_fileinfo\n");
			ofs++;
		}
		if (ofs < infile_size - (HAS_MD5 ? MD5_DIGEST_SIZE : 0))
			goto next_chunk;
		if (unlikely(ofs > infile_size))
			failure("Offset greater than archive size, likely corrupted/truncated archive.\n");
		print_output("Rzip compression: %.1f%% %lld / %lld\n",
			     (double)utotal / (double)(expected_size / 100),
			     utotal, expected_size);
		print_output("Back end compression: %.1f%% %lld / %lld\n",
			     (double)ctotal / (double)(utotal / 100),
			     ctotal, utotal);
		print_output("Overall compression: %.1f%% %lld / %lld\n",
			     (double)ctotal / (double)(expected_size / 100),
			     ctotal, expected_size);
	}

	if (unlikely(close(fd_in)))
		fatal("Failed to close fd_in in get_fileinfo\n");

	free(control->outfile);
	free(infilecopy);
}

/*
  compress one file from the command line
*/
void compress_file(rzip_control *control)
{
	const char *tmp, *tmpinfile; 	/* we're just using this as a proxy for control->infile.
					 * Spares a compiler warning
					 */
	int fd_in, fd_out;
	char header[24];

	memset(header, 0, sizeof(header));

	if (!STDIN) {
		/* is extension at end of infile? */
		if ((tmp = strrchr(control->infile, '.')) && !strcmp(tmp, control->suffix)) {
			print_err("%s: already has %s suffix. Skipping...\n", control->infile, control->suffix);
			return;
		}

		fd_in = open(control->infile, O_RDONLY);
		if (unlikely(fd_in == -1))
			fatal("Failed to open %s\n", control->infile);
	} else
		fd_in = 0;

	if (!STDOUT) {
		if (control->outname) {
				/* check if outname has control->suffix */
				if (*(control->suffix) == '\0') /* suffix is empty string */
					control->outfile = strdup(control->outname);
				else if ((tmp=strrchr(control->outname, '.')) && strcmp(tmp, control->suffix)) {
					control->outfile = malloc(strlen(control->outname) + strlen(control->suffix) + 1);
					if (unlikely(!control->outfile))
						fatal("Failed to allocate outfile name\n");
					strcpy(control->outfile, control->outname);
					strcat(control->outfile, control->suffix);
					print_output("Suffix added to %s.\nFull pathname is: %s\n", control->outname, control->outfile);
				} else	/* no, already has suffix */
					control->outfile = strdup(control->outname);
		} else {
			/* default output name from control->infile
			 * test if outdir specified. If so, strip path from filename of
			 * control->infile
			*/
			if (control->outdir && (tmp = strrchr(control->infile, '/')))
				tmpinfile = tmp + 1;
			else
				tmpinfile = control->infile;

			control->outfile = malloc((control->outdir == NULL? 0: strlen(control->outdir)) + strlen(tmpinfile) + strlen(control->suffix) + 1);
			if (unlikely(!control->outfile))
				fatal("Failed to allocate outfile name\n");

			if (control->outdir) {	/* prepend control->outdir */
				strcpy(control->outfile, control->outdir);
				strcat(control->outfile, tmpinfile);
			} else
				strcpy(control->outfile, tmpinfile);
			strcat(control->outfile, control->suffix);
			print_progress("Output filename is: %s\n", control->outfile);
		}

		if (FORCE_REPLACE)
			fd_out = open(control->outfile, O_WRONLY | O_CREAT | O_TRUNC, 0666);
		else
			fd_out = open(control->outfile, O_WRONLY | O_CREAT | O_EXCL, 0666);
		if (unlikely(fd_out == -1)) {
			/* We must ensure we don't delete a file that already
			 * exists just because we tried to create a new one */
			control->flags |= FLAG_KEEP_BROKEN;
			fatal("Failed to create %s\n", control->outfile);
		}
	} else
		fd_out = open_tmpoutfile(control);
	control->fd_out = fd_out;

	if (unlikely(STDOUT && unlink(control->outfile)))
		fatal("Failed to unlink tmpfile: %s\n", control->outfile);

	preserve_perms(control, fd_in, fd_out);

	/* write zeroes to 24 bytes at beginning of file */
	if (unlikely(write(fd_out, header, sizeof(header)) != sizeof(header)))
		fatal("Cannot write file header\n");

	rzip_fd(control, fd_in, fd_out);

	/* write magic at end b/c lzma does not tell us properties until it is done */
	write_magic(control, fd_in, fd_out);

	if (STDOUT)
		dump_tmpoutfile(control, fd_out);

	if (unlikely(close(fd_in) || close(fd_out)))
		fatal("Failed to close files\n");

	if (!KEEP_FILES) {
		if (unlikely(unlink(control->infile)))
			fatal("Failed to unlink %s\n", control->infile);
	}

	free(control->outfile);
}
