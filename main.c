/*
   Copyright (C) 2006-2015 Con Kolivas
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
/* lrzip compression - main program */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <signal.h>
#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif
#ifdef HAVE_SYS_TIME_H
# include <sys/time.h>
#endif
#ifdef HAVE_SYS_RESOURCE_H
# include <sys/resource.h>
#endif
#ifdef HAVE_SYS_TYPES_H
# include <sys/types.h>
#endif
#ifdef HAVE_SYS_STAT_H
# include <sys/stat.h>
#endif

#include <termios.h>
#ifdef HAVE_ENDIAN_H
# include <endian.h>
#elif HAVE_SYS_ENDIAN_H
# include <sys/endian.h>
#endif
#ifdef HAVE_ARPA_INET_H
# include <arpa/inet.h>
#endif

#include <getopt.h>

#include "rzip.h"
#include "lrzip_core.h"
#include "util.h"
#include "stream.h"

/* needed for CRC routines */
#include "lzma/C/7zCrc.h"

static rzip_control base_control, local_control, *control;

static void usage(void)
{
	print_output("lrzip version %s\n", PACKAGE_VERSION);
	print_output("Copyright (C) Con Kolivas 2006-2015\n");
	print_output("Based on rzip ");
	print_output("Copyright (C) Andrew Tridgell 1998-2003\n\n");
	print_output("Usage: lrzip [options] <file...>\n");
	print_output("General options:\n");
	print_output("	-c, --check		check integrity of file written on decompression\n");
	print_output("	-d, --decompress	decompress\n");
	print_output("	-e, --encrypt		password protected sha512/aes128 encryption on compression\n");
	print_output("	-h, -?, --help		show help\n");
	print_output("	-H, --hash		display md5 hash integrity information\n");
	print_output("	-i, --info		show compressed file information\n");
	print_output("	-q, --quiet		don't show compression progress\n");
	print_output("	-t, --test		test compressed file integrity\n");
	print_output("	-v[v], --verbose	Increase verbosity\n");
	print_output("	-V, --version		show version\n");
	print_output("Options affecting output:\n");
	print_output("	-D, --delete		delete existing files\n");
	print_output("	-f, --force		force overwrite of any existing files\n");
	print_output("	-k, --keep-broken	keep broken or damaged output files\n");
	print_output("	-o, --outfile filename	specify the output file name and/or path\n");
	print_output("	-O, --outdir directory	specify the output directory when -o is not used\n");
	print_output("	-S, --suffix suffix	specify compressed suffix (default '.lrz')\n");
	print_output("Options affecting compression:\n");
	print_output("	-b, --bzip2		bzip2 compression\n");
	print_output("	-g, --gzip		gzip compression using zlib\n");
	print_output("	-l, --lzo		lzo compression (ultra fast)\n");
	print_output("	-n, --no-compress	no backend compression - prepare for other compressor\n");
	print_output("	-z, --zpaq		zpaq compression (best, extreme compression, extremely slow)\n");
	print_output("Low level options:\n");
	print_output("	-L, --level level	set lzma/bzip2/gzip compression level (1-9, default 7)\n");
	print_output("	-N, --nice-level value	Set nice value to value (default 19)\n");
	print_output("	-p, --threads value	Set processor count to override number of threads\n");
	print_output("	-m, --maxram size	Set maximim available ram in hundreds of MB\n");
	print_output("				overrides detected ammount of available ram\n");
	print_output("	-T, --threshold		Disable LZO compressibility testing\n");
	print_output("	-U, --unlimited		Use unlimited window size beyond ramsize (potentially much slower)\n");
	print_output("	-w, --window size	maximum compression window in hundreds of MB\n");
	print_output("				default chosen by heuristic dependent on ram and chosen compression\n");
	print_output("\nLRZIP=NOCONFIG environment variable setting can be used to bypass lrzip.conf.\n");
	print_output("TMP environment variable will be used for storage of temporary files when needed.\n");
	print_output("TMPDIR may also be stored in lrzip.conf file.\n");
	print_output("\nIf no filenames or \"-\" is specified, stdin/out will be used.\n");

}

