/*
   Copyright (C) 2006-2016,2022 Con Kolivas
   Copyright (C) 2011 Peter Hyman
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

#ifndef LRZIP_MATCH_H
#define LRZIP_MATCH_H

#include "lrzip_private.h"
#include <string.h>

/* Compare only complete words inside the supplied span. memcpy permits
 * unaligned input without violating aliasing rules. */
static inline i64 match_forward(const uchar *a, const uchar *b, i64 n)
{
	i64 f = 0;

	while (n - f >= (i64)sizeof(unsigned long)) {
		unsigned long xa, xb, diff;

		memcpy(&xa, a + f, sizeof(xa));
		memcpy(&xb, b + f, sizeof(xb));
		diff = xa ^ xb;
		if (diff) {
#if __BYTE_ORDER == __LITTLE_ENDIAN
			return f + __builtin_ctzl(diff) / 8;
#else
			return f + __builtin_clzl(diff) / 8;
#endif
		}
		f += sizeof(xa);
	}
	while (f < n && a[f] == b[f])
		f++;
	return f;
}

/* a and b point just past the bytes to compare backwards. */
static inline i64 match_reverse(const uchar *a, const uchar *b, i64 n)
{
	i64 r = 0;

	while (n - r >= (i64)sizeof(unsigned long)) {
		unsigned long xa, xb, diff;

		memcpy(&xa, a - r - sizeof(xa), sizeof(xa));
		memcpy(&xb, b - r - sizeof(xb), sizeof(xb));
		diff = xa ^ xb;
		if (diff) {
#if __BYTE_ORDER == __LITTLE_ENDIAN
			return r + __builtin_clzl(diff) / 8;
#else
			return r + __builtin_ctzl(diff) / 8;
#endif
		}
		r += sizeof(xa);
	}
	while (r < n && a[-r - 1] == b[-r - 1])
		r++;
	return r;
}

/* Expand RLE match of period `offset` from the first `period` bytes of buf
 * out to `len` bytes (period == min(len, offset)). */
static inline void match_expand(uchar *buf, i64 period, i64 len)
{
	i64 pos = period;

	while (pos < len) {
		i64 n = MIN(len - pos, pos);

		/* The filled prefix contains whole periods until the final copy.
		 * Double it each time; source and destination never overlap. */
		memcpy(buf + pos, buf, (size_t)n);
		pos += n;
	}
}

#endif
