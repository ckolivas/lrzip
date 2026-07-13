/*
   Copyright (C) 2026 Con Kolivas

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
#ifndef LRZIP_FILTERS_H
#define LRZIP_FILTERS_H

#include "lrzip_private.h"

/* Reversible data transforms applied before compression, enabled by
 * --filter. The block layer (stream.c) uses all of them per backend block,
 * recorded in the block type byte; the chunk layer (rzip.c / runzip.c)
 * uses only the branch converters on whole chunks before rzip, recorded
 * in the v0.8 chunk header prefilter byte. */
#define LRZ_FILTER_NONE 0
#define LRZ_FILTER_X86 1
#define LRZ_FILTER_ARM64 2
#define LRZ_FILTER_DELTA1 3	/* delta distance 1 */
#define LRZ_FILTER_DELTA2 4
#define LRZ_FILTER_DELTA3 5
#define LRZ_FILTER_DELTA4 6
#define LRZ_FILTER_MAX LRZ_FILTER_DELTA4
/* Highest filter valid in a chunk header (branch converters only) */
#define LRZ_CHUNK_FILTER_MAX LRZ_FILTER_ARM64

/* Streaming converter state so a filter can be applied or reversed over a
 * region processed in slices. Feed slices in order; all but the final call
 * may leave up to 4 unconverted tail bytes for the caller to carry into
 * the next slice. */
struct lrz_filter_stream {
	int kind;
	bool encode;
	u32 x86_state;
	i64 pc;		/* position of buf[0] within the filtered region */
};

void lrz_filter_stream_init(struct lrz_filter_stream *fs, int kind, bool encode);
/* Convert buf[0..len) in place; returns the number of bytes fully
 * converted (== len when last is true). */
i64 lrz_filter_stream_conv(struct lrz_filter_stream *fs, uchar *buf, i64 len, bool last);
/* One shot in place conversion of a whole region */
void lrz_filter_convert_mem(uchar *buf, i64 len, int kind, bool encode);
/* Trial compress a sample plain and converted with each candidate filter;
 * returns the winning LRZ_FILTER_* or LRZ_FILTER_NONE. */
int lrz_filter_trial(rzip_control *control, uchar *sample_area, i64 avail);
/* Filter for one backend block under control->filter_mode: forced kind,
 * trial selection for auto, or LRZ_FILTER_NONE when filtering is off. */
int lrz_stream_filter_pick(rzip_control *control, uchar *buf, i64 len);
/* Decide whether branch converting a whole chunk before rzip pays off,
 * sampling multiple regions. Returns LRZ_FILTER_NONE/X86/ARM64. */
int lrz_chunk_filter_pick(rzip_control *control, uchar *buf, i64 len);

#endif
