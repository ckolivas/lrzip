/*
   Copyright (C) 2006-2011 Con Kolivas
   Copyright (C) 2008, 2011 Peter Hyman
   Copyright (C) 1998 Andrew Tridgell

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

/*
  Utilities used in rzip

  tridge, June 1996
  */

/*
 * Realloc removed
 * Functions added
 *    read_config()
 * Peter Hyman, December 2008
 */

#include "rzip.h"

static const char *infile = NULL;
static char delete_infile = 0;
static const char *outfile = NULL;
static char delete_outfile = 0;
static FILE *outputfile = NULL;

void register_infile(const char *name, char delete)
{
	infile = name;
	delete_infile = delete;
}

void register_outfile(const char *name, char delete)
{
	outfile = name;
	delete_outfile = delete;
}

void register_outputfile(FILE *f)
{
	outputfile = f;
}

static void unlink_files(void)
{
	/* Delete temporary files generated for testing or faking stdio */
	if (outfile && delete_outfile)
		unlink(outfile);

	if (infile && delete_infile)
		unlink(infile);
}

static void fatal_exit(void)
{
	unlink_files();
	fprintf(outputfile, "Fatal error - exiting\n");
	fflush(outputfile);
	exit(1);
}

/* Failure when there is likely to be a meaningful error in perror */
void fatal(const char *format, ...)
{
	va_list ap;

	if (format) {
		va_start(ap, format);
		vfprintf(stderr, format, ap);
		va_end(ap);
	}

	perror(NULL);
	fatal_exit();
}

void failure(const char *format, ...)
{
	va_list ap;

	if (format) {
		va_start(ap, format);
		vfprintf(stderr, format, ap);
		va_end(ap);
	}

	fatal_exit();
}

void sighandler()
{
	unlink_files();
	exit(0);
}

void round_to_page(i64 *size)
{
	*size -= *size % PAGE_SIZE;
	if (unlikely(!*size))
		*size = PAGE_SIZE;
}

void read_config( struct rzip_control *control )
{
	/* check for lrzip.conf in ., $HOME/.lrzip and /etc/lrzip */

	FILE *fp;
	char *parameter;
	char *parametervalue;
	char *line, *s;
	char *HOME, *homeconf;

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
		return;

	/* if we get here, we have a file. read until no more. */

	while ((s = fgets(line, 255, fp)) != NULL) {
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