static void sighandler(int sig __UNUSED__)
{
	struct termios termios_p;

	/* Make sure we haven't died after disabling stdin echo */
	tcgetattr(fileno(stdin), &termios_p);
	termios_p.c_lflag |= ECHO;
	tcsetattr(fileno(stdin), 0, &termios_p);

	unlink_files(control);
	exit(0);
}

static void show_summary(void)
{
	/* OK, if verbosity set, print summary of options selected */
	if (!INFO) {
		if (!TEST_ONLY)
			print_verbose("The following options are in effect for this %s.\n",
				      DECOMPRESS ? "DECOMPRESSION" : "COMPRESSION");
		print_verbose("Threading is %s. Number of CPUs detected: %d\n", control->threads > 1? "ENABLED" : "DISABLED",
			      control->threads);
		print_verbose("Detected %lld bytes ram\n", control->ramsize);
		print_verbose("Compression level %d\n", control->compression_level);
		print_verbose("Nice Value: %d\n", control->nice_val);
		print_verbose("Show Progress\n");
		print_maxverbose("Max ");
		print_verbose("Verbose\n");
		if (FORCE_REPLACE)
			print_verbose("Overwrite Files\n");
		if (!KEEP_FILES)
			print_verbose("Remove input files on completion\n");
		if (control->outdir)
			print_verbose("Output Directory Specified: %s\n", control->outdir);
		else if (control->outname)
			print_verbose("Output Filename Specified: %s\n", control->outname);
		if (TEST_ONLY)
			print_verbose("Test file integrity\n");
		if (control->tmpdir)
			print_verbose("Temporary Directory set as: %s\n", control->tmpdir);

		/* show compression options */
		if (!DECOMPRESS && !TEST_ONLY) {
			print_verbose("Compression mode is: ");
			if (LZMA_COMPRESS)
				print_verbose("LZMA. LZO Compressibility testing %s\n", (LZO_TEST? "enabled" : "disabled"));
			else if (LZO_COMPRESS)
				print_verbose("LZO\n");
			else if (BZIP2_COMPRESS)
				print_verbose("BZIP2. LZO Compressibility testing %s\n", (LZO_TEST? "enabled" : "disabled"));
			else if (ZLIB_COMPRESS)
				print_verbose("GZIP\n");
			else if (ZPAQ_COMPRESS)
				print_verbose("ZPAQ. LZO Compressibility testing %s\n", (LZO_TEST? "enabled" : "disabled"));
			else if (NO_COMPRESS)
				print_verbose("RZIP pre-processing only\n");
			if (control->window)
				print_verbose("Compression Window: %lld = %lldMB\n", control->window, control->window * 100ull);
			/* show heuristically computed window size */
			if (!control->window && !UNLIMITED) {
				i64 temp_chunk, temp_window;

				if (STDOUT || STDIN)
					temp_chunk = control->maxram;
				else
					temp_chunk = control->ramsize * 2 / 3;
				temp_window = temp_chunk / (100 * 1024 * 1024);
				print_verbose("Heuristically Computed Compression Window: %lld = %lldMB\n", temp_window, temp_window * 100ull);
			}
			if (UNLIMITED)
				print_verbose("Using Unlimited Window size\n");
		}
		if (!DECOMPRESS && !TEST_ONLY)
			print_maxverbose("Storage time in seconds %lld\n", control->secs);
		if (ENCRYPT)
			print_maxverbose("Encryption hash loops %lld\n", control->encloops);
	}
}

