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
	print_output("lrzip version %d.%d.%d\n", LRZIP_MAJOR_VERSION, LRZIP_MINOR_VERSION, LRZIP_MINOR_SUBVERSION);
	print_output("Copyright (C) Con Kolivas 2006-2010\n\n");
	print_output("Based on rzip ");
	print_output("Copyright (C) Andrew Tridgell 1998-2003\n");
	print_output("usage: lrzip [options] <file...>\n");
	print_output(" Options:\n");
	print_output("     -w size       maximum compression window in hundreds of MB\n");
	print_output("                   default chosen by heuristic dependent on ram and chosen compression\n");
	print_output("     -d            decompress\n");
	print_output("     -o filename   specify the output file name and/or path\n");
	print_output("     -O directory  specify the output directory when -o is not used\n");
	print_output("     -S suffix     specify compressed suffix (default '.lrz')\n");
	print_output("     -f            force overwrite of any existing files\n");
	print_output("     -D            delete existing files\n");
	print_output("     -P            don't set permissions on output file - may leave it world-readable\n");
	print_output("     -q            don't show compression progress\n");
	print_output("     -L level      set lzma/bzip2/gzip compression level (1-9, default 9)\n");
	print_output("     -n            no backend compression - prepare for other compressor\n");
	print_output("     -l            lzo compression (ultra fast)\n");
	print_output("     -b            bzip2 compression\n");
	print_output("     -g            gzip compression using zlib\n");
	print_output("     -z            zpaq compression (best, extreme compression, extremely slow)\n");
	print_output("     -M            Maximum window (all available ram)\n");
	print_output("     -U            Use unlimited window size beyond ramsize (potentially much slower)\n");
	print_output("     -T value      Compression threshold with LZO test. (0 (nil) - 10 (high), default 1)\n");
	print_output("     -N value      Set nice value to value (default 19)\n");
	print_output("     -v[v]         Increase verbosity\n");
	print_output("     -V            show version\n");
	print_output("     -t            test compressed file integrity\n");
	print_output("     -i            show compressed file information\n");
	print_output("\nIf no filenames or \"-\" is specified, stdin/out will be used.\n");
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

	if (unlikely(fstat(fd_in, &st)))
		fatal("bad magic file descriptor!?\n");

	memcpy(&magic[6], &control.st_size, 8);

	/* save LZMA compression flags */
	if (LZMA_COMPRESS) {
		for (i = 0; i < 5; i++)
			magic[i + 16] = (char)control.lzma_properties[i];
	}

	if (unlikely(lseek(fd_out, 0, SEEK_SET)))
		fatal("Failed to seek to BOF to write Magic Header\n");

	if (unlikely(write(fd_out, magic, sizeof(magic)) != sizeof(magic)))
		fatal("Failed to write magic header\n");
}

static void read_magic(int fd_in, i64 *expected_size)
{
	char magic[24];
	uint32_t v;
	int i;

	if (unlikely(read(fd_in, magic, sizeof(magic)) != sizeof(magic)))
		fatal("Failed to read magic header\n");

	*expected_size = 0;

	if (unlikely(strncmp(magic, "LRZI", 4)))
		fatal("Not an lrzip file\n");

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
	print_verbose("Detected lrzip version %d.%d file.\n", control.major_version, control.minor_version);
	if (control.major_version > LRZIP_MAJOR_VERSION ||
	    (control.major_version == LRZIP_MAJOR_VERSION && control.minor_version > LRZIP_MINOR_VERSION))
		print_output("Attempting to work with file produced by newer lrzip version %d.%d file.\n", control.major_version, control.minor_version);
}

/* preserve ownership and permissions where possible */
static void preserve_perms(int fd_in, int fd_out)
{
	struct stat st;

	if (unlikely(fstat(fd_in, &st)))
		fatal("Failed to fstat input file\n");
	if (fchmod(fd_out, (st.st_mode & 0777)))
		print_err("Warning, unable to set permissions on %s\n", control.outfile);

	/* chown fail is not fatal */
	fchown(fd_out, st.st_uid, st.st_gid);
}

