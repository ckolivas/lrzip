/*
   Copyright (C) Andrew Tridgell 1998-2003,
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
/* lrzip compression - main program */
#include "rzip.h"

struct rzip_control control;

static void usage(void)
{
	fprintf(stdout, "lrzip version %d.%d%d\n", LRZIP_MAJOR_VERSION, LRZIP_MINOR_VERSION, LRZIP_MINOR_SUBVERSION);
	fprintf(stdout, "Copyright (C) Con Kolivas 2006-2010\n\n");
	fprintf(stdout, "Based on rzip ");
	fprintf(stdout, "Copyright (C) Andrew Tridgell 1998-2003\n");
	fprintf(stdout, "usage: lrzip [options] <file...>\n");
	fprintf(stdout, " Options:\n");
	fprintf(stdout, "     -w size       compression window in hundreds of MB\n");
	fprintf(stdout, "                   default chosen by heuristic dependent on ram and chosen compression\n");
	fprintf(stdout, "     -d            decompress\n");
	fprintf(stdout, "     -o filename   specify the output file name and/or path\n");
	fprintf(stdout, "     -O directory  specify the output directory when -o is not used\n");
	fprintf(stdout, "     -S suffix     specify compressed suffix (default '.lrz')\n");
	fprintf(stdout, "     -f            force overwrite of any existing files\n");
	fprintf(stdout, "     -D            delete existing files\n");
	fprintf(stdout, "     -P            don't set permissions on output file - may leave it world-readable\n");
	fprintf(stdout, "     -q            don't show compression progress\n");
	fprintf(stdout, "     -L level      set lzma/bzip2/gzip compression level (1-9, default 7)\n");
	fprintf(stdout, "     -n            no backend compression - prepare for other compressor\n");
	fprintf(stdout, "     -l            lzo compression (ultra fast)\n");
	fprintf(stdout, "     -b            bzip2 compression\n");
	fprintf(stdout, "     -g            gzip compression using zlib\n");
	fprintf(stdout, "     -z            zpaq compression (best, extreme compression, extremely slow)\n");
	fprintf(stdout, "     -M            Maximum window and level - (all available ram and level 9)\n");
	fprintf(stdout, "     -T value      Compression threshold with LZO test. (0 (nil) - 10 (high), default 1)\n");
	fprintf(stdout, "     -N value      Set nice value to value (default 19)\n");
	fprintf(stdout, "     -v[v]         Increase verbosity\n");
	fprintf(stdout, "     -V            show version\n");
	fprintf(stdout, "     -t            test compressed file integrity\n");
	fprintf(stdout, "     -i            show compressed file information\n");
	fprintf(stdout, "\nIf no filenames or \"-\" is specified, stdin/out will be used.\n");
}

static void write_magic(int fd_in, int fd_out)
{
	struct stat st;
	char magic[24];
	int i;

	memset(magic, 0, sizeof(magic));
	strcpy(magic, "LRZI");
	magic[4] = LRZIP_MAJOR_VERSION;
	magic[5] = LRZIP_MINOR_VERSION;

	if (fstat(fd_in, &st) != 0)
		fatal("bad magic file descriptor!?\n");

	memcpy(&magic[6], &st.st_size, 8);

	/* save LZMA compression flags */
	if (LZMA_COMPRESS(control.flags)) {
		for (i = 0; i < 5; i++)
			magic[i + 16] = (char)control.lzma_properties[i];
	}

	if (lseek(fd_out, 0, SEEK_SET) != 0)
		fatal("Failed to seek to BOF to write Magic Header\n");

	if (write(fd_out, magic, sizeof(magic)) != sizeof(magic))
		fatal("Failed to write magic header\n");
}

