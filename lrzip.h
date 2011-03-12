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
#ifndef LRZIP_H
#define LRZIP_H

#include "lrzip_private.h"

void write_magic(rzip_control *control, int fd_in, int fd_out);
void read_magic(rzip_control *control, int fd_in, i64 *expected_size);
void preserve_perms(rzip_control *control, int fd_in, int fd_out);
int open_tmpoutfile(rzip_control *control);
void dump_tmpoutfile(rzip_control *control, int fd_out);
int open_tmpinfile(rzip_control *control);
void read_tmpinfile(rzip_control *control, int fd_in);
void decompress_file(rzip_control *control);
void get_header_info(rzip_control *control, int fd_in, uchar *ctype, i64 *c_len, i64 *u_len, i64 *last_head);
void get_fileinfo(rzip_control *control);
void compress_file(rzip_control *control);
void write_stdout_header(rzip_control *control, int fd_in);
void flush_stdout(rzip_control *control);
#endif