/* Open a temporary outputfile to emulate stdout */
static int open_tmpoutfile(void)
{
	int fd_out;

	if (STDOUT)
		print_verbose("Outputting to stdout.\n");
	control.outfile = realloc(NULL, 16);
	strcpy(control.outfile, "lrzipout.XXXXXX");
	if (unlikely(!control.outfile))
		fatal("Failed to allocate outfile name\n");

	fd_out = mkstemp(control.outfile);
	if (unlikely(fd_out == -1))
		fatal("Failed to create out tmpfile: %s\n", strerror(errno));
	return fd_out;
}

/* Dump temporary outputfile to perform stdout */
static void dump_tmpoutfile(int fd_out)
{
	FILE *tmpoutfp;
	int tmpchar;

	print_progress("Dumping to stdout.\n");
	/* flush anything not yet in the temporary file */
	fsync(fd_out);
	tmpoutfp = fdopen(fd_out, "r");
	if (unlikely(tmpoutfp == NULL))
		fatal("Failed to fdopen out tmpfile: %s\n", strerror(errno));
	rewind(tmpoutfp);

	while ((tmpchar = fgetc(tmpoutfp)) != EOF)
		putchar(tmpchar);

	fflush(control.msgout);
}

/* Open a temporary inputfile to perform stdin decompression */
static int open_tmpinfile(void)
{
	int fd_in;

	control.infile = malloc(15);
	strcpy(control.infile, "lrzipin.XXXXXX");
	if (unlikely(!control.infile))
		fatal("Failed to allocate infile name\n");

	fd_in = mkstemp(control.infile);
	if (unlikely(fd_in == -1))
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
	if (unlikely(tmpinfp == NULL))
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

	if (!STDIN) {
		if ((tmp = strrchr(control.infile, '.')) && strcmp(tmp,control.suffix)) {
			/* make sure infile has an extension. If not, add it
			  * because manipulations may be made to input filename, set local ptr
			*/
			infilecopy = malloc(strlen(control.infile) + strlen(control.suffix) + 1);
			if (unlikely(infilecopy == NULL))
				fatal("Failed to allocate memory for infile suffix\n");
			else {
				strcpy(infilecopy, control.infile);
				strcat(infilecopy, control.suffix);
			}
		} else
			infilecopy = strdup(control.infile);
		/* regardless, infilecopy has the input filename */
	}

	if (!STDOUT && !TEST_ONLY) {
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
			if (unlikely(!control.outfile))
				fatal("Failed to allocate outfile name\n");

			if (control.outdir) {	/* prepend control.outdir */
				strcpy(control.outfile, control.outdir);
				strcat(control.outfile, tmpoutfile);
			} else
				strcpy(control.outfile, tmpoutfile);
			free(tmpoutfile);
		}

		if (!STDOUT)
			print_progress("Output filename is: %s...Decompressing...\n", control.outfile);
	}

	if (STDIN) {
		fd_in = open_tmpinfile();
		read_tmpinfile(fd_in);
	} else {
		fd_in = open(infilecopy, O_RDONLY);
		if (unlikely(fd_in == -1)) {
			fatal("Failed to open %s: %s\n",
			      infilecopy,
			      strerror(errno));
		}
	}

	if (!(TEST_ONLY | STDOUT)) {
		if (FORCE_REPLACE)
			fd_out = open(control.outfile, O_WRONLY | O_CREAT | O_TRUNC, 0666);
		else
			fd_out = open(control.outfile, O_WRONLY | O_CREAT | O_EXCL, 0666);
		if (unlikely(fd_out == -1))
			fatal("Failed to create %s: %s\n", control.outfile, strerror(errno));

		if (!NO_SET_PERMS)
			preserve_perms(fd_in, fd_out);
	} else
		fd_out = open_tmpoutfile();

	fd_hist = open(control.outfile, O_RDONLY);
	if (unlikely(fd_hist == -1))
		fatal("Failed to open history file %s\n", control.outfile);

	read_magic(fd_in, &expected_size);
	print_progress("Decompressing...");

	runzip_fd(fd_in, fd_out, fd_hist, expected_size);

	if (STDOUT)
		dump_tmpoutfile(fd_out);

	/* if we get here, no fatal errors during decompression */
	print_progress("\r");
	if (!(STDOUT | TEST_ONLY))
		print_output("Output filename is: %s: ", control.outfile);
        print_progress("[OK] - %lld bytes                                \n", expected_size);

	if (unlikely(close(fd_hist) || close(fd_out)))
		fatal("Failed to close files\n");

	if (TEST_ONLY | STDOUT) {
		/* Delete temporary files generated for testing or faking stdout */
		if (unlikely(unlink(control.outfile)))
			fatal("Failed to unlink tmpfile: %s\n", strerror(errno));
	}

	close(fd_in);

	if (!(KEEP_FILES | TEST_ONLY) || STDIN) {
		if (unlikely(unlink(control.infile)))
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

	if (!STDIN) {
		if ((tmp = strrchr(control.infile, '.')) && strcmp(tmp,control.suffix)) {
			infilecopy = malloc(strlen(control.infile) + strlen(control.suffix) + 1);
			if (unlikely(infilecopy == NULL))
				fatal("Failed to allocate memory for infile suffix\n");
			else {
				strcpy(infilecopy, control.infile);
				strcat(infilecopy, control.suffix);
			}
		} else
			infilecopy = strdup(control.infile);
	}

	if (STDIN)
		fd_in = 0;
	else {
		fd_in = open(infilecopy, O_RDONLY);
		if (unlikely(fd_in == -1))
			fatal("Failed to open %s: %s\n", infilecopy, strerror(errno));
	}

	/* Get file size */
	if (unlikely(fstat(fd_in, &st)))
		fatal("bad magic file descriptor!?\n");
	memcpy(&infile_size, &st.st_size, 8);

	/* Get decompressed size */
	read_magic(fd_in, &expected_size);

	/* Version < 0.4 had different file format */
	if (control.major_version == 0 && control.minor_version < 4)
		seekspot = 50;
	else
		seekspot = 74;
	if (unlikely(lseek(fd_in, seekspot, SEEK_SET) == -1))
		fatal("Failed to lseek in get_fileinfo: %s\n", strerror(errno));

	/* Read the compression type of the first block. It's possible that
	   not all blocks are compressed so this may not be accurate.
	 */
	read(fd_in, &ctype, 1);

	cratio = (long double)expected_size / (long double)infile_size;
	
	print_output("%s:\nlrzip version: %d.%d file\n", infilecopy, control.major_version, control.minor_version);
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
	print_output("Decompressed file size: %llu\n", expected_size);
	print_output("Compressed file size: %llu\n", infile_size);
	print_output("Compression ratio: %.3Lf\n", cratio);

	if (STDIN) {
		if (unlikely(unlink(control.infile)))
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

	if (!STDIN) {
		/* is extension at end of infile? */
		if ((tmp = strrchr(control.infile, '.')) && !strcmp(tmp, control.suffix)) {
			print_err("%s: already has %s suffix. Skipping...\n", control.infile, control.suffix);
			return;
		}

		fd_in = open(control.infile, O_RDONLY);
		if (unlikely(fd_in == -1))
			fatal("Failed to open %s: %s\n", control.infile, strerror(errno));
	} else
		fd_in = 0;

	if (!STDOUT) {
		if (control.outname) {
				/* check if outname has control.suffix */
				if (*(control.suffix) == '\0') /* suffix is empty string */
					control.outfile = strdup(control.outname);
				else if ((tmp=strrchr(control.outname, '.')) && strcmp(tmp, control.suffix)) {
					control.outfile = malloc(strlen(control.outname) + strlen(control.suffix) + 1);
					if (unlikely(!control.outfile))
						fatal("Failed to allocate outfile name\n");
					strcpy(control.outfile, control.outname);
					strcat(control.outfile, control.suffix);
					print_output("Suffix added to %s.\nFull pathname is: %s\n", control.outname, control.outfile);
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
			if (unlikely(!control.outfile))
				fatal("Failed to allocate outfile name\n");

			if (control.outdir) {	/* prepend control.outdir */
				strcpy(control.outfile, control.outdir);
				strcat(control.outfile, tmpinfile);
			} else
				strcpy(control.outfile, tmpinfile);
			strcat(control.outfile, control.suffix);
			print_progress("Output filename is: %s\n", control.outfile);
		}

		if (FORCE_REPLACE)
			fd_out = open(control.outfile, O_WRONLY | O_CREAT | O_TRUNC, 0666);
		else
			fd_out = open(control.outfile, O_WRONLY | O_CREAT | O_EXCL, 0666);
		if (unlikely(fd_out == -1))
			fatal("Failed to create %s: %s\n", control.outfile, strerror(errno));
	} else
		fd_out = open_tmpoutfile();

        if (!NO_SET_PERMS)
		preserve_perms(fd_in, fd_out);

	/* write zeroes to 24 bytes at beginning of file */
	if (unlikely(write(fd_out, header, sizeof(header)) != sizeof(header)))
		fatal("Cannot write file header\n");

	rzip_fd(fd_in, fd_out);

	/* write magic at end b/c lzma does not tell us properties until it is done */
	write_magic(fd_in, fd_out);

	if (STDOUT)
		dump_tmpoutfile(fd_out);

	if (unlikely(close(fd_in) || close(fd_out)))
		fatal("Failed to close files\n");

	if (STDOUT) {
		/* Delete temporary files generated for testing or faking stdout */
		if (unlikely(unlink(control.outfile)))
			fatal("Failed to unlink tmpfile: %s\n", strerror(errno));
	}

	if (!KEEP_FILES) {
		if (unlikely(unlink(control.infile)))
			fatal("Failed to unlink %s: %s\n", control.infile, strerror(errno));
	}

	free(control.outfile);
}

int main(int argc, char *argv[])
{
	struct timeval start_time, end_time;
	struct sigaction handler;
	double seconds,total_time; // for timers
	int c, i;
	int hours,minutes;
	extern int optind;
	char *eptr; /* for environment */

	memset(&control, 0, sizeof(control));

	control.msgout = stderr;
	control.flags = FLAG_SHOW_PROGRESS | FLAG_KEEP_FILES;
	control.suffix = ".lrz";
	control.outdir = NULL;

	if (strstr(argv[0], "lrunzip"))
		control.flags |= FLAG_DECOMPRESS;

	control.compression_level = 9;
	control.ramsize = get_ram();
	control.window = 0;
	control.threshold = 1.0;	/* default lzo test compression threshold (level 1) with LZMA compression */
	/* for testing single CPU */
	control.threads = PROCESSORS;	/* get CPUs for LZMA */
	control.page_size = PAGE_SIZE;

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

	while ((c = getopt(argc, argv, "L:hdS:tVvDfqo:w:nlbMUO:T:N:gPzi")) != -1) {
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
			if (!KEEP_FILES)
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
			print_output("lrzip version %d.%d.%d\n",
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
			control.flags |= FLAG_MAXRAM;
			break;
		case 'U':
			control.flags |= FLAG_UNLIMITED;
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

	if (VERBOSE && !SHOW_PROGRESS) {
		print_err("Cannot have -v and -q options. -v wins.\n");
		control.flags |= FLAG_SHOW_PROGRESS;
	}

	if (argc < 1)
		control.flags |= FLAG_STDIN;

	if (UNLIMITED && STDIN) {
		print_err("Cannot have -U and stdin, unlimited mode disabled.\n");
		control.flags &= ~ FLAG_UNLIMITED;
	}

	/* OK, if verbosity set, print summary of options selected */
	if (!INFO) {
		print_verbose("The following options are in effect for this %s.\n",
			DECOMPRESS ? "DECOMPRESSION" : "COMPRESSION");
		if (LZMA_COMPRESS)
			print_verbose("Threading is %s. Number of CPUs detected: %lu\n", control.threads > 1? "ENABLED" : "DISABLED",
				control.threads);
		print_verbose("Detected %lld bytes ram\n", control.ramsize);
		print_verbose("Nice Value: %d\n", control.nice_val);
		print_progress("Show Progress\n");
		print_maxverbose("Max ");
		print_verbose("Verbose\n");
		if (FORCE_REPLACE)
			print_verbose("Overwrite Files\n");
		if (!KEEP_FILES)
			print_verbose("Remove input files on completion\n");
		if (control.outdir)
			print_verbose("Output Directory Specified: %s\n", control.outdir);
		else if (control.outname)
			print_verbose("Output Filename Specified: %s\n", control.outname);
		if (TEST_ONLY)
			print_verbose("Test file integrity\n");

		/* show compression options */
		if (!DECOMPRESS) {
			print_verbose("Compression mode is: ");
			if (LZMA_COMPRESS)
				print_verbose("LZMA. LZO Test Compression Threshold: %.f\n",
				       (control.threshold < 1.05 ? 21 - control.threshold * 20 : 0));
			else if (LZO_COMPRESS)
				print_verbose("LZO\n");
			else if (BZIP2_COMPRESS)
				print_verbose("BZIP2. LZO Test Compression Threshold: %.f\n",
				       (control.threshold < 1.05 ? 21 - control.threshold * 20 : 0));
			else if (ZLIB_COMPRESS)
				print_verbose("GZIP\n");
			else if (ZPAQ_COMPRESS)
				print_verbose("ZPAQ. LZO Test Compression Threshold: %.f\n",
				       (control.threshold < 1.05 ? 21 - control.threshold * 20 : 0));
			else if (NO_COMPRESS)
				print_verbose("RZIP pre-processing only\n");
			if (control.window) {
				print_verbose("Compression Window: %lld = %lldMB\n", control.window, control.window * 100ull);
				print_verbose("Compression Level: %d\n", control.compression_level);
			}
		}
	}

	if (unlikely(setpriority(PRIO_PROCESS, 0, control.nice_val) == -1))
		print_err("Warning, unable to set nice value\n");

	/* One extra iteration for the case of no parameters means we will default to stdin/out */
	for (i = 0; i <= argc; i++) {
		if (i < argc)
			control.infile = argv[i];
		else if (!(i == 0 && STDIN))
			break;
		if (control.infile && (strcmp(control.infile, "-") == 0))
			control.flags |= FLAG_STDIN;

		if (control.outname && (strcmp(control.outname, "-") == 0)) {
			control.flags |= FLAG_STDOUT;
			control.msgout = stderr;
		}

		/* If we're using stdin and no output filename, use stdout */
		if (STDIN && !control.outname) {
			control.flags |= FLAG_STDOUT;
			control.msgout = stderr;
		}

		if (!STDOUT)
			control.msgout = stdout;
		/* Implement signal handler only once flags are set */
		handler.sa_handler = &sighandler;
		sigaction(SIGTERM, &handler, 0);
		sigaction(SIGINT, &handler, 0);

		gettimeofday(&start_time, NULL);

		if (control.flags & (FLAG_DECOMPRESS | FLAG_TEST_ONLY))
			decompress_file();
		else if (INFO)
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
		if (!INFO)
			print_progress("Total time: %02d:%02d:%06.3f\n", hours, minutes, seconds);
	}

	return 0;
}
