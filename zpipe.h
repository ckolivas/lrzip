/*  zpipe streaming file compressor v1.0

(C) 2009, Ocarina Networks, Inc.
    Written by Matt Mahoney, matmahoney@yahoo.com, Sept. 29, 2009.

    LICENSE

    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License as
    published by the Free Software Foundation; either version 3 of
    the License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    General Public License for more details at
    Visit <http://www.gnu.org/copyleft/gpl.html>.
*/
#ifndef ZPIPE_H
#define ZPIPE_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

void zpipe_compress(FILE *in, FILE *out, FILE *msgout, long long int buf_len,
			       int progress, long thread);

void zpipe_decompress(FILE *in, FILE *out, FILE *msgout, long long int buf_len, int progress, long thread);

#ifdef __cplusplus
}
#endif

#endif