static void read_magic(int fd_in, i64 *expected_size)
{
	char magic[24];
	uint32_t v;
	int i;

	if (read(fd_in, magic, sizeof(magic)) != sizeof(magic))
		fatal("Failed to read magic header\n");

	*expected_size = 0;

	if (strncmp(magic, "LRZI", 4) != 0) {
		fatal("Not an lrzip file\n");
	}

	memcpy(&control.major_version, &magic[4], 1);
	memcpy(&control.minor_version, &magic[5], 1);

	/* Support the convoluted way we described size in versions < 0.40 */
	if (control.major_version == 0 && control.minor_version < 4) {
		memcpy(&v, &magic[6], 4);
		*expected_size = ntohl(v);
		memcpy(&v, &magic[10], 4);
		*expected_size |= ((i64)ntohl(v)) << 32;
	} else
		memcpy(expected_size, &magic[6], 8);

	/* restore LZMA compression flags only if stored */
	if ((int) magic[16]) {
		for (i = 0; i < 5; i++)
			control.lzma_properties[i] = magic[i + 16];
	}
	if (control.flags & FLAG_VERBOSE) {
		fprintf(control.msgout, "Detected lrzip version %d.%d file.\n", control.major_version, control.minor_version);
		fflush(control.msgout);
	}
	if (control.major_version > LRZIP_MAJOR_VERSION ||
	    (control.major_version == LRZIP_MAJOR_VERSION && control.minor_version > LRZIP_MINOR_VERSION)) {
		fprintf(control.msgout, "Attempting to work with file produced by newer lrzip version %d.%d file.\n", control.major_version, control.minor_version);
		fflush(control.msgout);
	}
}

/* preserve ownership and permissions where possible */
static void preserve_perms(int fd_in, int fd_out)
{
	struct stat st;

	if (fstat(fd_in, &st) != 0)
		fatal("Failed to fstat input file\n");
	if (fchmod(fd_out, (st.st_mode & 0777)) != 0)
		fatal("Failed to set permissions on %s\n", control.outfile);

	/* chown fail is not fatal */
	fchown(fd_out, st.st_uid, st.st_gid);
}

/* Open a temporary outputfile for testing decompression or msgout */
static int open_tmpoutfile(void)
{
	int fd_out;

	if ((control.flags & FLAG_STDOUT) && (control.flags & FLAG_VERBOSE))
		fprintf(control.msgout, "Outputting to stdout.\n");
	control.outfile = realloc(NULL, 16);
	strcpy(control.outfile, "lrzipout.XXXXXX");
	if (!control.outfile)
		fatal("Failed to allocate outfile name\n");

	fd_out = mkstemp(control.outfile);
	if (fd_out == -1)
		fatal("Failed to create out tmpfile: %s\n", strerror(errno));
	return fd_out;
}

/* Dump temporary outputfile to perform stdout */
static void dump_tmpoutfile(int fd_out)
{
	FILE *tmpoutfp;
	int tmpchar;

	if (control.flags & FLAG_SHOW_PROGRESS)
		fprintf(control.msgout, "Dumping to stdout.\n");
	/* flush anything not yet in the temporary file */
	fflush(NULL);
	tmpoutfp = fdopen(fd_out, "r");
	if (tmpoutfp == NULL)
		fatal("Failed to fdopen out tmpfile: %s\n", strerror(errno));
	rewind(tmpoutfp);

	while ((tmpchar = fgetc(tmpoutfp)) != EOF)
		putchar(tmpchar);

	fflush(control.msgout);
}

/* Open a temporary inputfile to perform stdin */
static int open_tmpinfile(void)
{
	int fd_in;

	control.infile = malloc(15);
	strcpy(control.infile, "lrzipin.XXXXXX");
	if (!control.infile)
		fatal("Failed to allocate infile name\n");

	fd_in = mkstemp(control.infile);
	if (fd_in == -1)
		fatal("Failed to create in tmpfile: %s\n", strerror(errno));
	return fd_in;
}

/* Read data from stdin into temporary inputfile */
static void read_tmpinfile(int fd_in)
{
	FILE *tmpinfp;
	int tmpchar;

	if (control.flags & FLAG_SHOW_PROGRESS)
		fprintf(control.msgout, "Copying from stdin.\n");
	tmpinfp = fdopen(fd_in, "w+");
	if (tmpinfp == NULL)
		fatal("Failed to fdopen in tmpfile: %s\n", strerror(errno));

	while ((tmpchar = getchar()) != EOF)
		fputc(tmpchar, tmpinfp);

	fflush(tmpinfp);
	rewind(tmpinfp);
}

