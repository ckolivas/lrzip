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
#include <sys/mman.h>
#include <sys/time.h>
#include <termios.h>

#include "md5.h"
#include "rzip.h"
#include "runzip.h"
#include "util.h"
#include "stream.h"
#include "liblrzip.h" /* flag defines */

#define MAGIC_LEN (24)

static i64 fdout_seekto(rzip_control *control, i64 pos)
{
	if (TMP_OUTBUF) {
		pos -= control->out_relofs;
		control->out_ofs = pos;
		if (unlikely(pos > control->out_len || pos < 0)) {
			print_err("Trying to seek to %lld outside tmp outbuf in fdout_seekto\n", pos);
			return -1;
		}
		return 0;
	}
	return lseek(control->fd_out, pos, SEEK_SET);
}

void write_magic(rzip_control *control)
{
	char magic[MAGIC_LEN];

	memset(magic, 0, MAGIC_LEN);
	strcpy(magic, "LRZI");
	magic[4] = LRZIP_MAJOR_VERSION;
	magic[5] = LRZIP_MINOR_VERSION;

	/* File size is stored as zero for streaming STDOUT blocks when the
	 * file size is unknown. In encrypted files, the size is left unknown
	 * and instead the salt is stored here to preserve space. */
	if (ENCRYPT)
		memcpy(&magic[6], &control->salt, 8);
	else if (!STDIN || !STDOUT || control->eof)
		memcpy(&magic[6], &control->st_size, 8);

	/* save LZMA compression flags */
	if (LZMA_COMPRESS) {
		int i;

		for (i = 0; i < 5; i++)
			magic[i + 16] = (char)control->lzma_properties[i];
	}

	/* This is a flag that the archive contains an md5 sum at the end
	 * which can be used as an integrity check instead of crc check.
	 * crc is still stored for compatibility with 0.5 versions.
	 */
	magic[21] = 1;
	if (ENCRYPT)
		magic[22] = 1;

	if (unlikely(fdout_seekto(control, 0)))
		fatal("Failed to seek to BOF to write Magic Header\n");

	if (unlikely(put_fdout(control, magic, MAGIC_LEN) != MAGIC_LEN))
		fatal("Failed to write magic header\n");
	control->magic_written = 1;
}

static i64 enc_loops(uchar b1, uchar b2)
{
	return (i64)b2 << (i64)b1;
}

static void get_magic(rzip_control *control, char *magic)
{
	int encrypted, md5, i;
	i64 expected_size;
	uint32_t v;

	if (unlikely(strncmp(magic, "LRZI", 4)))
		failure("Not an lrzip file\n");

	memcpy(&control->major_version, &magic[4], 1);
	memcpy(&control->minor_version, &magic[5], 1);

	print_verbose("Detected lrzip version %d.%d file.\n", control->major_version, control->minor_version);
	if (control->major_version > LRZIP_MAJOR_VERSION ||
	    (control->major_version == LRZIP_MAJOR_VERSION && control->minor_version > LRZIP_MINOR_VERSION))
		print_output("Attempting to work with file produced by newer lrzip version %d.%d file.\n", control->major_version, control->minor_version);

	/* Support the convoluted way we described size in versions < 0.40 */
	if (control->major_version == 0 && control->minor_version < 4) {
		memcpy(&v, &magic[6], 4);
		expected_size = ntohl(v);
		memcpy(&v, &magic[10], 4);
		expected_size |= ((i64)ntohl(v)) << 32;
	} else
		memcpy(&expected_size, &magic[6], 8);
	control->st_size = expected_size;

	/* restore LZMA compression flags only if stored */
	if ((int) magic[16]) {
		for (i = 0; i < 5; i++)
			control->lzma_properties[i] = magic[i + 16];
	}

	/* Whether this archive contains md5 data at the end or not */
	md5 = magic[21];
	if (md5) {
		if (md5 == 1)
			control->flags |= FLAG_MD5;
		else
			print_verbose("Unknown hash, falling back to CRC\n");
	}
	encrypted = magic[22];
	if (encrypted) {
		if (encrypted == 1)
			control->flags |= FLAG_ENCRYPT;
		else
			failure("Unkown encryption\n");
		/* In encrypted files, the size field is used to store the salt
		 * instead and the size is unknown, just like a STDOUT chunked
		 * file */
		memcpy(&control->salt, &magic[6], 8);
		control->st_size = expected_size = 0;
		control->encloops = enc_loops(control->salt[0], control->salt[1]);
		print_maxverbose("Encryption hash loops %lld\n", control->encloops);
	} else if (ENCRYPT) {
		print_output("Asked to decrypt a non-encrypted archive. Bypassing decryption.\n");
		control->flags &= ~FLAG_ENCRYPT;
	}
}

