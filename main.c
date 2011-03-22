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
/* lrzip compression - main program */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <signal.h>
#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif
#ifdef HAVE_CTYPE_H
# include <ctype.h>
#endif
#ifdef HAVE_SYS_TIME_H
# include <sys/time.h>
#endif
#ifdef HAVE_SYS_RESOURCE_H
# include <sys/resource.h>
#endif
#include <math.h>
#include <termios.h>
#ifdef HAVE_ENDIAN_H
# include <endian.h>
#elif HAVE_SYS_ENDIAN_H
# include <sys/endian.h>
#endif

#include "rzip.h"
#include "lrzip.h"
#include "util.h"
#include "stream.h"

/* needed for CRC routines */
#include "lzma/C/7zCrc.h"

/* Macros for testing parameters */
#define isparameter( parmstring, value )	(!strcasecmp( parmstring, value ))
#define iscaseparameter( parmvalue, value )	(!strcmp( parmvalue, value ))

/* main() defines, different from liblrzip defines */
#define FLAG_VERBOSE (FLAG_VERBOSITY | FLAG_VERBOSITY_MAX)
#define FLAG_NOT_LZMA (FLAG_NO_COMPRESS | FLAG_LZO_COMPRESS | FLAG_BZIP2_COMPRESS | FLAG_ZLIB_COMPRESS | FLAG_ZPAQ_COMPRESS)
#define LZMA_COMPRESS	(!(control.flags & FLAG_NOT_LZMA))

#define SHOW_PROGRESS	(control.flags & FLAG_SHOW_PROGRESS)
#define KEEP_FILES	(control.flags & FLAG_KEEP_FILES)
#define TEST_ONLY	(control.flags & FLAG_TEST_ONLY)
#define FORCE_REPLACE	(control.flags & FLAG_FORCE_REPLACE)
#define DECOMPRESS	(control.flags & FLAG_DECOMPRESS)
#define NO_COMPRESS	(control.flags & FLAG_NO_COMPRESS)
#define LZO_COMPRESS	(control.flags & FLAG_LZO_COMPRESS)
#define BZIP2_COMPRESS	(control.flags & FLAG_BZIP2_COMPRESS)
#define ZLIB_COMPRESS	(control.flags & FLAG_ZLIB_COMPRESS)
#define ZPAQ_COMPRESS	(control.flags & FLAG_ZPAQ_COMPRESS)
#define VERBOSE		(control.flags & FLAG_VERBOSE)
#define VERBOSITY	(control.flags & FLAG_VERBOSITY)
#define MAX_VERBOSE	(control.flags & FLAG_VERBOSITY_MAX)
#define STDIN		(control.flags & FLAG_STDIN)
#define STDOUT		(control.flags & FLAG_STDOUT)
#define INFO		(control.flags & FLAG_INFO)
#define UNLIMITED	(control.flags & FLAG_UNLIMITED)
#define HASH_CHECK	(control.flags & FLAG_HASH)
#define HAS_MD5		(control.flags & FLAG_MD5)
#define CHECK_FILE	(control.flags & FLAG_CHECK)
#define KEEP_BROKEN	(control.flags & FLAG_KEEP_BROKEN)
#define LZO_TEST	(control.flags & FLAG_THRESHOLD)
#define TMP_OUTBUF	(control.flags & FLAG_TMP_OUTBUF)
#define TMP_INBUF	(control.flags & FLAG_TMP_INBUF)
#define ENCRYPT		(control.flags & FLAG_ENCRYPT)
#define CHUNKED		(control.flags & FLAG_CHUNKED)

#define print_output(format, args...)	do {\
	fprintf(control.msgout, format, ##args);	\
	fflush(control.msgout);	\
} while (0)

#define print_progress(format, args...)	do {\
	if (SHOW_PROGRESS)	\
		print_output(format, ##args);	\
} while (0)

#define print_verbose(format, args...)	do {\
	if (VERBOSE)	\
		print_output(format, ##args);	\
} while (0)

#define print_maxverbose(format, args...)	do {\
	if (MAX_VERBOSE)	\
		print_output(format, ##args);	\
} while (0)


