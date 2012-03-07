/*
   Copyright (C) 2012 Con Kolivas

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
#undef NDEBUG
#include <stdlib.h>
#include <stdio.h>
#ifdef HAVE_STRING_H
# include <string.h>
#endif
#include <assert.h>
#ifdef HAVE_ERRNO_H
# include <errno.h>
#else
extern int errno;
#endif
#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif
#include <termios.h>
#include <Lrzip.h>

#define failure(...) do { \
	fprintf(stderr, __VA_ARGS__); \
	exit(1); \
} while (0)

static void usage(void)
{
	printf("lrzip version %s\n", PACKAGE_VERSION);
	printf("Copyright (C) Con Kolivas 2006-2011\n");
	printf("Based on rzip ");
	printf("Copyright (C) Andrew Tridgell 1998-2003\n\n");
	printf("Usage: lrzip [options] <file...>\n");
	printf("General options:\n");
	printf("     -c            check integrity of file written on decompression\n");
	printf("     -d            decompress\n");
	printf("     -e            password protected sha512/aes128 encryption on compression\n");
	printf("     -h|-?         show help\n");
	printf("     -H            display md5 hash integrity information\n");
	printf("     -i            show compressed file information\n");
	printf("     -q            don't show compression progress\n");
	printf("     -t            test compressed file integrity\n");
	printf("     -v[v]         Increase verbosity\n");
	printf("     -V            show version\n");
	printf("Options affecting output:\n");
	printf("     -D            delete existing files\n");
	printf("     -f            force overwrite of any existing files\n");
	printf("     -k            keep broken or damaged output files\n");
	printf("     -o filename   specify the output file name and/or path\n");
	printf("     -O directory  specify the output directory when -o is not used\n");
	printf("     -S suffix     specify compressed suffix (default '.lrz')\n");
	printf("Options affecting compression:\n");
	printf("     -b            bzip2 compression\n");
	printf("     -g            gzip compression using zlib\n");
	printf("     -l            lzo compression (ultra fast)\n");
	printf("     -n            no backend compression - prepare for other compressor\n");
	printf("     -z            zpaq compression (best, extreme compression, extremely slow)\n");
	printf("Low level options:\n");
	printf("     -L level      set lzma/bzip2/gzip compression level (1-9, default 7)\n");
	printf("     -N value      Set nice value to value (default 19)\n");
	printf("     -p value      Set processor count to override number of threads\n");
	printf("     -T            Disable LZO compressibility testing\n");
	printf("     -U            Use unlimited window size beyond ramsize (potentially much slower)\n");
	printf("     -w size       maximum compression window in hundreds of MB\n");
	printf("                   default chosen by heuristic dependent on ram and chosen compression\n");
	printf("\nLRZIP=NOCONFIG environment variable setting can be used to bypass lrzip.conf.\n");
	printf("TMP environment variable will be used for storage of temporary files when needed.\n");
	printf("TMPDIR may also be stored in lrzip.conf file.\n");
	printf("\nIf no filenames or \"-\" is specified, stdin/out will be used.\n");
}

static int get_pass(char *s, size_t slen)
{
	int len;

	memset(s, 0, slen);
	if (!fgets(s, slen, stdin)) {
		fprintf(stderr, "Failed to retrieve passphrase\n");
		return -1;
	}
	len = strlen(s);
	if (len > 0 && ('\r' ==  s[len - 1] || '\n' == s[len - 1]))
		s[len - 1] = '\0';
	if (len > 1 && ('\r' ==  s[len - 2] || '\n' == s[len - 2]))
		s[len - 2] = '\0';
	len = strlen(s);
	if (!len) {
		fprintf(stderr, "Empty passphrase\n");
		return -1;
	}
	return len;
}

static void pass_cb(void *data __UNUSED__, char *pass_string, size_t pass_len)
{
	int len;
	struct termios termios_p;
	/* Disable stdin echo to screen */
	tcgetattr(fileno(stdin), &termios_p);
	termios_p.c_lflag &= ~ECHO;
	tcsetattr(fileno(stdin), 0, &termios_p);

	printf("Enter passphrase: ");
	len = get_pass(pass_string, pass_len);
	printf("\n");

	if (len < 1) exit(1);

	termios_p.c_lflag |= ECHO;
	tcsetattr(fileno(stdin), 0, &termios_p);
}

