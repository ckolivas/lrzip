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

/* ---- Chunk level filter detection ----
 *
 * Local trial compression cannot see why branch conversion helps a whole
 * chunk: for x86 the gain comes from long range duplicate code sequences
 * that only become identical after conversion (rzip then removes them),
 * while locally the converted bytes are slightly HIGHER entropy. So the
 * x86 signal counts duplicate 32 byte windows across large samples before
 * and after conversion; more duplicates after conversion means rzip will
 * find more, fewer means conversion would break existing duplicates (for
 * example the same binary stored twice), which doubles as protection for
 * dedup heavy archives. arm64 gains show up at the entropy level instead
 * (4 byte call words), so it uses net trial compression summed over
 * several regions, guarded by an instruction density prescreen. */

#define CHUNK_SAMPLE_REGIONS 4
#define CHUNK_SAMPLE_SIZE (16 * 1024 * 1024)
#define DUP_TAB_BITS 23

/* Content defined anchors: a gear hash selects ~1 in 64 positions from the
 * content itself, so identical data at ANY pair of offsets yields the same
 * anchors and windows. Fixed stride sampling would only see duplicates
 * whose distance happens to be a stride multiple. */
static uint64_t gear[256];

static void gear_init(void)
{
	uint64_t x = 0x9E3779B97F4A7C15ULL;
	int i;

	if (gear[0])
		return;
	for (i = 0; i < 256; i++) {
		uint64_t z;

		/* splitmix64 */
		x += 0x9E3779B97F4A7C15ULL;
		z = x;
		z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
		z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
		gear[i] = z ^ (z >> 31);
	}
}

static void dup_count_feed(uint64_t *tab, const uchar *buf, i64 len, i64 *dups)
{
	uint64_t g = 0;
	i64 i;

	gear_init();
	for (i = 0; i + 32 <= len; i++) {
		g = (g << 1) + gear[buf[i]];
		if (g & 0x3F)
			continue;
		{
			uint64_t h = 1469598103934665603ULL;
			int j;

			for (j = 0; j < 32; j++) {
				h ^= buf[i + j];
				h *= 1099511628211ULL;
			}
			if (tab[(h >> 40) & ((1u << DUP_TAB_BITS) - 1)] == h)
				(*dups)++;
			else
				tab[(h >> 40) & ((1u << DUP_TAB_BITS) - 1)] = h;
		}
	}
}

/* Fraction (permille) of aligned words that look like arm64 BL calls */
static int arm64_bl_permille(const uchar *buf, i64 len)
{
	i64 words = len / 4, i, bl = 0;

	if (!words)
		return 0;
	for (i = 0; i < len - 3; i += 4) {
		/* BL: top 6 bits 100101 (0x94..0x97 in byte 3, LE) */
		if ((buf[i + 3] & 0xFC) == 0x94)
			bl++;
	}
	return (int)(bl * 1000 / words);
}