/*
  decompress one file from the command line
*/
static void decompress_file(void)
{
	char *tmp, *tmpoutfile, *infilecopy = NULL;
	int fd_in, fd_out = -1, fd_hist = -1;
	i64 expected_size;

	if (!(control.flags & FLAG_STDIN)) {
		if ((tmp = strrchr(control.infile, '.')) && strcmp(tmp,control.suffix)) {
			/* make sure infile has an extension. If not, add it
			  * because manipulations may be made to input filename, set local ptr
			*/
			infilecopy = malloc(strlen(control.infile) + strlen(control.suffix) + 1);
			if (infilecopy == NULL)
				fatal("Failed to allocate memory for infile suffix\n");
			else {
				strcpy(infilecopy, control.infile);
				strcat(infilecopy, control.suffix);
			}
		} else
			infilecopy = strdup(control.infile);
		/* regardless, infilecopy has the input filename */
	}

	if (!(control.flags & FLAG_STDOUT) && !(control.flags & FLAG_TEST_ONLY)) {
		/* if output name already set, use it */
		if (control.outname) {
			control.outfile = strdup(control.outname);
		} else {
			/* default output name from infilecopy
			 * test if outdir specified. If so, strip path from filename of
			 * infilecopy, then remove suffix.
			*/
			if (control.outdir && (tmp = strrchr(infilecopy, '/')))
				tmpoutfile = strdup(tmp + 1);
			else
				tmpoutfile = strdup(infilecopy);

			/* remove suffix to make outfile name */
			if ((tmp = strrchr(tmpoutfile, '.')) && !strcmp(tmp, control.suffix))
				*tmp='\0';

			control.outfile = malloc((control.outdir == NULL? 0: strlen(control.outdir)) + strlen(tmpoutfile) + 1);
			if (!control.outfile)
				fatal("Failed to allocate outfile name\n");

			if (control.outdir) {	/* prepend control.outdir */
				strcpy(control.outfile, control.outdir);
				strcat(control.outfile, tmpoutfile);
			} else
				strcpy(control.outfile, tmpoutfile);
			free(tmpoutfile);
		}

		if ((control.flags & FLAG_SHOW_PROGRESS) && !(control.flags & FLAG_STDOUT)) {
			fprintf(control.msgout, "Output filename is: %s...Decompressing...\n", control.outfile);
			fflush(control.msgout);
		}
	}

	if (control.flags & FLAG_STDIN) {
		fd_in = open_tmpinfile();
		read_tmpinfile(fd_in);
	} else {
		fd_in = open(infilecopy, O_RDONLY);
		if (fd_in == -1) {
			fatal("Failed to open %s: %s\n",
			      infilecopy,
			      strerror(errno));
		}
	}

	if ((control.flags & (FLAG_TEST_ONLY | FLAG_STDOUT)) == 0) {
		if (control.flags & FLAG_FORCE_REPLACE)
			fd_out = open(control.outfile, O_WRONLY | O_CREAT | O_TRUNC, 0666);
		else
			fd_out = open(control.outfile, O_WRONLY | O_CREAT | O_EXCL, 0666);
		if (fd_out == -1)
			fatal("Failed to create %s: %s\n", control.outfile, strerror(errno));

		if (!(control.flags & FLAG_NO_SET_PERMS))
			preserve_perms(fd_in, fd_out);
	} else
		fd_out = open_tmpoutfile();

	fd_hist = open(control.outfile, O_RDONLY);
	if (fd_hist == -1)
		fatal("Failed to open history file %s\n", control.outfile);

	read_magic(fd_in, &expected_size);
	if (control.flags & FLAG_SHOW_PROGRESS) {
		fprintf(control.msgout, "Decompressing...");
		fflush(control.msgout);
	}

	runzip_fd(fd_in, fd_out, fd_hist, expected_size);

	if (control.flags & FLAG_STDOUT)
		dump_tmpoutfile(fd_out);

	/* if we get here, no fatal errors during decompression */
	fprintf(control.msgout, "\r");
	if (!(control.flags & (FLAG_STDOUT | FLAG_TEST_ONLY)))
		fprintf(control.msgout, "Output filename is: %s: ", control.outfile);
        if (control.flags & FLAG_SHOW_PROGRESS)
                fprintf(control.msgout, "[OK] - %lld bytes                                 \n", (long long)expected_size);
	fflush(control.msgout);

	if (close(fd_hist) != 0 || close(fd_out) != 0)
		fatal("Failed to close files\n");

	if (control.flags & (FLAG_TEST_ONLY | FLAG_STDOUT)) {
		/* Delete temporary files generated for testing or faking stdout */
		if (unlink(control.outfile) != 0)
			fatal("Failed to unlink tmpfile: %s\n", strerror(errno));
	}

	close(fd_in);

	if (((control.flags & (FLAG_KEEP_FILES | FLAG_TEST_ONLY)) == 0) || (control.flags & FLAG_STDIN)) {
		if (unlink(control.infile) != 0)
			fatal("Failed to unlink %s: %s\n", infilecopy, strerror(errno));
	}

	free(control.outfile);
	free(infilecopy);
}