#if defined(NOTHREAD) || !defined(_SC_NPROCESSORS_ONLN)
# define PROCESSORS (1)
#else
# define PROCESSORS (sysconf(_SC_NPROCESSORS_ONLN))
#endif

#ifdef _SC_PAGE_SIZE
# define PAGE_SIZE (sysconf(_SC_PAGE_SIZE))
#else
# define PAGE_SIZE (4096)
#endif

#ifdef __APPLE__
# include <sys/sysctl.h>
static inline i64 get_ram(void)
{
	int mib[2];
	size_t len;
	i64 *p, ramsize;

	mib[0] = CTL_HW;
	mib[1] = HW_MEMSIZE;
	sysctl(mib, 2, NULL, &len, NULL, 0);
	p = malloc(len);
	sysctl(mib, 2, p, &len, NULL, 0);
	ramsize = *p;

	return ramsize;
}
#else /* __APPLE__ */
static inline i64 get_ram(void)
{
	i64 ramsize;
	FILE *meminfo;
	char aux[256];

	ramsize = (i64)sysconf(_SC_PHYS_PAGES) * PAGE_SIZE;
	if (ramsize > 0)
		return ramsize;

	/* Workaround for uclibc which doesn't properly support sysconf */
	if(!(meminfo = fopen("/proc/meminfo", "r")))
		fatal("fopen\n");

	while(!feof(meminfo) && !fscanf(meminfo, "MemTotal: %Lu kB", &ramsize)) {
		if (unlikely(fgets(aux, sizeof(aux), meminfo) == NULL))
			fatal("Failed to fgets in get_ram\n");
	}
	if (fclose(meminfo) == -1)
		fatal("fclose");
	ramsize *= 1000;

	return ramsize;
}
#endif

static rzip_control control;

static void usage(void)
{
	print_output("lrzip version %s\n", PACKAGE_VERSION);
	print_output("Copyright (C) Con Kolivas 2006-2011\n");
	print_output("Based on rzip ");
	print_output("Copyright (C) Andrew Tridgell 1998-2003\n\n");
	print_output("Usage: lrzip [options] <file...>\n");
	print_output("General options:\n");
	print_output("     -c            check integrity of file written on decompression\n");
	print_output("     -d            decompress\n");
	print_output("     -e            password protected sha512/aes128 encryption on compression\n");
	print_output("     -h|-?         show help\n");
	print_output("     -H            display md5 hash integrity information\n");
	print_output("     -i            show compressed file information\n");
	print_output("     -q            don't show compression progress\n");
	print_output("     -t            test compressed file integrity\n");
	print_output("     -v[v]         Increase verbosity\n");
	print_output("     -V            show version\n");
	print_output("Options affecting output:\n");
	print_output("     -D            delete existing files\n");
	print_output("     -f            force overwrite of any existing files\n");
	print_output("     -k            keep broken or damaged output files\n");
	print_output("     -o filename   specify the output file name and/or path\n");
	print_output("     -O directory  specify the output directory when -o is not used\n");
	print_output("     -S suffix     specify compressed suffix (default '.lrz')\n");
	print_output("Options affecting compression:\n");
	print_output("     -b            bzip2 compression\n");
	print_output("     -g            gzip compression using zlib\n");
	print_output("     -l            lzo compression (ultra fast)\n");
	print_output("     -n            no backend compression - prepare for other compressor\n");
	print_output("     -z            zpaq compression (best, extreme compression, extremely slow)\n");
	print_output("Low level options:\n");
	print_output("     -L level      set lzma/bzip2/gzip compression level (1-9, default 7)\n");
	print_output("     -N value      Set nice value to value (default 19)\n");
	print_output("     -p value      Set processor count to override number of threads\n");
	print_output("     -T            Disable LZO compressibility testing\n");
	print_output("     -U            Use unlimited window size beyond ramsize (potentially much slower)\n");
	print_output("     -w size       maximum compression window in hundreds of MB\n");
	print_output("                   default chosen by heuristic dependent on ram and chosen compression\n");
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

	unlink_files();
	exit(0);
}