int lrz_chunk_filter_pick(rzip_control *control, uchar *buf, i64 len)
{
	i64 region, spacing, ofs[CHUNK_SAMPLE_REGIONS];
	i64 dup_plain = 0, dup_conv = 0;
	uint64_t *tab;
	uchar *copy, *out;
	int i, ret = LRZ_FILTER_NONE, bl_permille = 0;

	if (len < (2 << 20))
		return LRZ_FILTER_NONE;

	region = MIN((i64)CHUNK_SAMPLE_SIZE, len / CHUNK_SAMPLE_REGIONS);
	region &= ~(i64)3;
	spacing = (len - region) / (CHUNK_SAMPLE_REGIONS - 1);
	for (i = 0; i < CHUNK_SAMPLE_REGIONS; i++)
		ofs[i] = (spacing * i) & ~(i64)3;

	tab = calloc(1u << DUP_TAB_BITS, sizeof(*tab));
	copy = malloc(region);
	if (unlikely(!tab || !copy)) {
		dealloc(tab);
		dealloc(copy);
		return LRZ_FILTER_NONE;
	}

	/* Duplicate windows in the plain samples */
	for (i = 0; i < CHUNK_SAMPLE_REGIONS; i++) {
		dup_count_feed(tab, buf + ofs[i], region, &dup_plain);
		bl_permille += arm64_bl_permille(buf + ofs[i], region);
	}
	bl_permille /= CHUNK_SAMPLE_REGIONS;

	/* Duplicate windows after x86 conversion, converted with the same
	 * pc base each region would have inside the whole converted chunk */
	memset(tab, 0, (size_t)(1u << DUP_TAB_BITS) * sizeof(*tab));
	for (i = 0; i < CHUNK_SAMPLE_REGIONS; i++) {
		struct lrz_filter_stream fs;

		memcpy(copy, buf + ofs[i], region);
		lrz_filter_stream_init(&fs, LRZ_FILTER_X86, true);
		fs.pc = ofs[i];
		lrz_filter_stream_conv(&fs, copy, region, true);
		dup_count_feed(tab, copy, region, &dup_conv);
	}

	print_maxverbose("Chunk filter probe: dup windows plain %"PRId64" x86 %"PRId64", arm64 bl %d/1000\n",
			 dup_plain, dup_conv, bl_permille);

	/* x86: conversion must create clearly more long range duplicates */
	if (dup_plain && dup_conv > dup_plain + dup_plain / 32) {
		ret = LRZ_FILTER_X86;
		goto out_free;
	}

	/* The arm64 signal below is net trial compression over the samples;
	 * only extrapolate it when the samples cover a meaningful fraction
	 * of the chunk. Beyond that the unsampled volume dominates and
	 * whole chunk rzip effects the samples cannot see take over:
	 * a 417MB debian /usr tree regressed 3% archive-wide despite a 2%
	 * win on the samples, while a 107MB rootfs with the same sample win
	 * gained 2.2%. The x86 duplicate window signal above counts matches
	 * across all samples and stays robust at any size. */
	if (len > (i64)CHUNK_SAMPLE_REGIONS * CHUNK_SAMPLE_SIZE * 3)
		goto out_free;

	/* arm64: needs plausible code density, then a net win on trial
	 * compression summed over the regions, and must not reduce the
	 * long range duplicates that rzip relies on (measured with the
	 * arm64 conversion, not the x86 one).
	 * Random data shows ~15/1000 bl density by chance, real aarch64
	 * code 30-60. */
	if (bl_permille >= 18) {
		i64 dup_a64 = 0;

		memset(tab, 0, (size_t)(1u << DUP_TAB_BITS) * sizeof(*tab));
		for (i = 0; i < CHUNK_SAMPLE_REGIONS; i++) {
			struct lrz_filter_stream fs;

			memcpy(copy, buf + ofs[i], region);
			lrz_filter_stream_init(&fs, LRZ_FILTER_ARM64, true);
			fs.pc = ofs[i];
			lrz_filter_stream_conv(&fs, copy, region, true);
			dup_count_feed(tab, copy, region, &dup_a64);
		}
		print_maxverbose("Chunk filter probe: arm64 dup windows %"PRId64"\n", dup_a64);
		if (dup_a64 < dup_plain - dup_plain / 100)
			goto out_free;
	}
	if (bl_permille >= 18) {
		unsigned char props[5];
		size_t prop_size, plen, clen;
		i64 plain_total = 0, conv_total = 0;
		int lret;

		out = malloc(region + region / 2);
		if (unlikely(!out))
			goto out_free;
		for (i = 0; i < CHUNK_SAMPLE_REGIONS; i++) {
			struct lrz_filter_stream fs;

			plen = region + region / 2;
			prop_size = 5;
			lret = LzmaCompress(out, &plen, buf + ofs[i], region,
					    props, &prop_size, 2, 1 << 22,
					    -1, -1, -1, -1, 1);
			if (lret != SZ_OK)
				break;
			memcpy(copy, buf + ofs[i], region);
			lrz_filter_stream_init(&fs, LRZ_FILTER_ARM64, true);
			fs.pc = ofs[i];
			lrz_filter_stream_conv(&fs, copy, region, true);
			clen = region + region / 2;
			prop_size = 5;
			lret = LzmaCompress(out, &clen, copy, region,
					    props, &prop_size, 2, 1 << 22,
					    -1, -1, -1, -1, 1);
			if (lret != SZ_OK)
				break;
			plain_total += plen;
			conv_total += clen;
		}
		dealloc(out);
		print_maxverbose("Chunk filter probe: arm64 trial plain %"PRId64" conv %"PRId64"\n",
				 plain_total, conv_total);
		if (plain_total && conv_total &&
		    conv_total < plain_total - plain_total / 64)
			ret = LRZ_FILTER_ARM64;
	}

out_free:
	dealloc(tab);
	dealloc(copy);
	return ret;
}