static void get_fileinfo(void)
{
	int fd_in, ctype = 0;
	long double cratio;
	i64 expected_size;
	i64 infile_size;
	struct stat st;
	int seekspot;

	char *tmp, *infilecopy = NULL;

	if (!(control.flags & FLAG_STDIN)) {
		if ((tmp = strrchr(control.infile, '.')) && strcmp(tmp,control.suffix)) {
			infilecopy = malloc(strlen(control.infile) + strlen(control.suffix) + 1);
			if (infilecopy == NULL)
				fatal("Failed to allocate memory for infile suffix\n");
			else {
				strcpy(infilecopy, control.infile);
				strcat(infilecopy, control.suffix);
			}
		} else
			infilecopy = strdup(control.infile);
	}

	if (control.flags & FLAG_STDIN) {
		fd_in = open_tmpinfile();
		read_tmpinfile(fd_in);
	} else {
		fd_in = open(infilecopy, O_RDONLY);
		if (fd_in == -1)
			fatal("Failed to open %s: %s\n", infilecopy, strerror(errno));
	}

	/* Get file size */
	if (fstat(fd_in, &st) != 0)
		fatal("bad magic file descriptor!?\n");
	memcpy(&infile_size, &st.st_size, 8);

	/* Get decompressed size */
	read_magic(fd_in, &expected_size);

	/* Version < 0.4 had different file format */
	if (control.major_version == 0 && control.minor_version < 4)
		seekspot = 50;
	else
		seekspot = 74;
	if (lseek(fd_in, seekspot, SEEK_SET) == -1)
		fatal("Failed to lseek in get_fileinfo: %s\n", strerror(errno));

	/* Read the compression type of the first block. It's possible that
	   not all blocks are compressed so this may not be accurate.
	 */
	read(fd_in, &ctype, 1);

	cratio = (long double)expected_size / (long double)infile_size;
	
	fprintf(control.msgout, "%s:\nlrzip version: %d.%d file\n", infilecopy, control.major_version, control.minor_version);
	fprintf(control.msgout, "Compression: ");
	if (ctype == CTYPE_NONE)
		fprintf(control.msgout, "rzip alone\n");
	else if (ctype == CTYPE_BZIP2)
		fprintf(control.msgout, "rzip + bzip2\n");
	else if (ctype == CTYPE_LZO)
		fprintf(control.msgout, "rzip + lzo\n");
	else if (ctype == CTYPE_LZMA)
		fprintf(control.msgout, "rzip + lzma\n");
	else if (ctype == CTYPE_GZIP)
		fprintf(control.msgout, "rzip + gzip\n");
	else if (ctype == CTYPE_ZPAQ)
		fprintf(control.msgout, "rzip + zpaq\n");
	fprintf(control.msgout, "Decompressed file size: %llu\n", expected_size);
	fprintf(control.msgout, "Compressed file size: %llu\n", infile_size);
	fprintf(control.msgout, "Compression ratio: %.3Lf\n", cratio);

	if (control.flags & FLAG_STDIN) {
		if (unlink(control.infile) != 0)
			fatal("Failed to unlink %s: %s\n", infilecopy, strerror(errno));
	}

	free(control.outfile);
	free(infilecopy);
}