static void show_summary(void)
{
	/* OK, if verbosity set, print summary of options selected */
	if (!INFO) {
		if (!TEST_ONLY)
			print_verbose("The following options are in effect for this %s.\n",
				      DECOMPRESS ? "DECOMPRESSION" : "COMPRESSION");
		print_verbose("Threading is %s. Number of CPUs detected: %d\n", control.threads > 1? "ENABLED" : "DISABLED",
			      control.threads);
		print_verbose("Detected %lld bytes ram\n", control.ramsize);
		print_verbose("Compression level %d\n", control.compression_level);
		print_verbose("Nice Value: %d\n", control.nice_val);
		print_verbose("Show Progress\n");
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
		if (control.tmpdir)
			print_verbose("Temporary Directory set as: %s\n", control.tmpdir);

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
			if (control.window)
				print_verbose("Compression Window: %lld = %lldMB\n", control.window, control.window * 100ull);
			/* show heuristically computed window size */
			if (!control.window && !UNLIMITED) {
				i64 temp_chunk, temp_window;

				if (STDOUT || STDIN)
					temp_chunk = control.maxram;
				else
					temp_chunk = control.maxram * 2;
				temp_window = temp_chunk / (100 * 1024 * 1024);
				print_verbose("Heuristically Computed Compression Window: %lld = %lldMB\n", temp_window, temp_window * 100ull);
			}
			if (UNLIMITED)
				print_verbose("Using Unlimited Window size\n");
		}
		print_maxverbose("Storage time in seconds %lld\n", control.secs);
		if (ENCRYPT)
			print_maxverbose("Encryption hash loops %lld\n", control.encloops);
	}
}

