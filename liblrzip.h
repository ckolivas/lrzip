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

#ifndef LIBLRZIP_H
#define LIBLRZIP_H

#define FLAG_VERBOSE (FLAG_VERBOSITY | FLAG_VERBOSITY_MAX)
#define FLAG_NOT_LZMA (FLAG_NO_COMPRESS | FLAG_LZO_COMPRESS | FLAG_BZIP2_COMPRESS | FLAG_ZLIB_COMPRESS | FLAG_ZPAQ_COMPRESS)
#define LZMA_COMPRESS	(!(control->flags & FLAG_NOT_LZMA))

#define SHOW_PROGRESS	(control->flags & FLAG_SHOW_PROGRESS)
#define KEEP_FILES	(control->flags & FLAG_KEEP_FILES)
#define TEST_ONLY	(control->flags & FLAG_TEST_ONLY)
#define FORCE_REPLACE	(control->flags & FLAG_FORCE_REPLACE)
#define DECOMPRESS	(control->flags & FLAG_DECOMPRESS)
#define NO_COMPRESS	(control->flags & FLAG_NO_COMPRESS)
#define LZO_COMPRESS	(control->flags & FLAG_LZO_COMPRESS)
#define BZIP2_COMPRESS	(control->flags & FLAG_BZIP2_COMPRESS)
#define ZLIB_COMPRESS	(control->flags & FLAG_ZLIB_COMPRESS)
#define ZPAQ_COMPRESS	(control->flags & FLAG_ZPAQ_COMPRESS)
#define VERBOSE		(control->flags & FLAG_VERBOSE)
#define VERBOSITY	(control->flags & FLAG_VERBOSITY)
#define MAX_VERBOSE	(control->flags & FLAG_VERBOSITY_MAX)
#define STDIN		(control->flags & FLAG_STDIN)
#define STDOUT		(control->flags & FLAG_STDOUT)
#define INFO		(control->flags & FLAG_INFO)
#define UNLIMITED	(control->flags & FLAG_UNLIMITED)
#define HASH_CHECK	(control->flags & FLAG_HASH)
#define HAS_MD5		(control->flags & FLAG_MD5)
#define CHECK_FILE	(control->flags & FLAG_CHECK)
#define KEEP_BROKEN	(control->flags & FLAG_KEEP_BROKEN)
#define LZO_TEST	(control->flags & FLAG_THRESHOLD)

#define print_output(format, args...)	do {\
	fprintf(control->msgout, format, ##args);	\
	fflush(control->msgout);	\
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

#endif