/*
  compress one file from the command line
*/
static void compress_file(void)
{
	const char *tmp, *tmpinfile; 	/* we're just using this as a proxy for control.infile.
					 * Spares a compiler warning
					 */
	int fd_in, fd_out;
	char header[24];

	memset(header, 0, sizeof(header));

	if (!(control.flags & FLAG_STDIN)) {
		/* is extension at end of infile? */
		if ((tmp = strrchr(control.infile, '.')) && !strcmp(tmp, control.suffix)) {
			fprintf(control.msgout, "%s: already has %s suffix. Skipping...\n", control.infile, control.suffix);
			return;
		}

		fd_in = open(control.infile, O_RDONLY);
		if (fd_in == -1)
			fatal("Failed to open %s: %s\n", control.infile, strerror(errno));
	} else {
		fd_in = open_tmpinfile();
		read_tmpinfile(fd_in);
	}

	if (!(control.flags & FLAG_STDOUT)) {
		if (control.outname) {
				/* check if outname has control.suffix */
				if (*(control.suffix) == '\0') /* suffix is empty string */
					control.outfile = strdup(control.outname);
				else if ((tmp=strrchr(control.outname, '.')) && strcmp(tmp, control.suffix)) {
					control.outfile = malloc(strlen(control.outname) + strlen(control.suffix) + 1);
					if (!control.outfile)
						fatal("Failed to allocate outfile name\n");
					strcpy(control.outfile, control.outname);
					strcat(control.outfile, control.suffix);
					fprintf(control.msgout, "Suffix added to %s.\nFull pathname is: %s\n", control.outname, control.outfile);
				} else	/* no, already has suffix */
					control.outfile = strdup(control.outname);
		} else {
			/* default output name from control.infile
			 * test if outdir specified. If so, strip path from filename of
			 * control.infile
			*/
			if (control.outdir && (tmp = strrchr(control.infile, '/')))
				tmpinfile = tmp + 1;
			else
				tmpinfile = control.infile;

			control.outfile = malloc((control.outdir == NULL? 0: strlen(control.outdir)) + strlen(tmpinfile) + strlen(control.suffix) + 1);
			if (!control.outfile)
				fatal("Failed to allocate outfile name\n");

			if (control.outdir) {	/* prepend control.outdir */
				strcpy(control.outfile, control.outdir);
				strcat(control.outfile, tmpinfile);
			} else
				strcpy(control.outfile, tmpinfile);
			strcat(control.outfile, control.suffix);
			if ( control.flags & FLAG_SHOW_PROGRESS )
				fprintf(control.msgout, "Output filename is: %s\n", control.outfile);
		}

		if (control.flags & FLAG_FORCE_REPLACE)
			fd_out = open(control.outfile, O_WRONLY | O_CREAT | O_TRUNC, 0666);
		else
			fd_out = open(control.outfile, O_WRONLY | O_CREAT | O_EXCL, 0666);
		if (fd_out == -1)
			fatal("Failed to create %s: %s\n", control.outfile, strerror(errno));
	} else
		fd_out = open_tmpoutfile();

        if (!(control.flags & FLAG_NO_SET_PERMS))
		preserve_perms(fd_in, fd_out);

	/* write zeroes to 24 bytes at beginning of file */
	if (write(fd_out, header, sizeof(header)) != sizeof(header))
		fatal("Cannot write file header\n");

	rzip_fd(fd_in, fd_out);

	/* write magic at end b/c lzma does not tell us properties until it is done */
	write_magic(fd_in, fd_out);

	if (control.flags & FLAG_STDOUT)
		dump_tmpoutfile(fd_out);

	if (close(fd_in) != 0 || close(fd_out) != 0)
		fatal("Failed to close files\n");

	if (control.flags & FLAG_STDOUT) {
		/* Delete temporary files generated for testing or faking stdout */
		if (unlink(control.outfile) != 0)
			fatal("Failed to unlink tmpfile: %s\n", strerror(errno));
	}

	if ((control.flags & FLAG_KEEP_FILES) == 0 || (control.flags & FLAG_STDIN)) {
		if (unlink(control.infile) != 0)
			fatal("Failed to unlink %s: %s\n", control.infile, strerror(errno));
	}

	free(control.outfile);
}

/*
 * Returns ram size on linux/darwin.
 */