static void read_config(rzip_control *control)
{
	/* check for lrzip.conf in ., $HOME/.lrzip and /etc/lrzip */
	char *HOME, *homeconf;
	char *parametervalue;
	char *parameter;
	char *line;
	FILE *fp;

	line = malloc(255);
	homeconf = malloc(255);
	if (line == NULL || homeconf == NULL)
		fatal("Fatal Memory Error in read_config");

	fp = fopen("lrzip.conf", "r");
	if (fp)
		fprintf(control->msgout, "Using configuration file ./lrzip.conf\n");
	if (fp == NULL) {
		fp = fopen("/etc/lrzip/lrzip.conf", "r");
		if (fp)
			fprintf(control->msgout, "Using configuration file /etc/lrzip/lrzip.conf\n");
	}
	if (fp == NULL) {
		HOME=getenv("HOME");
		if (HOME) {
			strcpy(homeconf, HOME);
			strcat(homeconf,"/.lrzip/lrzip.conf");
			fp = fopen(homeconf, "r");
			if (fp)
				fprintf(control->msgout, "Using configuration file %s\n", homeconf);
		}
	}
	if (fp == NULL)
		goto out;

	/* if we get here, we have a file. read until no more. */

	while ((fgets(line, 255, fp)) != NULL) {
		if (strlen(line))
			line[strlen(line) - 1] = '\0';
		parameter = strtok(line, " =");
		if (parameter == NULL)
			continue;
		/* skip if whitespace or # */
		if (isspace(*parameter))
			continue;
		if (*parameter == '#')
			continue;

		parametervalue = strtok(NULL, " =");
		if (parametervalue == NULL)
			continue;

		/* have valid parameter line, now assign to control */

		if (isparameter(parameter, "window"))
			control->window = atoi(parametervalue);
		else if (isparameter(parameter, "unlimited")) {
			if (isparameter(parametervalue, "yes"))
				control->flags |= FLAG_UNLIMITED;
		} else if (isparameter(parameter, "compressionlevel")) {
			control->compression_level = atoi(parametervalue);
			if ( control->compression_level < 1 || control->compression_level > 9 )
				failure("CONF.FILE error. Compression Level must between 1 and 9");
		} else if (isparameter(parameter, "compressionmethod")) {
			/* valid are rzip, gzip, bzip2, lzo, lzma (default), and zpaq */
			if (control->flags & FLAG_NOT_LZMA)
				failure("CONF.FILE error. Can only specify one compression method");
			if (isparameter(parametervalue, "bzip2"))
				control->flags |= FLAG_BZIP2_COMPRESS;
			else if (isparameter(parametervalue, "gzip"))
				control->flags |= FLAG_ZLIB_COMPRESS;
			else if (isparameter(parametervalue, "lzo"))
				control->flags |= FLAG_LZO_COMPRESS;
			else if (isparameter(parametervalue, "rzip"))
				control->flags |= FLAG_NO_COMPRESS;
			else if (isparameter(parametervalue, "zpaq"))
				control->flags |= FLAG_ZPAQ_COMPRESS;
			else if (!isparameter(parametervalue, "lzma")) /* oops, not lzma! */
				failure("CONF.FILE error. Invalid compression method %s specified\n",parametervalue);
		} else if (isparameter(parameter, "lzotest")) {
			/* default is yes */
			if (isparameter(parametervalue, "no"))
				control->flags &= ~FLAG_THRESHOLD;
		} else if (isparameter(parameter, "hashcheck")) {
			if (isparameter(parametervalue, "yes")) {
				control->flags |= FLAG_CHECK;
				control->flags |= FLAG_HASH;
			}
		} else if (isparameter(parameter, "showhash")) {
			if (isparameter(parametervalue, "yes"))
				control->flags |= FLAG_HASH;
		} else if (isparameter(parameter, "outputdirectory")) {
			control->outdir = malloc(strlen(parametervalue) + 2);
			if (!control->outdir)
				fatal("Fatal Memory Error in read_config");
			strcpy(control->outdir, parametervalue);
			if (strcmp(parametervalue + strlen(parametervalue) - 1, "/"))
				strcat(control->outdir, "/");
		} else if (isparameter(parameter,"verbosity")) {
			if (control->flags & FLAG_VERBOSE)
				failure("CONF.FILE error. Verbosity already defined.");
			if (isparameter(parametervalue, "yes"))
				control->flags |= FLAG_VERBOSITY;
			else if (isparameter(parametervalue,"max"))
				control->flags |= FLAG_VERBOSITY_MAX;
			else /* oops, unrecognized value */
				print_err("lrzip.conf: Unrecognized verbosity value %s. Ignored.\n", parametervalue);
		} else if (isparameter(parameter, "showprogress")) {
			/* Yes by default */
			if (isparameter(parametervalue, "NO"))
				control->flags &= ~FLAG_SHOW_PROGRESS;
		} else if (isparameter(parameter,"nice")) {
			control->nice_val = atoi(parametervalue);
			if (control->nice_val < -20 || control->nice_val > 19)
				failure("CONF.FILE error. Nice must be between -20 and 19");
		} else if (isparameter(parameter, "keepbroken")) {
			if (isparameter(parametervalue, "yes" ))
				control->flags |= FLAG_KEEP_BROKEN;
		} else if (iscaseparameter(parameter, "DELETEFILES")) {
			/* delete files must be case sensitive */
			if (iscaseparameter(parametervalue, "YES"))
				control->flags &= ~FLAG_KEEP_FILES;
		} else if (iscaseparameter(parameter, "REPLACEFILE")) {
			/* replace lrzip file must be case sensitive */
			if (iscaseparameter(parametervalue, "YES"))
				control->flags |= FLAG_FORCE_REPLACE;
		} else if (isparameter(parameter, "tmpdir")) {
			control->tmpdir = realloc(NULL, strlen(parametervalue) + 2);
			if (!control->tmpdir)
				fatal("Fatal Memory Error in read_config");
			strcpy(control->tmpdir, parametervalue);
			if (strcmp(parametervalue + strlen(parametervalue) - 1, "/"))
				strcat(control->tmpdir, "/");
		} else
			/* oops, we have an invalid parameter, display */
			print_err("lrzip.conf: Unrecognized parameter value, %s = %s. Continuing.\n",\
				       parameter, parametervalue);
	}

	if (unlikely(fclose(fp)))
		fatal("Failed to fclose fp in read_config\n");
out:
	/* clean up */
	free(line);
	free(homeconf);

/*	fprintf(stderr, "\nWindow = %d \
		\nCompression Level = %d \
		\nThreshold = %1.2f \
		\nOutput Directory = %s \
		\nFlags = %d\n", control->window,control->compression_level, control->threshold, control->outdir, control->flags);
*/
}