void read_magic(rzip_control *control, int fd_in, i64 *expected_size)
{
	char magic[MAGIC_LEN];

	memset(magic, 0, sizeof(magic));
	/* Initially read only <v0.6x header */
	if (unlikely(read(fd_in, magic, 24) != 24))
		fatal("Failed to read magic header\n");

	get_magic(control, magic);
	*expected_size = control->st_size;
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

static void fwrite_stdout(void *buf, i64 len)
{
	uchar *offset_buf = buf;
	ssize_t ret;
	i64 total;

	total = 0;
	while (len > 0) {
		if (len > one_g)
			ret = one_g;
		else
			ret = len;
		ret = fwrite(offset_buf, 1, ret, stdout);
		if (unlikely(ret <= 0))
			fatal("Failed to fwrite in fwrite_stdout\n");
		len -= ret;
		offset_buf += ret;
		total += ret;
	}
	fflush(stdout);
}

void write_fdout(rzip_control *control, void *buf, i64 len)
{
	uchar *offset_buf = buf;
	ssize_t ret;

	while (len > 0) {
		ret = MIN(len, one_g);
		ret = write(control->fd_out, offset_buf, (size_t)ret);
		if (unlikely(ret <= 0))
			fatal("Failed to write to fd_out in write_fdout\n");
		len -= ret;
		offset_buf += ret;
	}
}

void flush_tmpoutbuf(rzip_control *control)
{
	if (!TEST_ONLY) {
		print_maxverbose("Dumping buffer to physical file.\n");
		if (STDOUT)
			fwrite_stdout(control->tmp_outbuf, control->out_len);
		else
			write_fdout(control, control->tmp_outbuf, control->out_len);
	}
	control->out_relofs += control->out_len;
	control->out_ofs = control->out_len = 0;
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

/* Used if we're unable to read STDIN into the temporary buffer, shunts data
 * to temporary file */
void write_fdin(struct rzip_control *control)
{
	uchar *offset_buf = control->tmp_inbuf;
	i64 len = control->in_len;
	ssize_t ret;

	while (len > 0) {
		ret = MIN(len, one_g);
		ret = write(control->fd_in, offset_buf, (size_t)ret);
		if (unlikely(ret <= 0))
			fatal("Failed to write to fd_in in write_fdin\n");
		len -= ret;
		offset_buf += ret;
	}
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

static void read_tmpinmagic(rzip_control *control)
{
	char magic[MAGIC_LEN];
	int i, tmpchar;

	memset(magic, 0, sizeof(magic));
	for (i = 0; i < 24; i++) {
		tmpchar = getchar();
		if (unlikely(tmpchar == EOF))
			failure("Reached end of file on STDIN prematurely on v05 magic read\n");
		magic[i] = (char)tmpchar;
	}
	get_magic(control, magic);
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

/* To perform STDOUT, we allocate a proportion of ram that is then used as
 * a pseudo-temporary file */
static void open_tmpoutbuf(rzip_control *control)
{
	control->flags |= FLAG_TMP_OUTBUF;
	control->out_maxlen = control->maxram;
	/* Allocate slightly more so we can cope when the buffer overflows and
	 * fall back to a real temporary file */
	control->tmp_outbuf = malloc(control->maxram + control->page_size);
	if (unlikely(!control->tmp_outbuf))
		fatal("Failed to malloc tmp_outbuf in open_tmpoutbuf\n");
	if (!DECOMPRESS && !TEST_ONLY)
		control->out_ofs = control->out_len = MAGIC_LEN;
}

void close_tmpoutbuf(rzip_control *control)
{
	control->flags &= ~FLAG_TMP_OUTBUF;
	free(control->tmp_outbuf);
}

static void open_tmpinbuf(rzip_control *control)
{
	control->flags |= FLAG_TMP_INBUF;
	control->in_maxlen = control->maxram;
	control->tmp_inbuf = malloc(control->maxram + control->page_size);
	if (unlikely(!control->tmp_inbuf))
		fatal("Failed to malloc tmp_inbuf in open_tmpinbuf\n");
}

void clear_tmpinbuf(rzip_control *control)
{
	control->in_len = control->in_ofs = 0;
}

void clear_tmpinfile(rzip_control *control)
{
	if (unlikely(lseek(control->fd_in, 0, SEEK_SET)))
		fatal("Failed to lseek on fd_in in clear_tmpinfile\n");
	if (unlikely(ftruncate(control->fd_in, 0)))
		fatal("Failed to truncate fd_in in clear_tmpinfile\n");
}

void close_tmpinbuf(rzip_control *control)
{
	control->flags &= ~FLAG_TMP_INBUF;
	free(control->tmp_inbuf);
}

static void get_pass(char *s)
{
	int len;

	memset(s, 0, PASS_LEN - SALT_LEN);
	if (unlikely(fgets(s, PASS_LEN - SALT_LEN, stdin) == NULL))
		failure("Failed to retrieve passphrase\n");
	len = strlen(s);
	if (len > 0 && ('\r' ==  s[len - 1] || '\n' == s[len - 1]))
		s[len - 1] = '\0';
	if (len > 1 && ('\r' ==  s[len - 2] || '\n' == s[len - 2]))
		s[len - 2] = '\0';
	len = strlen(s);
	if (unlikely(0 == len))
		failure("Empty passphrase\n");
}

static void get_hash(rzip_control *control, int make_hash)
{
	char *passphrase, *testphrase;
	struct termios termios_p;
	i64 i;
	int j;

	passphrase = calloc(PASS_LEN, 1);
	testphrase = calloc(PASS_LEN, 1);
	control->pass_hash = calloc(HASH_LEN, 1);
	control->hash = calloc(HASH_LEN, 1);
	if (unlikely(!passphrase || !testphrase || !control->pass_hash || !control->hash))
		fatal("Failed to calloc encrypt buffers in compress_file\n");
	mlock(passphrase, PASS_LEN);
	mlock(testphrase, PASS_LEN);
	mlock(control->pass_hash, HASH_LEN);
	mlock(control->hash, HASH_LEN);

	/* Disable stdin echo to screen */
	tcgetattr(fileno(stdin), &termios_p);
	termios_p.c_lflag &= ~ECHO;
	tcsetattr(fileno(stdin), 0, &termios_p);
retry_pass:
	print_output("Enter passphrase: ");
	get_pass(passphrase);
	print_output("\n");
	if (make_hash) {
		print_output("Re-enter passphrase: ");
		get_pass(testphrase);
		print_output("\n");
		if (strcmp(passphrase, testphrase)) {
			print_output("Passwords do not match. Try again.\n");
			goto retry_pass;
		}
	}
	termios_p.c_lflag |= ECHO;
	tcsetattr(fileno(stdin), 0, &termios_p);
	memset(testphrase, 0, PASS_LEN);
	munlock(testphrase, PASS_LEN);
	free(testphrase);

	memcpy(passphrase + PASS_LEN - SALT_LEN, control->salt, SALT_LEN);
	lrz_keygen(control, passphrase);
	memset(passphrase, 0, PASS_LEN);
	munlock(passphrase, PASS_LEN);
	free(passphrase);
}

static void release_hashes(rzip_control *control)
{
	memset(control->pass_hash, 0, HASH_LEN);
	memset(control->hash, 0, SALT_LEN);
	munlockall();
	free(control->pass_hash);
	free(control->hash);
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
			print_progress("Output filename is: %s\n", control->outfile);
	}

	if (STDIN) {
		fd_in = open_tmpinfile(control);
		read_tmpinmagic(control);
		if (ENCRYPT)
			failure("Cannot decompress encrypted file from STDIN\n");
		expected_size = control->st_size;
		open_tmpinbuf(control);
	} else {
		fd_in = open(infilecopy, O_RDONLY);
		if (unlikely(fd_in == -1)) {
			fatal("Failed to open %s\n", infilecopy);
		}
	}
	control->fd_in = fd_in;

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
		fd_hist = open(control->outfile, O_RDONLY);
		if (unlikely(fd_hist == -1))
			fatal("Failed to open history file %s\n", control->outfile);

		preserve_perms(control, fd_in, fd_out);
	} else {
		fd_out = open_tmpoutfile(control);
		if (unlikely(fd_out == -1))
			fatal("Failed to create %s\n", control->outfile);
		fd_hist = open(control->outfile, O_RDONLY);
		if (unlikely(fd_hist == -1))
			fatal("Failed to open history file %s\n", control->outfile);
		/* Unlink temporary file as soon as possible */
		if (unlikely(unlink(control->outfile)))
			fatal("Failed to unlink tmpfile: %s\n", control->outfile);
	}

	open_tmpoutbuf(control);

	if (!STDIN)
		read_magic(control, fd_in, &expected_size);

	if (!STDOUT) {
		/* Check if there's enough free space on the device chosen to fit the
		* decompressed file. */
		if (unlikely(fstatvfs(fd_out, &fbuf)))
			fatal("Failed to fstatvfs in decompress_file\n");
		free_space = (i64)fbuf.f_bsize * (i64)fbuf.f_bavail;
		if (free_space < expected_size) {
			if (FORCE_REPLACE)
				print_err("Warning, inadequate free space detected, but attempting to decompress due to -f option being used.\n");
			else
				failure("Inadequate free space to decompress file, use -f to override.\n");
		}
	}
	control->fd_out = fd_out;
	control->fd_hist = fd_hist;

	if (NO_MD5)
		print_verbose("Not performing MD5 hash check\n");
	if (HAS_MD5)
		print_verbose("MD5 ");
	else
		print_verbose("CRC32 ");
	print_verbose("being used for integrity testing.\n");

	if (ENCRYPT)
		get_hash(control, 0);

	print_progress("Decompressing...\n");

	runzip_fd(control, fd_in, fd_out, fd_hist, expected_size);

	if (STDOUT && !TMP_OUTBUF)
		dump_tmpoutfile(control, fd_out);

	/* if we get here, no fatal errors during decompression */
	print_progress("\r");
	if (!(STDOUT | TEST_ONLY))
		print_output("Output filename is: %s: ", control->outfile);
	if (!expected_size)
		expected_size = control->st_size;
	print_progress("[OK] - %lld bytes                                \n", expected_size);

	if (unlikely(close(fd_hist) || close(fd_out)))
		fatal("Failed to close files\n");

	close(fd_in);

	if (!KEEP_FILES) {
		if (unlikely(unlink(control->infile)))
			fatal("Failed to unlink %s\n", infilecopy);
	}

	if (ENCRYPT)
		release_hashes(control);

	free(control->outfile);
	free(infilecopy);
}

void get_header_info(rzip_control *control, int fd_in, uchar *ctype, i64 *c_len,
		     i64 *u_len, i64 *last_head, int chunk_bytes)
{
	if (unlikely(read(fd_in, ctype, 1) != 1))
		fatal("Failed to read in get_header_info\n");

	*c_len = *u_len = *last_head = 0;
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
		int read_len;

		if (control->major_version == 0 && control->minor_version == 5)
			read_len = 8;
		else
			read_len = chunk_bytes;
		if (unlikely(read(fd_in, c_len, read_len) != read_len))
			fatal("Failed to read in get_header_info");
		if (unlikely(read(fd_in, u_len, read_len) != read_len))
			fatal("Failed to read in get_header_info");
		if (unlikely(read(fd_in, last_head, read_len) != read_len))
			fatal("Failed to read_i64 in get_header_info");
	}
}

void get_fileinfo(rzip_control *control)
{
	i64 u_len, c_len, last_head, utotal = 0, ctotal = 0, ofs = 25, stream_head[2];
	i64 expected_size, infile_size, chunk_size = 0, chunk_total = 0;
	int header_length, stream = 0, chunk = 0;
	char *tmp, *infilecopy = NULL;
	int seekspot, fd_in;
	char chunk_byte = 0;
	long double cratio;
	uchar ctype = 0;
	struct stat st;

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
		if (control->major_version == 0 && control->minor_version > 5) {
			if (unlikely(read(fd_in, &control->eof, 1) != 1))
				fatal("Failed to read eof in get_fileinfo\n");
			if (unlikely(read(fd_in, &chunk_size, chunk_byte) != chunk_byte))
				fatal("Failed to read chunk_size in get_fileinfo\n");
		}
	}

	if (control->major_version == 0 && control->minor_version < 4) {
		ofs = 24;
		header_length = 13;
	} else if (control->major_version == 0 && control->minor_version == 4) {
		ofs = 24;
		header_length = 25;
	} else if (control->major_version == 0 && control->minor_version == 5) {
		ofs = 25;
		header_length = 25;
	} else {
		ofs = 26 + chunk_byte;
		header_length = 1 + (chunk_byte * 3);
	}
next_chunk:
	stream = 0;
	stream_head[0] = 0;
	stream_head[1] = stream_head[0] + header_length;

	print_verbose("Rzip chunk %d:\n", ++chunk);
	if (chunk_byte)
		print_verbose("Chunk byte width: %d\n", chunk_byte);
	if (chunk_size) {
		chunk_total += chunk_size;
		print_verbose("Chunk size: %lld\n", chunk_size);
	}
	while (stream < NUM_STREAMS) {
		int block = 1;

		if (unlikely(lseek(fd_in, stream_head[stream] + ofs, SEEK_SET)) == -1)
			fatal("Failed to seek to header data in get_fileinfo\n");
		get_header_info(control, fd_in, &ctype, &c_len, &u_len, &last_head, chunk_byte);

		print_verbose("Stream: %d\n", stream);
		print_maxverbose("Offset: %lld\n", ofs);
		print_verbose("Block\tComp\tPercent\tSize\n");
		do {
			i64 head_off;

			if (unlikely(last_head + ofs > infile_size))
				failure("Offset greater than archive size, likely corrupted/truncated archive.\n");
			if (unlikely(head_off = lseek(fd_in, last_head + ofs, SEEK_SET)) == -1)
				fatal("Failed to seek to header data in get_fileinfo\n");
			get_header_info(control, fd_in, &ctype, &c_len, &u_len,
					&last_head, chunk_byte);
			if (unlikely(last_head < 0 || c_len < 0 || u_len < 0))
				failure("Entry negative, likely corrupted archive.\n");
			print_verbose("%d\t", block);
			if (ctype == CTYPE_NONE)
				print_verbose("none");
			else if (ctype == CTYPE_BZIP2)
				print_verbose("bzip2");
			else if (ctype == CTYPE_LZO)
				print_verbose("lzo");
			else if (ctype == CTYPE_LZMA)
				print_verbose("lzma");
			else if (ctype == CTYPE_GZIP)
				print_verbose("gzip");
			else if (ctype == CTYPE_ZPAQ)
				print_verbose("zpaq");
			else
				print_verbose("Dunno wtf");
			utotal += u_len;
			ctotal += c_len;
			print_verbose("\t%.1f%%\t%lld / %lld", (double)c_len / (double)(u_len / 100), c_len, u_len);
			print_maxverbose("\tOffset: %lld\tHead: %lld", head_off, last_head);
			print_verbose("\n");
			block++;
		} while (last_head);
		++stream;
	}

	if (ENCRYPT) {
		if (unlikely((ofs = lseek(fd_in, c_len + 8, SEEK_CUR)) == -1))
			fatal("Failed to lseek c_len in get_fileinfo\n");
	} else {
		if (unlikely((ofs = lseek(fd_in, c_len, SEEK_CUR)) == -1))
		fatal("Failed to lseek c_len in get_fileinfo\n");
	}

	if (ofs >= infile_size - (HAS_MD5 ? MD5_DIGEST_SIZE : 0))
		goto done;
	/* Chunk byte entry */
	if (control->major_version == 0 && control->minor_version > 4) {
		if (unlikely(read(fd_in, &chunk_byte, 1) != 1))
			fatal("Failed to read chunk_byte in get_fileinfo\n");
		ofs++;
		if (control->major_version == 0 && control->minor_version > 5) {
			if (unlikely(read(fd_in, &control->eof, 1) != 1))
				fatal("Failed to read eof in get_fileinfo\n");
			if (unlikely(read(fd_in, &chunk_size, chunk_byte) != chunk_byte))
				fatal("Failed to read chunk_size in get_fileinfo\n");
			ofs += 1 + chunk_byte;
			header_length = 1 + (chunk_byte * 3);
		}
	}
	goto next_chunk;
done:
	if (unlikely(ofs > infile_size))
		failure("Offset greater than archive size, likely corrupted/truncated archive.\n");
	if (chunk_total > expected_size)
		expected_size = chunk_total;
	print_verbose("Rzip compression: %.1f%% %lld / %lld\n",
			(double)utotal / (double)(expected_size / 100),
			utotal, expected_size);
	print_verbose("Back end compression: %.1f%% %lld / %lld\n",
			(double)ctotal / (double)(utotal / 100),
			ctotal, utotal);
	print_verbose("Overall compression: %.1f%% %lld / %lld\n",
			(double)ctotal / (double)(expected_size / 100),
			ctotal, expected_size);

	cratio = (long double)expected_size / (long double)infile_size;

	print_output("%s:\nlrzip version: %d.%d file\n", infilecopy, control->major_version, control->minor_version);

	if (ENCRYPT)
		print_output("Encrypted\n");
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
		if (ENCRYPT)
			print_output("Unknown, encrypted\n");
		else {
			for (i = 0; i < MD5_DIGEST_SIZE; i++)
				print_output("%02x", md5_stored[i] & 0xFF);
		}
		print_output("\n");
	} else
		print_output("CRC32 used for integrity testing\n");
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
	int fd_in, fd_out = -1;
	char header[MAGIC_LEN];

	if (ENCRYPT)
		get_hash(control, 1);
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
			fd_out = open(control->outfile, O_RDWR | O_CREAT | O_TRUNC, 0666);
		else
			fd_out = open(control->outfile, O_RDWR | O_CREAT | O_EXCL, 0666);
		if (unlikely(fd_out == -1)) {
			/* We must ensure we don't delete a file that already
			 * exists just because we tried to create a new one */
			control->flags |= FLAG_KEEP_BROKEN;
			fatal("Failed to create %s\n", control->outfile);
		}
		control->fd_out = fd_out;
		preserve_perms(control, fd_in, fd_out);
	} else 
		open_tmpoutbuf(control);

	/* Write zeroes to header at beginning of file */
	if (unlikely(!STDOUT && write(fd_out, header, sizeof(header)) != sizeof(header)))
		fatal("Cannot write file header\n");

	rzip_fd(control, fd_in, fd_out);

	/* Wwrite magic at end b/c lzma does not tell us properties until it is done */
	if (!STDOUT)
		write_magic(control);

	if (ENCRYPT)
		release_hashes(control);

	if (unlikely(close(fd_in)))
		fatal("Failed to close fd_in\n");
	if (unlikely(!STDOUT && close(fd_out)))
		fatal("Failed to close fd_out\n");
	if (TMP_OUTBUF)
		close_tmpoutbuf(control);

	if (!KEEP_FILES) {
		if (unlikely(unlink(control->infile)))
			fatal("Failed to unlink %s\n", control->infile);
	}

	free(control->outfile);
}