#ifdef __APPLE__
static i64 get_ram(void)
{
	int mib[2];
	size_t len;
	i64 *p, ramsize;

	mib[0] = CTL_HW;
	mib[1] = HW_MEMSIZE;
	sysctl(mib, 2, NULL, &len, NULL, 0);
	p = malloc(len);
	sysctl(mib, 2, p, &len, NULL, 0);
	ramsize = *p / 1024; // bytes -> KB

	/* Darwin can't overcommit as much as linux so we return half the ram
	   size to fudge it to use smaller windows */
	ramsize /= 2;
	return ramsize;
}
#else
static i64 get_ram(void)
{
	return (i64)sysconf(_SC_PHYS_PAGES) * (i64)sysconf(_SC_PAGE_SIZE) / 1024;
}
#endif

int main(int argc, char *argv[])
{
	struct timeval start_time, end_time;
	struct sigaction handler;
	double seconds,total_time; // for timers
	int c, i, maxwin = 0;
	int hours,minutes;
	extern int optind;
	char *eptr; /* for environment */

	memset(&control, 0, sizeof(control));

	control.msgout = stderr;
	control.flags = FLAG_SHOW_PROGRESS | FLAG_KEEP_FILES;
	control.suffix = ".lrz";
	control.outdir = NULL;

	if (strstr(argv[0], "lrunzip")) {
		control.flags |= FLAG_DECOMPRESS;
	}

	control.compression_level = 7;
	control.ramsize = get_ram() / 104858ull; /* hundreds of megabytes */
	control.window = 0;
	control.threshold = 1.0;	/* default lzo test compression threshold (level 1) with LZMA compression */
	/* for testing single CPU */
#ifndef NOTHREAD
	control.threads = sysconf(_SC_NPROCESSORS_ONLN);	/* get CPUs for LZMA */
#else
	control.threads = 1;
#endif

	control.nice_val = 19;

	/* generate crc table */
	CrcGenerateTable();

	/* Get Preloaded Defaults from lrzip.conf
	 * Look in ., $HOME/.lrzip/, /etc/lrzip.
	 * If LRZIP=NOCONFIG is set, then ignore config
	 */

	eptr = getenv("LRZIP");
	if (eptr == NULL)
		read_config(&control);
	else if (!strstr(eptr,"NOCONFIG"))
		read_config(&control);

	while ((c = getopt(argc, argv, "L:hdS:tVvDfqo:w:nlbMO:T:N:gPzi")) != -1) {
		switch (c) {
		case 'L':
			control.compression_level = atoi(optarg);
			if (control.compression_level < 1 || control.compression_level > 9)
				fatal("Invalid compression level (must be 1-9)\n");
			break;
		case 'w':
			control.window = atol(optarg);
			break;
		case 'd':
			control.flags |= FLAG_DECOMPRESS;
			break;
		case 'S':
			control.suffix = optarg;
			break;
		case 'o':
			if (control.outdir)
				fatal("Cannot have -o and -O together\n");
			control.outname = optarg;
			break;
		case 'f':
			control.flags |= FLAG_FORCE_REPLACE;
			break;
		case 'D':
			control.flags &= ~FLAG_KEEP_FILES;
			break;
		case 't':
			if (control.outname)
				fatal("Cannot specify an output file name when just testing.\n");
			if (!(control.flags & FLAG_KEEP_FILES))
				fatal("Doubt that you want to delete a file when just testing.\n");
			control.flags |= FLAG_TEST_ONLY;
			break;
		case 'v':
			/* set verbosity flag */
			if (!(control.flags & FLAG_VERBOSITY) && !(control.flags & FLAG_VERBOSITY_MAX))
				control.flags |= FLAG_VERBOSITY;
			else if ((control.flags & FLAG_VERBOSITY)) {
				control.flags &= ~FLAG_VERBOSITY;
				control.flags |= FLAG_VERBOSITY_MAX;
			}
			break;
		case 'q':
			control.flags &= ~FLAG_SHOW_PROGRESS;
			break;
		case 'V':
			fprintf(stdout, "lrzip version %d.%d%d\n",
				LRZIP_MAJOR_VERSION, LRZIP_MINOR_VERSION, LRZIP_MINOR_SUBVERSION);
			exit(0);
			break;
		case 'l':
			if (control.flags & FLAG_NOT_LZMA)
				fatal("Can only use one of -l, -b, -g, -z or -n\n");
			control.flags |= FLAG_LZO_COMPRESS;
			break;
		case 'b':
			if (control.flags & FLAG_NOT_LZMA)
				fatal("Can only use one of -l, -b, -g, -z or -n\n");
			control.flags |= FLAG_BZIP2_COMPRESS;
			break;
		case 'n':
			if (control.flags & FLAG_NOT_LZMA)
				fatal("Can only use one of -l, -b, -g, -z or -n\n");
			control.flags |= FLAG_NO_COMPRESS;
			break;
		case 'M':
			control.compression_level = 9;
			maxwin = 1;
			break;
		case 'O':
			if (control.outname)	/* can't mix -o and -O */
				fatal("Cannot have options -o and -O together\n");
			control.outdir = malloc(strlen(optarg) + 2);
			if (control.outdir == NULL)
				fatal("Failed to allocate for outdir\n");
			strcpy(control.outdir,optarg);
			if (strcmp(optarg+strlen(optarg) - 1, "/")) 	/* need a trailing slash */
				strcat(control.outdir, "/");
			break;
		case 'T':
			/* invert argument, a threshold of 1 means that the compressed result can be
			 * 90%-100% of the sample size
			*/
			control.threshold = atoi(optarg);
			if (control.threshold < 0 || control.threshold > 10)
				fatal("Threshold value must be between 0 and 10\n");
			control.threshold = 1.05 - control.threshold / 20;
			break;
		case 'N':
			control.nice_val = atoi(optarg);
			if (control.nice_val < -20 || control.nice_val > 19)
				fatal("Invalid nice value (must be -20..19)\n");
			break;
		case 'g':
			if (control.flags & FLAG_NOT_LZMA)
				fatal("Can only use one of -l, -b, -g, -z or -n\n");
			control.flags |= FLAG_ZLIB_COMPRESS;
			break;
		case 'P':
			control.flags |= FLAG_NO_SET_PERMS;
			break;
		case 'z':
			if (control.flags & FLAG_NOT_LZMA)
				fatal("Can only use one of -l, -b, -g, -z or -n\n");
			control.flags |= FLAG_ZPAQ_COMPRESS;
			break;
		case 'i':
			control.flags |= FLAG_INFO;
			break;
		case 'h':
			usage();
			return -1;
		}
	}

	argc -= optind;
	argv += optind;

	if (control.outname && argc > 1)
		fatal("Cannot specify output filename with more than 1 file\n");

	if ((control.flags & FLAG_VERBOSE) && !(control.flags & FLAG_SHOW_PROGRESS)) {
		fprintf(stderr, "Cannot have -v and -q options. -v wins.\n");
		control.flags |= FLAG_SHOW_PROGRESS;
	}

	if (argc < 1)
		control.flags |= FLAG_STDIN;

	if (control.window > control.ramsize)
		fprintf(stderr, "Compression window has been set to larger than ramsize, proceeding at your request. If you did not mean this, abort now.\n");

	if (sizeof(long) == 4 && control.ramsize > 9) {
		/* On 32 bit, the default high/lowmem split of 896MB lowmem
		   means we will be unable to allocate contiguous blocks
		   over 900MB between 900 and 1800MB. It will be less prone
		   to failure if we limit the block size.
		   */
		if (control.ramsize < 18)
			control.ramsize = 9;
		else
			control.ramsize -= 9;
	}

	/* The control window chosen is the largest that will not cause
	   massive swapping on the machine (60% of ram). Most of the pages
	   will be shared by lzma even though it uses just as much ram itself
	   */
	if (!control.window) {
		if (maxwin)
			control.window = (control.ramsize * 9 / 10);
		else
			control.window = (control.ramsize * 2 / 3);
		if (!control.window)
			control.window = 1;
	}

	/* OK, if verbosity set, print summary of options selected */
	if ((control.flags & FLAG_VERBOSE) && !(control.flags & FLAG_INFO)) {
		fprintf(stderr, "The following options are in effect for this %s.\n",
			control.flags & FLAG_DECOMPRESS ? "DECOMPRESSION" : "COMPRESSION");
		if (LZMA_COMPRESS(control.flags))
			fprintf(stderr, "Threading is %s. Number of CPUs detected: %lu\n", control.threads > 1? "ENABLED" : "DISABLED",
				control.threads);
		fprintf(stderr, "Nice Value: %d\n", control.nice_val);
		if (control.flags & FLAG_SHOW_PROGRESS)
			fprintf(stderr, "Show Progress\n");
		if (control.flags & FLAG_VERBOSITY)
			fprintf(stderr, "Verbose\n");
		else if (control.flags & FLAG_VERBOSITY_MAX)
			fprintf(stderr, "Max Verbosity\n");
		if (control.flags & FLAG_FORCE_REPLACE)
			fprintf(stderr, "Overwrite Files\n");
		if (!(control.flags & FLAG_KEEP_FILES))
			fprintf(stderr, "Remove input files on completion\n");
		if (control.outdir)
			fprintf(stderr, "Output Directory Specified: %s\n", control.outdir);
		else if (control.outname)
			fprintf(stderr, "Output Filename Specified: %s\n", control.outname);
		if (control.flags & FLAG_TEST_ONLY)
			fprintf(stderr, "Test file integrity\n");
		
		/* show compression options */
		if (!(control.flags & FLAG_DECOMPRESS)) {
			fprintf(stderr, "Compression mode is: ");
			if (LZMA_COMPRESS(control.flags))
				fprintf(stderr, "LZMA. LZO Test Compression Threshold: %.f\n",
				       (control.threshold < 1.05 ? 21 - control.threshold * 20 : 0));
			else if (control.flags & FLAG_LZO_COMPRESS)
				fprintf(stderr, "LZO\n");
			else if (control.flags & FLAG_BZIP2_COMPRESS)
				fprintf(stderr, "BZIP2. LZO Test Compression Threshold: %.f\n",
				       (control.threshold < 1.05 ? 21 - control.threshold * 20 : 0));
			else if (control.flags & FLAG_ZLIB_COMPRESS)
				fprintf(stderr, "GZIP\n");
			else if (control.flags & FLAG_ZPAQ_COMPRESS)
				fprintf(stderr, "ZPAQ. LZO Test Compression Threshold: %.f\n",
				       (control.threshold < 1.05 ? 21 - control.threshold * 20 : 0));
			else if (control.flags & FLAG_NO_COMPRESS)
				fprintf(stderr, "RZIP\n");
			fprintf(stderr, "Compression Window: %lld = %lldMB\n", control.window, control.window * 100ull);
			fprintf(stderr, "Compression Level: %d\n", control.compression_level);
		}
		fprintf(stderr, "\n");
	}

	if (setpriority(PRIO_PROCESS, 0, control.nice_val) == -1)
		fatal("Unable to set nice value\n");

	/* One extra iteration for the case of no parameters means we will default to stdin/out */
	for (i = 0; i <= argc; i++) {
		if (i < argc)
			control.infile = argv[i];
		else if (!(i == 0 && (control.flags & FLAG_STDIN)))
			break;
		if (control.infile && (strcmp(control.infile, "-") == 0))
			control.flags |= FLAG_STDIN;

		if (control.outname && (strcmp(control.outname, "-") == 0)) {
			control.flags |= FLAG_STDOUT;
			control.msgout = stderr;
		}

		/* If we're using stdin and no output filename, use stdout */
		if ((control.flags & FLAG_STDIN) && !control.outname) {
			control.flags |= FLAG_STDOUT;
			control.msgout = stderr;
		}

		if (!(control.flags & FLAG_STDOUT))
			control.msgout = stdout;
		/* Implement signal handler only once flags are set */
		handler.sa_handler = &sighandler;
		sigaction(SIGTERM, &handler, 0);
		sigaction(SIGINT, &handler, 0);

		gettimeofday(&start_time, NULL);

		if (control.flags & (FLAG_DECOMPRESS | FLAG_TEST_ONLY))
			decompress_file();
		else if (control.flags & FLAG_INFO)
			get_fileinfo();
		else
			compress_file();

		/* compute total time */
		gettimeofday(&end_time, NULL);
		total_time = (end_time.tv_sec + (double)end_time.tv_usec / 1000000) -
			      (start_time.tv_sec + (double)start_time.tv_usec / 1000000);
		hours = (int)total_time / 3600;
		minutes = (int)(total_time - hours * 3600) / 60;
		seconds = total_time - hours * 60 - minutes * 60;
		if ((control.flags & FLAG_SHOW_PROGRESS) && !(control.flags & FLAG_INFO))
			fprintf(control.msgout, "Total time: %02d:%02d:%06.3f\n", hours, minutes, seconds);
	}

	return 0;
}
