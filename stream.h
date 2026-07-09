/*
   Copyright (C) 2006-2016,2026 Con Kolivas
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

#ifndef LRZIP_STREAM_H
#define LRZIP_STREAM_H

#include "lrzip_private.h"
#include <pthread.h>

bool create_pthread(rzip_control *control, pthread_t *thread, pthread_attr_t * attr,
	void * (*start_routine)(void *), void *arg);
bool join_pthread(rzip_control *control, pthread_t th, void **thread_return);
bool init_mutex(rzip_control *control, pthread_mutex_t *mutex);
bool unlock_mutex(rzip_control *control, pthread_mutex_t *mutex);
bool lock_mutex(rzip_control *control, pthread_mutex_t *mutex);
ssize_t write_all(rzip_control *control, void *buf, i64 len);
ssize_t read_all(rzip_control *control, int fd, void *buf, i64 len);
i64 get_readseek(rzip_control *control, int fd);
bool prepare_streamout_threads(rzip_control *control);
bool wait_streamout_threads(rzip_control *control);
bool close_streamout_threads(rzip_control *control);
void *open_stream_out(rzip_control *control, int f, unsigned int n, i64 chunk_limit, char cbytes);
void *open_stream_in(rzip_control *control, int f, int n, char cbytes);
void flush_buffer(rzip_control *control, struct stream_info *sinfo, int stream);
void write_stream(rzip_control *control, void *ss, int streamno, uchar *p, i64 len);
i64 read_stream(rzip_control *control, void *ss, int streamno, uchar *p, i64 len);
int close_stream_out(rzip_control *control, void *ss);
int close_stream_in(rzip_control *control, void *ss);
ssize_t put_fdout(rzip_control *control, void *offset_buf, ssize_t ret);
bool patch_lrzc_c_size(rzip_control *control, int fd);
bool read_lrzc_header(rzip_control *control, int fd_in, i64 *c_size_out, i64 *u_size_out);

#endif
