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

#include <stdbool.h>
#include <stdio.h>
#ifdef _WIN32
# include <stddef.h>
#else
# include <inttypes.h>
#endif

typedef struct Lrzip Lrzip;

typedef enum {
	LRZIP_LOG_LEVEL_ERROR = 0,
	LRZIP_LOG_LEVEL_INFO,
	LRZIP_LOG_LEVEL_PROGRESS,
	LRZIP_LOG_LEVEL_VERBOSE,
	LRZIP_LOG_LEVEL_DEBUG
} Lrzip_Log_Level;

typedef enum {
	LRZIP_MODE_NONE = 0,
	LRZIP_MODE_INFO,
	LRZIP_MODE_TEST,
	LRZIP_MODE_DECOMPRESS,
	LRZIP_MODE_COMPRESS_NONE,
	LRZIP_MODE_COMPRESS_LZO,
	LRZIP_MODE_COMPRESS_ZLIB,
	LRZIP_MODE_COMPRESS_BZIP2,
	LRZIP_MODE_COMPRESS_LZMA,
	LRZIP_MODE_COMPRESS_ZPAQ
} Lrzip_Mode;

typedef enum {
	LRZIP_FLAG_REMOVE_SOURCE = (1 << 0),
	LRZIP_FLAG_REMOVE_DESTINATION = (1 << 1),
	LRZIP_FLAG_KEEP_BROKEN = (1 << 2),
	LRZIP_FLAG_VERIFY = (1 << 3),
	LRZIP_FLAG_DISABLE_LZO_CHECK = (1 << 4),
	LRZIP_FLAG_UNLIMITED_RAM = (1 << 5),
	LRZIP_FLAG_ENCRYPT = (1 << 6)
} Lrzip_Flag;

typedef void (*Lrzip_Info_Cb)(void *data, int pct, int chunk_pct);
typedef void (*Lrzip_Log_Cb)(void *data, unsigned int level, unsigned int line, const char *file, const char *format, va_list args);
typedef void (*Lrzip_Password_Cb)(void *, char **, size_t);

bool lrzip_init(void);
void lrzip_config_env(Lrzip *lr);
void lrzip_free(Lrzip *lr);
Lrzip *lrzip_new(Lrzip_Mode mode);
Lrzip_Mode lrzip_mode_get(Lrzip *lr);
bool lrzip_mode_set(Lrzip *lr, Lrzip_Mode mode);
bool lrzip_compression_level_set(Lrzip *lr, unsigned int level);
unsigned int lrzip_compression_level_get(Lrzip *lr);
void lrzip_flags_set(Lrzip *lr, unsigned int flags);
unsigned int lrzip_flags_get(Lrzip *lr);
void lrzip_nice_set(Lrzip *lr, int nice);
int lrzip_nice_get(Lrzip *lr);
void lrzip_threads_set(Lrzip *lr, unsigned int threads);
unsigned int lrzip_threads_get(Lrzip *lr);
void lrzip_compression_window_max_set(Lrzip *lr, int64_t size);
int64_t lrzip_compression_window_max_get(Lrzip *lr);
unsigned int lrzip_files_count(Lrzip *lr);
unsigned int lrzip_filenames_count(Lrzip *lr);
FILE **lrzip_files_get(Lrzip *lr);
char **lrzip_filenames_get(Lrzip *lr);
bool lrzip_file_add(Lrzip *lr, FILE *file);
bool lrzip_file_del(Lrzip *lr, FILE *file);
FILE *lrzip_file_pop(Lrzip *lr);
void lrzip_files_clear(Lrzip *lr);
bool lrzip_filename_add(Lrzip *lr, const char *file);
bool lrzip_filename_del(Lrzip *lr, const char *file);
const char *lrzip_filename_pop(Lrzip *lr);
void lrzip_filenames_clear(Lrzip *lr);
void lrzip_suffix_set(Lrzip *lr, const char *suffix);
const char *lrzip_suffix_get(Lrzip *lr);
void lrzip_outdir_set(Lrzip *lr, const char *dir);
const char *lrzip_outdir_get(Lrzip *lr);
void lrzip_outfile_set(Lrzip *lr, FILE *file);
FILE *lrzip_outfile_get(Lrzip *lr);
void lrzip_outfilename_set(Lrzip *lr, const char *file);
const char *lrzip_outfilename_get(Lrzip *lr);
const unsigned char *lrzip_md5digest_get(Lrzip *lr);
bool lrzip_run(Lrzip *lr);
void lrzip_log_level_set(Lrzip *lr, int level);
int lrzip_log_level_get(Lrzip *lr);
void lrzip_log_cb_set(Lrzip *lr, Lrzip_Log_Cb cb, void *log_data);
void lrzip_log_stdout_set(Lrzip *lr, FILE *out);
FILE *lrzip_log_stdout_get(Lrzip *lr);
void lrzip_log_stderr_set(Lrzip *lr, FILE *err);
FILE *lrzip_log_stderr_get(Lrzip *lr);
void lrzip_pass_cb_set(Lrzip *lr, Lrzip_Password_Cb cb, void *data);
void lrzip_info_cb_set(Lrzip *lr, Lrzip_Info_Cb cb, void *data);
#endif