static struct option long_options[] = {
	{"bzip2",	no_argument,	0,	'b'},
	{"check",	no_argument,	0,	'c'},
	{"decompress",	no_argument,	0,	'd'},
	{"delete",	no_argument,	0,	'D'},
	{"encrypt",	no_argument,	0,	'e'},
	{"force",	no_argument,	0,	'f'},
	{"gzip",	no_argument,	0,	'g'},
	{"help",	no_argument,	0,	'h'},
	{"hash",	no_argument,	0,	'H'},
	{"info",	no_argument,	0,	'i'},
	{"keep-broken",	no_argument,	0,	'k'},
	{"lzo",		no_argument,	0,	'l'},
	{"level",	no_argument,	0,	'L'},
	{"maxram",	required_argument,	0,	'm'},
	{"no-compress",	no_argument,	0,	'n'},
	{"nice-level",	required_argument,	0,	'N'},
	{"outfile",	required_argument,	0,	'o'},
	{"outdir",	required_argument,	0,	'O'},
	{"threads",	required_argument,	0,	'p'},
	{"quiet",	no_argument,	0,	'q'},
	{"suffix",	required_argument,	0,	'S'},
	{"test",	no_argument,	0,	't'},
	{"threshold",	required_argument,	0,	'T'},
	{"unlimited",	no_argument,	0,	'U'},
	{"verbose",	no_argument,	0,	'v'},
	{"version",	no_argument,	0,	'V'},
	{"window",	required_argument,	0,	'w'},
	{"zpaq",	no_argument,	0,	'z'},
};

