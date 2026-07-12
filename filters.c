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
/* Reversible prefilters: x86/arm64 branch conversion and byte delta.
 * Branch conversion turns relative call/jump targets into absolute ones so
 * that repeated calls to the same target become repeated byte sequences;
 * delta subtracts the byte N before, turning slowly changing sampled data
 * into small values. All are exact inverses of themselves. */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <string.h>
#include <stdint.h>
#include <inttypes.h>

#include "filters.h"
#include "util.h"
#include "lzma/C/LzmaLib.h"
#include "lzma/C/Bra.h"
#include "lzma/C/Delta.h"

void lrz_filter_stream_init(struct lrz_filter_stream *fs, int kind, bool encode)
{
	fs->kind = kind;
	fs->encode = encode;
	fs->x86_state = Z7_BRANCH_CONV_ST_X86_STATE_INIT_VAL;
	fs->pc = 0;
}

i64 lrz_filter_stream_conv(struct lrz_filter_stream *fs, uchar *buf, i64 len, bool last)
{
	Byte delta_state[DELTA_STATE_SIZE];
	uchar *stop;
	i64 done;

	switch (fs->kind) {
	case LRZ_FILTER_X86:
		if (fs->encode)
			stop = z7_BranchConvSt_X86_Enc(buf, (SizeT)len, (UInt32)fs->pc, &fs->x86_state);
		else
			stop = z7_BranchConvSt_X86_Dec(buf, (SizeT)len, (UInt32)fs->pc, &fs->x86_state);
		done = last ? len : stop - buf;
		break;
	case LRZ_FILTER_ARM64:
		if (fs->encode)
			stop = z7_BranchConv_ARM64_Enc(buf, (SizeT)len, (UInt32)fs->pc);
		else
			stop = z7_BranchConv_ARM64_Dec(buf, (SizeT)len, (UInt32)fs->pc);
		done = last ? len : stop - buf;
		break;
	case LRZ_FILTER_DELTA1 ... LRZ_FILTER_DELTA4:
		/* Delta carries its state internally per call; for the
		 * streaming case the caller must pass whole regions, so it
		 * is only supported one shot here. */
		Delta_Init(delta_state);
		if (fs->encode)
			Delta_Encode(delta_state, fs->kind - LRZ_FILTER_DELTA1 + 1, buf, (SizeT)len);
		else
			Delta_Decode(delta_state, fs->kind - LRZ_FILTER_DELTA1 + 1, buf, (SizeT)len);
		done = len;
		break;
	default:
		done = len;
		break;
	}
	fs->pc += done;
	return done;
}

void lrz_filter_convert_mem(uchar *buf, i64 len, int kind, bool encode)
{
	struct lrz_filter_stream fs;

	lrz_filter_stream_init(&fs, kind, encode);
	lrz_filter_stream_conv(&fs, buf, len, true);
}

int lrz_filter_trial(rzip_control *control, uchar *sample_area, i64 avail)
{
	unsigned char props[5];
	size_t prop_size = 5, plain_len, best_len, trial_len, sample;
	int best = LRZ_FILTER_NONE, ret, i;
	uchar *pristine, *copy, *out;
	i64 sample_ofs;

	if (avail < (1 << 20))
		return LRZ_FILTER_NONE;	/* not worth the trials on small data */

	sample = MIN((size_t)avail, (size_t)(1 << 21));

	pristine = malloc(sample);
	copy = malloc(sample);
	out = malloc(sample);
	if (unlikely(!pristine || !copy || !out))
		goto out;

	/* Sample from the middle, away from headers, 4 byte aligned so the
	 * arm64 trial sees the same word phase as the full conversion. */
	sample_ofs = ((avail - (i64)sample) / 2) & ~(i64)3;
	memcpy(pristine, sample_area + sample_ofs, sample);

	plain_len = sample;
	ret = LzmaCompress(out, &plain_len, pristine, sample, props, &prop_size,
			   1, 1 << 20, -1, -1, -1, -1, 1);
	if (ret != SZ_OK && ret != SZ_ERROR_OUTPUT_EOF)
		goto out;

	/* Candidates must win by at least this much versus plain so that
	 * borderline data stays unfiltered */
	best_len = plain_len - (plain_len / 64);

	for (i = LRZ_FILTER_X86; i <= LRZ_FILTER_MAX; i++) {
		memcpy(copy, pristine, sample);
		lrz_filter_convert_mem(copy, sample, i, true);
		trial_len = sample;
		prop_size = 5;
		ret = LzmaCompress(out, &trial_len, copy, sample, props, &prop_size,
				   1, 1 << 20, -1, -1, -1, -1, 1);
		if (ret != SZ_OK)
			continue;
		if (trial_len < best_len) {
			best_len = trial_len;
			best = i;
		}
	}
	print_maxverbose("Filter trial: plain %zu best %zu, chose filter %d\n",
			 plain_len, best_len, best);
out:
	dealloc(pristine);
	dealloc(copy);
	dealloc(out);
	return best;
}

int lrz_stream_filter_pick(rzip_control *control, uchar *buf, i64 len)
{
	if (!control->filter_mode)
		return LRZ_FILTER_NONE;
	if (control->filter_mode > 0)
		return control->filter_mode;
	return lrz_filter_trial(control, buf, len);
}