/* Determine how many times to hash the password when encrypting, based on
 * the date such that we increase the number of loops according to Moore's
 * law relative to when the data is encrypted. It is then stored as a two
 * byte value in the header */
#define MOORE 1.835          // world constant  [TIMES per YEAR]
#define ARBITRARY  1000000   // number of sha2 calls per one second in 2011
#define T_ZERO 1293840000    // seconds since epoch in 2011

#define SECONDS_IN_A_YEAR (365*86400)
#define MOORE_TIMES_PER_SECOND pow (MOORE, 1.0 / SECONDS_IN_A_YEAR)
#define ARBITRARY_AT_EPOCH (ARBITRARY * pow (MOORE_TIMES_PER_SECOND, -T_ZERO))

static i64 nloops(i64 seconds, uchar *b1, uchar *b2)
{
	i64 nloops;
	int nbits;

	nloops = ARBITRARY_AT_EPOCH * pow(MOORE_TIMES_PER_SECOND, seconds);
	if (nloops < ARBITRARY)
		nloops = ARBITRARY;
	for (nbits = 0; nloops > 255; nbits ++)
		nloops = nloops >> 1;
	*b1 = nbits;
	*b2 = nloops;
	return nloops << nbits;
}

int main(int argc, char *argv[])
{
	struct timeval start_time, end_time, tv;
	struct sigaction handler;
	double seconds,total_time; // for timers
	int c, i;
	int hours,minutes;
	extern int optind;
	char *eptr; /* for environment */

	memset(&control, 0, sizeof(control));

	control.msgout = stderr;
	register_outputfile(control.msgout);
	control.flags = FLAG_SHOW_PROGRESS | FLAG_KEEP_FILES | FLAG_THRESHOLD;
	control.suffix = ".lrz";
	control.outdir = NULL;
	control.tmpdir = NULL;

	if (strstr(argv[0], "lrunzip"))
		control.flags |= FLAG_DECOMPRESS;

	control.compression_level = 7;
	control.ramsize = get_ram();
	/* for testing single CPU */
	control.threads = PROCESSORS;	/* get CPUs for LZMA */
	control.page_size = PAGE_SIZE;
	control.nice_val = 19;

	/* The first 5 bytes of the salt is the time in seconds.
	 * The next 2 bytes encode how many times to hash the password.
	 * The last 9 bytes are random data, making 16 bytes of salt */
	if (unlikely(gettimeofday(&tv, NULL)))
		fatal("Failed to gettimeofday in main\n");
	control.secs = tv.tv_sec;
	control.encloops = nloops(control.secs, control.salt, control.salt + 1);
	get_rand(control.salt + 2, 6);

	/* generate crc table */
	CrcGenerateTable();

	/* Get Temp Dir */
	eptr = getenv("TMP");
	if (eptr != NULL) {
		control.tmpdir = malloc(strlen(eptr)+2);
		if (control.tmpdir == NULL)
			fatal("Failed to allocate for tmpdir\n");
		strcpy(control.tmpdir, eptr);
		if (strcmp(eptr+strlen(eptr) - 1, "/")) 	/* need a trailing slash */
			strcat(control.tmpdir, "/");
	}

	/* Get Preloaded Defaults from lrzip.conf
	 * Look in ., $HOME/.lrzip/, /etc/lrzip.
	 * If LRZIP=NOCONFIG is set, then ignore config
	 */
	eptr = getenv("LRZIP");
	if (eptr == NULL)
		read_config(&control);
	else if (!strstr(eptr,"NOCONFIG"))
		read_config(&control);

	while ((c = getopt(argc, argv, "bcdDefghHiklL:nN:o:O:p:qS:tTUvVw:z?")) != -1) {
		switch (c) {
		case 'b':
			if (control.flags & FLAG_NOT_LZMA)
				failure("Can only use one of -l, -b, -g, -z or -n\n");
			control.flags |= FLAG_BZIP2_COMPRESS;
			break;
		case 'c':
			control.flags |= FLAG_CHECK;
			control.flags |= FLAG_HASH;
			break;
		case 'd':
			control.flags |= FLAG_DECOMPRESS;
			break;
		case 'D':
			control.flags &= ~FLAG_KEEP_FILES;
			break;
		case 'e':
			control.flags |= FLAG_ENCRYPT;
			break;
		case 'f':
			control.flags |= FLAG_FORCE_REPLACE;
			break;
		case 'g':
			if (control.flags & FLAG_NOT_LZMA)
				failure("Can only use one of -l, -b, -g, -z or -n\n");
			control.flags |= FLAG_ZLIB_COMPRESS;
			break;
		case 'h':
		case '?':
			usage();
			return -1;
		case 'H':
			control.flags |= FLAG_HASH;
			break;
		case 'i':
			control.flags |= FLAG_INFO;
			break;
		case 'k':
			control.flags |= FLAG_KEEP_BROKEN;
			break;
		case 'l':
			if (control.flags & FLAG_NOT_LZMA)
				failure("Can only use one of -l, -b, -g, -z or -n\n");
			control.flags |= FLAG_LZO_COMPRESS;
			break;
		case 'L':
			control.compression_level = atoi(optarg);
			if (control.compression_level < 1 || control.compression_level > 9)
				failure("Invalid compression level (must be 1-9)\n");
			break;
		case 'n':
			if (control.flags & FLAG_NOT_LZMA)
				failure("Can only use one of -l, -b, -g, -z or -n\n");
			control.flags |= FLAG_NO_COMPRESS;
			break;
		case 'N':
			control.nice_val = atoi(optarg);
			if (control.nice_val < -20 || control.nice_val > 19)
				failure("Invalid nice value (must be -20..19)\n");
			break;
		case 'o':
			if (control.outdir)
				failure("Cannot have -o and -O together\n");
			control.outname = optarg;
			break;
		case 'O':
			if (control.outname)	/* can't mix -o and -O */
				failure("Cannot have options -o and -O together\n");
			control.outdir = malloc(strlen(optarg) + 2);
			if (control.outdir == NULL)
				fatal("Failed to allocate for outdir\n");
			strcpy(control.outdir,optarg);
			if (strcmp(optarg+strlen(optarg) - 1, "/")) 	/* need a trailing slash */
				strcat(control.outdir, "/");
			break;
		case 'p':
			control.threads = atoi(optarg);
			if (control.threads < 1)
				failure("Must have at least one thread\n");
			break;
		case 'q':
			control.flags &= ~FLAG_SHOW_PROGRESS;
			break;
		case 'S':
			control.suffix = optarg;
			break;
		case 't':
			if (control.outname)
				failure("Cannot specify an output file name when just testing.\n");
			if (!KEEP_FILES)
				failure("Doubt that you want to delete a file when just testing.\n");
			control.flags |= FLAG_TEST_ONLY;
			break;
		case 'T':
			control.flags &= ~FLAG_THRESHOLD;
			break;
		case 'U':
			control.flags |= FLAG_UNLIMITED;
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
		case 'V':
			print_output("lrzip version %d.%d%d\n",
				LRZIP_MAJOR_VERSION, LRZIP_MINOR_VERSION, LRZIP_MINOR_SUBVERSION);
			exit(0);
			break;
		case 'w':
			control.window = atol(optarg);
			break;
		case 'z':
			if (control.flags & FLAG_NOT_LZMA)
				failure("Can only use one of -l, -b, -g, -z or -n\n");
			control.flags |= FLAG_ZPAQ_COMPRESS;
			break;
		}
	}

	argc -= optind;
	argv += optind;

	if (control.outname && argc > 1)
		failure("Cannot specify output filename with more than 1 file\n");

	if (VERBOSE && !SHOW_PROGRESS) {
		print_err("Cannot have -v and -q options. -v wins.\n");
		control.flags |= FLAG_SHOW_PROGRESS;
	}

	if (UNLIMITED && control.window) {
		print_err("If -U used, cannot specify a window size with -w.\n");
		control.window = 0;
	}

	if (argc < 1)
		control.flags |= FLAG_STDIN;

	if (UNLIMITED && STDIN) {
		print_err("Cannot have -U and stdin, unlimited mode disabled.\n");
		control.flags &= ~FLAG_UNLIMITED;
	}

	/* Work out the compression overhead per compression thread for the
	 * compression back-ends that need a lot of ram */
	if (LZMA_COMPRESS) {
		int level = control.compression_level * 7 / 9 ? : 1;
		i64 dictsize = (level <= 5 ? (1 << (level * 2 + 14)) :
				(level == 6 ? (1 << 25) : (1 << 26)));

		control.overhead = (dictsize * 23 / 2) + (4 * 1024 * 1024);
	} else if (ZPAQ_COMPRESS)
		control.overhead = 112 * 1024 * 1024;

	/* Set the main nice value to half that of the backend threads since
	 * the rzip stage is usually the rate limiting step */
	if (control.nice_val > 0 && !NO_COMPRESS) {
		if (unlikely(setpriority(PRIO_PROCESS, 0, control.nice_val / 2) == -1))
			print_err("Warning, unable to set nice value\n");
	} else {
		if (unlikely(setpriority(PRIO_PROCESS, 0, control.nice_val) == -1))
			print_err("Warning, unable to set nice value\n");
	}

	/* One extra iteration for the case of no parameters means we will default to stdin/out */
	for (i = 0; i <= argc; i++) {
		if (i < argc)
			control.infile = argv[i];
		else if (!(i == 0 && STDIN))
			break;
		if (control.infile && (strcmp(control.infile, "-") == 0))
			control.flags |= FLAG_STDIN;

		if (INFO && STDIN)
			failure("Will not get file info from STDIN\n");

		if (control.outname && (strcmp(control.outname, "-") == 0)) {
			control.flags |= FLAG_STDOUT;
			control.msgout = stderr;
			register_outputfile(control.msgout);
		}

		/* If we're using stdin and no output filename, use stdout */
		if (STDIN && !control.outname) {
			control.flags |= FLAG_STDOUT;
			control.msgout = stderr;
			register_outputfile(control.msgout);
		}

		if (!STDOUT) {
			control.msgout = stdout;
			register_outputfile(control.msgout);
		}
		/* Implement signal handler only once flags are set */
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
				control.flags &= ~FLAG_CHECK;
			} else if (STDOUT) {
				print_err("Can't check file written when writing to stdout. Checking disabled.\n");
				control.flags &= ~FLAG_CHECK;
			}
		}

		/* Use less ram when using STDOUT to store the temporary output
		 * file. */
		if (STDOUT && ((STDIN && DECOMPRESS) || !(DECOMPRESS || TEST_ONLY)))
			control.maxram = control.ramsize * 2 / 9;
		else
			control.maxram = control.ramsize / 3;
		if (BITS32) {
			/* Decrease usable ram size on 32 bits due to kernel /
			 * userspace split. Cannot allocate larger than a 1
			 * gigabyte chunk due to 32 bit signed long being
			 * used in alloc */
			control.usable_ram = MAX(control.ramsize - 900000000ll, 900000000ll);
			control.maxram = MIN(control.maxram, control.usable_ram);
			control.maxram = MIN(control.maxram, one_g);
		} else
			control.usable_ram = control.maxram;
		round_to_page(&control.maxram);

		show_summary();

		gettimeofday(&start_time, NULL);

		if (unlikely(STDIN && ENCRYPT))
			failure("Unable to work from STDIN while reading password\n");

		if (DECOMPRESS || TEST_ONLY)
			decompress_file(&control);
		else if (INFO)
			get_fileinfo(&control);
		else
			compress_file(&control);

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