int main(int argc, char *argv[])
{
	struct timeval start_time, end_time;
	struct sigaction handler;
	double seconds,total_time; // for timers
	bool lrzcat = false;
	int c, i;
	int hours,minutes;
	extern int optind;
	char *eptr; /* for environment */

        control = &base_control;

	initialise_control(control);

	if (strstr(argv[0], "lrunzip"))
		control->flags |= FLAG_DECOMPRESS;
	else if (strstr(argv[0], "lrzcat")) {
		control->flags |= FLAG_DECOMPRESS | FLAG_STDOUT;
		lrzcat = true;
	}

	/* generate crc table */
	CrcGenerateTable();

	/* Get Preloaded Defaults from lrzip.conf
	 * Look in ., $HOME/.lrzip/, /etc/lrzip.
	 * If LRZIP=NOCONFIG is set, then ignore config
	 */
	eptr = getenv("LRZIP");
	if (eptr == NULL)
		read_config(control);
	else if (!strstr(eptr,"NOCONFIG"))
		read_config(control);

	while ((c = getopt_long(argc, argv, "bcdDefghHiklL:nN:o:O:p:qS:tTUm:vVw:z?", long_options, &i)) != -1) {
		switch (c) {
		case 'b':
			if (control->flags & FLAG_NOT_LZMA)
				failure("Can only use one of -l, -b, -g, -z or -n\n");
			control->flags |= FLAG_BZIP2_COMPRESS;
			break;
		case 'c':
			control->flags |= FLAG_CHECK;
			control->flags |= FLAG_HASH;
			break;
		case 'd':
			control->flags |= FLAG_DECOMPRESS;
			break;
		case 'D':
			control->flags &= ~FLAG_KEEP_FILES;
			break;
		case 'e':
			control->flags |= FLAG_ENCRYPT;
			break;
		case 'f':
			control->flags |= FLAG_FORCE_REPLACE;
			break;
		case 'g':
			if (control->flags & FLAG_NOT_LZMA)
				failure("Can only use one of -l, -b, -g, -z or -n\n");
			control->flags |= FLAG_ZLIB_COMPRESS;
			break;
		case 'h':
		case '?':
			usage();
			return -1;
		case 'H':
			control->flags |= FLAG_HASH;
			break;
		case 'i':
			control->flags |= FLAG_INFO;
			break;
		case 'k':
			control->flags |= FLAG_KEEP_BROKEN;
			break;
		case 'l':
			if (control->flags & FLAG_NOT_LZMA)
				failure("Can only use one of -l, -b, -g, -z or -n\n");
			control->flags |= FLAG_LZO_COMPRESS;
			break;
		case 'L':
			control->compression_level = atoi(optarg);
			if (control->compression_level < 1 || control->compression_level > 9)
				failure("Invalid compression level (must be 1-9)\n");
			break;
		case 'm':
			control->ramsize = atol(optarg) * 1024 * 1024 * 100;
			break;
		case 'n':
			if (control->flags & FLAG_NOT_LZMA)
				failure("Can only use one of -l, -b, -g, -z or -n\n");
			control->flags |= FLAG_NO_COMPRESS;
			break;
		case 'N':
			control->nice_val = atoi(optarg);
			if (control->nice_val < -20 || control->nice_val > 19)
				failure("Invalid nice value (must be -20..19)\n");
			break;
		case 'o':
			if (control->outdir)
				failure("Cannot have -o and -O together\n");
			if (unlikely(STDOUT))
				failure("Cannot specify an output filename when outputting to stdout\n");
			control->outname = optarg;
			control->suffix = "";
			break;
		case 'O':
			if (control->outname)	/* can't mix -o and -O */
				failure("Cannot have options -o and -O together\n");
			if (unlikely(STDOUT))
				failure("Cannot specify an output directory when outputting to stdout\n");
			control->outdir = malloc(strlen(optarg) + 2);
			if (control->outdir == NULL)
				fatal("Failed to allocate for outdir\n");
			strcpy(control->outdir,optarg);
			if (strcmp(optarg+strlen(optarg) - 1, "/")) 	/* need a trailing slash */
				strcat(control->outdir, "/");
			break;
		case 'p':
			control->threads = atoi(optarg);
			if (control->threads < 1)
				failure("Must have at least one thread\n");
			break;
		case 'q':
			control->flags &= ~FLAG_SHOW_PROGRESS;
			break;
		case 'S':
			if (control->outname)
				failure("Specified output filename already, can't specify an extension.\n");
			if (unlikely(STDOUT))
				failure("Cannot specify a filename suffix when outputting to stdout\n");
			control->suffix = optarg;
			break;
		case 't':
			if (control->outname)
				failure("Cannot specify an output file name when just testing.\n");
			if (!KEEP_FILES)
				failure("Doubt that you want to delete a file when just testing.\n");
			control->flags |= FLAG_TEST_ONLY;
			break;
		case 'T':
			control->flags &= ~FLAG_THRESHOLD;
			break;
		case 'U':
			control->flags |= FLAG_UNLIMITED;
			break;
		case 'v':
			/* set verbosity flag */
			if (!(control->flags & FLAG_VERBOSITY) && !(control->flags & FLAG_VERBOSITY_MAX))
				control->flags |= FLAG_VERBOSITY;
			else if ((control->flags & FLAG_VERBOSITY)) {
				control->flags &= ~FLAG_VERBOSITY;
				control->flags |= FLAG_VERBOSITY_MAX;
			}
			break;
		case 'V':
			print_output("lrzip version %s\n", PACKAGE_VERSION);
			exit(0);
			break;
		case 'w':
			control->window = atol(optarg);
			break;
		case 'z':
			if (control->flags & FLAG_NOT_LZMA)
				failure("Can only use one of -l, -b, -g, -z or -n\n");
			control->flags |= FLAG_ZPAQ_COMPRESS;
			break;
		}
	}

	argc -= optind;
	argv += optind;

	if (control->outname && argc > 1)
		failure("Cannot specify output filename with more than 1 file\n");

	if (VERBOSE && !SHOW_PROGRESS) {
		print_err("Cannot have -v and -q options. -v wins.\n");
		control->flags |= FLAG_SHOW_PROGRESS;
	}

	if (UNLIMITED && control->window) {
		print_err("If -U used, cannot specify a window size with -w.\n");
		control->window = 0;
	}

	if (argc < 1)
		control->flags |= FLAG_STDIN;

	if (UNLIMITED && STDIN) {
		print_err("Cannot have -U and stdin, unlimited mode disabled.\n");
		control->flags &= ~FLAG_UNLIMITED;
	}

	setup_overhead(control);

	/* Set the main nice value to half that of the backend threads since
	 * the rzip stage is usually the rate limiting step */
	if (control->nice_val > 0 && !NO_COMPRESS) {
		if (unlikely(setpriority(PRIO_PROCESS, 0, control->nice_val / 2) == -1))
			print_err("Warning, unable to set nice value\n");
	} else {
		if (unlikely(setpriority(PRIO_PROCESS, 0, control->nice_val) == -1))
			print_err("Warning, unable to set nice value\n");
	}

	/* One extra iteration for the case of no parameters means we will default to stdin/out */
	for (i = 0; i <= argc; i++) {
		if (i < argc)
			control->infile = argv[i];
		else if (!(i == 0 && STDIN))
			break;
		if (control->infile) {
			if ((strcmp(control->infile, "-") == 0))
				control->flags |= FLAG_STDIN;
			else {
				struct stat infile_stat;

				stat(control->infile, &infile_stat);
				if (unlikely(S_ISDIR(infile_stat.st_mode)))
					failure("lrzip only works directly on FILES.\n"
					"Use lrztar or pipe through tar for compressing directories.\n");
			}
		}

		if (INFO && STDIN)
			failure("Will not get file info from STDIN\n");

		if ((control->outname && (strcmp(control->outname, "-") == 0)) ||
			/* If no output filename is specified, and we're using
			 * stdin, use stdout */
			(!control->outname && STDIN) || lrzcat ) {
				control->flags |= FLAG_STDOUT;
				control->outFILE = stdout;
				control->msgout = stderr;
				register_outputfile(control, control->msgout);
		}

		if (lrzcat) {
			control->msgout = stderr;
			control->outFILE = stdout;
			register_outputfile(control, control->msgout);
		}

		if (!STDOUT) {
			control->msgout = stdout;
			register_outputfile(control, control->msgout);
		}

		if (STDIN)
			control->inFILE = stdin;
		/* Implement signal handler only once flags are set */
		sigemptyset(&handler.sa_mask);
		handler.sa_flags = 0;
		handler.sa_handler = &sighandler;
		sigaction(SIGTERM, &handler, 0);
		sigaction(SIGINT, &handler, 0);

		if (!FORCE_REPLACE) {
			if (STDIN && isatty(fileno((FILE *)stdin))) {
				print_err("Will not read stdin from a terminal. Use -f to override.\n");
				usage();
				exit (1);
			}
			if (!TEST_ONLY && STDOUT && isatty(fileno((FILE *)stdout))) {
				print_err("Will not write stdout to a terminal. Use -f to override.\n");
				usage();
				exit (1);
			}
		}

		if (CHECK_FILE) {
			if (!DECOMPRESS) {
				print_err("Can only check file written on decompression.\n");
				control->flags &= ~FLAG_CHECK;
			} else if (STDOUT) {
				print_err("Can't check file written when writing to stdout. Checking disabled.\n");
				control->flags &= ~FLAG_CHECK;
			}
		}

		setup_ram(control);
		show_summary();

		gettimeofday(&start_time, NULL);

		if (unlikely(STDIN && ENCRYPT))
			failure("Unable to work from STDIN while reading password\n");

		memcpy(&local_control, &base_control, sizeof(rzip_control));
		if (DECOMPRESS || TEST_ONLY)
			decompress_file(&local_control);
		else if (INFO)
			get_fileinfo(&local_control);
		else
			compress_file(&local_control);

		/* compute total time */
		gettimeofday(&end_time, NULL);
		total_time = (end_time.tv_sec + (double)end_time.tv_usec / 1000000) -
			      (start_time.tv_sec + (double)start_time.tv_usec / 1000000);
		hours = (int)total_time / 3600;
		minutes = (int)(total_time / 60) % 60;
		seconds = total_time - hours * 3600 - minutes * 60;
		if (!INFO)
			print_progress("Total time: %02d:%02d:%05.2f\n", hours, minutes, seconds);
	}

	return 0;
}