static void mode_check(Lrzip *lr, Lrzip_Mode mode)
{
	Lrzip_Mode current = lrzip_mode_get(lr);
	if (current && (current != mode))
		failure("Can only use one of -l, -b, -g, -z or -n\n");
	lrzip_mode_set(lr, mode);
}

int main(int argc, char *argv[])
{
	Lrzip *lr;
	extern int optind;
	extern char *optarg;
	int64_t x;
	int c;
	bool get_hash = false;

	lrzip_init();
	lr = lrzip_new(LRZIP_MODE_NONE);
	assert(lr);
	lrzip_config_env(lr);
	lrzip_log_level_set(lr, LRZIP_LOG_LEVEL_PROGRESS);
	while ((c = getopt(argc, argv, "bcdDefghHiklL:nN:o:O:p:qS:tTUvVw:z?")) != -1) {
		switch (c) {
		case 'b':
			mode_check(lr, LRZIP_MODE_COMPRESS_BZIP2);
			break;
		case 'c':
			lrzip_flags_set(lr, lrzip_flags_get(lr) | LRZIP_FLAG_VERIFY);
			break;
		case 'd':
			mode_check(lr, LRZIP_MODE_DECOMPRESS);
			break;
		case 'D':
			lrzip_flags_set(lr, lrzip_flags_get(lr) | LRZIP_FLAG_REMOVE_SOURCE);
			break;
		case 'e':
			lrzip_flags_set(lr, lrzip_flags_get(lr) | LRZIP_FLAG_ENCRYPT);
			break;
		case 'f':
			lrzip_flags_set(lr, lrzip_flags_get(lr) | LRZIP_FLAG_REMOVE_DESTINATION);
			break;
		case 'g':
			mode_check(lr, LRZIP_MODE_COMPRESS_ZLIB);
			break;
		case 'h':
		case '?':
			usage();
			return -1;
		case 'H':
			get_hash = true;
			break;
		case 'i':
			mode_check(lr, LRZIP_MODE_INFO);
			break;
		case 'k':
			lrzip_flags_set(lr, lrzip_flags_get(lr) | LRZIP_FLAG_KEEP_BROKEN);
			break;
		case 'l':
			mode_check(lr, LRZIP_MODE_COMPRESS_LZO);
			break;
		case 'L':
			errno = 0;
			x = strtol(optarg, NULL, 10);
			if (errno || ((x < 1) || (x > 9)))
				failure("Invalid compression level (must be 1-9)\n");
			lrzip_compression_level_set(lr, (unsigned int)x);
			break;
		case 'n':
			mode_check(lr, LRZIP_MODE_COMPRESS_NONE);
			break;
		case 'N':
			errno = 0;
			x = strtol(optarg, NULL, 10);
			if (errno || (x < -20 || x > 19))
				failure("Invalid nice value (must be -20..19)\n");
			lrzip_nice_set(lr, x);
			break;
		case 'o':
			if (lrzip_outdir_get(lr))
				failure("Cannot have -o and -O together\n");
			if (!strcmp(optarg, "-"))
				lrzip_outfile_set(lr, stdout);
			else
				lrzip_outfilename_set(lr, optarg);
			break;
		case 'O':
			if (lrzip_outfilename_get(lr))	/* can't mix -o and -O */
				failure("Cannot have options -o and -O together\n");
			if (lrzip_outfile_get(lr))
				failure("Cannot specify an output directory when outputting to stdout\n");
			lrzip_outdir_set(lr, optarg);
			break;
		case 'p':
			errno = 0;
			x = strtol(optarg, NULL, 10);
			if (errno || (x < 1))
				failure("Must have at least one thread\n");
			lrzip_threads_set(lr, (unsigned int)x);
			break;
		case 'q':
			lrzip_log_level_set(lr, lrzip_log_level_get(lr) - 1);
			break;
		case 'S':
			if (lrzip_outfilename_get(lr))
				failure("Specified output filename already, can't specify an extension.\n");
			if (lrzip_outfile_get(lr))
				failure("Cannot specify a filename suffix when outputting to stdout\n");
			lrzip_suffix_set(lr, optarg);
			break;
		case 't':
			if (lrzip_outfilename_get(lr))
				failure("Cannot specify an output file name when just testing.\n");
			if (lrzip_flags_get(lr) & LRZIP_FLAG_REMOVE_SOURCE)
				failure("Doubt that you want to delete a file when just testing.\n");
			mode_check(lr, LRZIP_MODE_TEST);
			break;
		case 'T':
			lrzip_flags_set(lr, lrzip_flags_get(lr) | LRZIP_FLAG_DISABLE_LZO_CHECK);
			break;
		case 'U':
			lrzip_flags_set(lr, lrzip_flags_get(lr) | LRZIP_FLAG_UNLIMITED_RAM);
			break;
		case 'v':
			lrzip_log_level_set(lr, lrzip_log_level_get(lr) + 1);
			break;
		case 'V':
			printf("lrzip version %s\n", PACKAGE_VERSION);
			exit(0);
			break;
		case 'w':
			errno = 0;
			x = strtoll(optarg, NULL, 10);
			if (errno || (x < 1))
				failure("Invalid compression window '%s'!\n", optarg);
			lrzip_compression_window_max_set(lr, x);
			break;
		case 'z':
			mode_check(lr, LRZIP_MODE_COMPRESS_ZPAQ);
			break;
		}
	}
	/* LZMA is the default */
	if (!lrzip_mode_get(lr)) lrzip_mode_set(lr, LRZIP_MODE_COMPRESS_LZMA);
	argc -= optind, argv += optind;

	if (lrzip_outfilename_get(lr) && (argc > 1))
		failure("Cannot specify output filename with more than 1 file\n");

	if ((lrzip_flags_get(lr) & LRZIP_FLAG_UNLIMITED_RAM) && lrzip_compression_window_max_get(lr)) {
		fprintf(stderr, "If -U used, cannot specify a window size with -w.\n");
		lrzip_compression_window_max_set(lr, 0);
	}

	if (argc < 1) lrzip_file_add(lr, stdin);

	if ((lrzip_flags_get(lr) & LRZIP_FLAG_UNLIMITED_RAM) && lrzip_files_count(lr)) {
		fprintf(stderr, "Cannot have -U and stdin, unlimited mode disabled.\n");
		lrzip_flags_set(lr, lrzip_flags_get(lr) & ~LRZIP_FLAG_UNLIMITED_RAM);
	}

	/* If no output filename is specified, and we're using stdin,
	 * use stdout */
	if (lrzip_files_count(lr) && (!lrzip_outfilename_get(lr)))
		lrzip_outfile_set(lr, stdout);

	if (lrzip_flags_get(lr) & LRZIP_FLAG_VERIFY) {
		if (lrzip_mode_get(lr) != LRZIP_MODE_DECOMPRESS) {
			fprintf(stderr, "Can only check file written on decompression.\n");
			lrzip_flags_set(lr, lrzip_flags_get(lr) & ~LRZIP_FLAG_VERIFY);
		} else if (lrzip_outfile_get(lr)) {
			fprintf(stderr, "Can't check file written when writing to stdout. Checking disabled.\n");
			lrzip_flags_set(lr, lrzip_flags_get(lr) & ~LRZIP_FLAG_VERIFY);
		}
	}

	for (x = 0; x < argc; x++) {
		if (argv[x][0] != '-') {
			assert(lrzip_filename_add(lr, argv[x]));
			continue;
		}
		if (argv[x][1] == 0) {
			assert(lrzip_file_add(lr, stdin));
			continue;
		}
	}
	if (argc == 1) {
		if (!lrzip_files_count(lr)) lrzip_file_add(lr, stdin);
		if (lrzip_filenames_count(lr)) {
			if (!lrzip_outfilename_get(lr)) {
				char buf[4096];
				const char *infile;
				size_t len;

				infile = lrzip_filenames_get(lr)[0];
				len = strlen(infile);
				if (!strcmp(infile + len - 4, ".lrz"))
					strncat(buf, infile, len - 4);
				else
					snprintf(buf, sizeof(buf), "%s.out", infile);
				lrzip_outfilename_set(lr, buf);
			}
		} else if (!lrzip_outfile_get(lr)) lrzip_outfile_set(lr, stdout);
	}
	lrzip_log_stdout_set(lr, stdout);
	lrzip_log_stderr_set(lr, stderr);
	lrzip_pass_cb_set(lr, pass_cb, NULL);
	if (!lrzip_run(lr)) exit(1);
	if (get_hash) {
		const unsigned char *digest = lrzip_md5digest_get(lr);
		for (x = 0; x < 16; x++)
			fprintf(stdout, "%02x", digest[x] & 0xFF);
	}
	lrzip_free(lr);
	return 0;
}
