/*
   Copyright (C) 2006-2016,2018,2022,2026 Con Kolivas
   Copyright (C) 1998 Andrew Tridgell

   Modified to use flat hash, memory limit and variable hash culling
   by Rusty Russell copyright (C) 2003.

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
/* rzip compression algorithm */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#ifdef HAVE_SYS_MMAN_H
# include <sys/mman.h>
#endif
#include <sys/statvfs.h>
#ifdef HAVE_SYS_STAT_H
# include <sys/stat.h>
#endif
#ifdef HAVE_SYS_TIME_H
# include <sys/time.h>
#endif
#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif
#include <lzo/lzoconf.h>
#include <lzo/lzo1x.h>
#ifdef HAVE_ERRNO_H
# include <errno.h>
#endif
#ifdef HAVE_ENDIAN_H
# include <endian.h>
#elif HAVE_SYS_ENDIAN_H
# include <sys/endian.h>
#endif
#ifdef HAVE_ARPA_INET_H
# include <arpa/inet.h>
#endif
#include <inttypes.h>
#include <string.h>

#include "md5.h"
#include "stream.h"
#include "util.h"
#include "filters.h"
#include "lrzip_core.h"

#ifndef MAP_ANONYMOUS
# define MAP_ANONYMOUS MAP_ANON
#endif

#define CHUNK_MULTIPLE (100 * 1024 * 1024)
#define CKSUM_CHUNK 1024*1024
#define GREAT_MATCH 1024
#define MINIMUM_MATCH 31

/* Hash table works as follows.  We start by throwing tags at every
 * offset into the table.  As it fills, we start eliminating tags
 * which don't have lower bits set to one (ie. first we eliminate all
 * even tags, then all tags divisible by four, etc.).  This ensures
 * that on average, all parts of the file are covered by the hash, if
 * sparsely.
 *
 * Slot size is 8 bytes (uint32 tag + uint32 offset) when the chunk fits
 * in 32 bits, else 16-byte wide entries. Table length targets the same
 * slot count rzip-2.1 used for a given mb_used (computed as if entries
 * were 8 bytes), capped by chunk size so small files do not build huge
 * tables.
 */

/* Levels control hashtable size. mb_used is the historical "megabytes of
 * hash" knob from rzip (8-byte entries); we map it to a slot count and
 * allocate with the real entry size. */
static struct level {
	unsigned long mb_used;
	unsigned initial_freq;
	unsigned max_chain_len;
} levels[10] = {
	{ 1, 4, 1 },
	{ 2, 4, 2 },
	{ 4, 4, 2 },
	{ 8, 4, 2 },
	{ 16, 4, 3 },
	{ 32, 4, 4 },
	{ 32, 2, 6 },
	{ 64, 1, 16 }, /* More MB makes sense, but need bigger test files */
	{ 64, 1, 32 },
	{ 64, 1, 128 },
};

#define HASH_ENTRY_SIZE_NARROW	8
/* rzip-2.1 entry was u32+u32 = 8 bytes; use that for target slot density */

static void remap_low_sb(rzip_control *control, struct sliding_buffer *sb)
{
	i64 new_offset;

	new_offset = sb->offset_search;
	round_to_page(&new_offset);
	print_maxverbose("Sliding main buffer to offset %"PRId64"\n", new_offset);
	if (unlikely(munmap(sb->buf_low, sb->size_low)))
		failure("Failed to munmap in remap_low_sb\n");
	if (new_offset + sb->size_low > sb->orig_size)
		sb->size_low = sb->orig_size - new_offset;
	sb->offset_low = new_offset;
	sb->buf_low = (uchar *)mmap(sb->buf_low, sb->size_low, PROT_READ, MAP_SHARED, sb->fd, sb->orig_offset + sb->offset_low);
	if (unlikely(sb->buf_low == MAP_FAILED))
		failure("Failed to re mmap in remap_low_sb\n");
}

static inline void remap_high_sb(rzip_control *control, struct sliding_buffer *sb, i64 p)
{
	if (unlikely(munmap(sb->buf_high, sb->size_high)))
		failure("Failed to munmap in remap_high_sb\n");
	sb->size_high = sb->high_length; /* In case we shrunk it when we hit the end of the file */
	sb->offset_high = p;
	/* Make sure offset is rounded to page size of total offset */
	sb->offset_high -= (sb->offset_high + sb->orig_offset) % control->page_size;
	if (unlikely(sb->offset_high + sb->size_high > sb->orig_size))
		sb->size_high = sb->orig_size - sb->offset_high;
	sb->buf_high = (uchar *)mmap(sb->buf_high, sb->size_high, PROT_READ, MAP_SHARED, sb->fd, sb->orig_offset + sb->offset_high);
	if (unlikely(sb->buf_high == MAP_FAILED))
		failure("Failed to re mmap in remap_high_sb\n");
}

/* We use a "sliding mmap" to effectively read more than we can fit into the
 * compression window. This is done by using a maximally sized lower mmap at
 * the beginning of the block which slides up once the hash search moves beyond
 * it, and a 64k mmap block that slides up and down as is required for any
 * offsets outside the range of the lower one. This is much slower than mmap
 * but makes it possible to have unlimited sized compression windows. */

/* True if [p, p+len) lies entirely in the low map (len may be 0). */
static inline int sliding_in_low(const struct sliding_buffer *sb, i64 p, i64 len)
{
	return p >= sb->offset_low && len >= 0 &&
	       p + len <= sb->offset_low + sb->size_low;
}

static uchar *sliding_get_sb(rzip_control *control, i64 p)
{
	struct sliding_buffer *sb = &control->sb;
	i64 sbo, sbs;

	sbo = sb->offset_low;
	sbs = sb->size_low;
	if (p >= sbo && p < sbo + sbs)
		return sb->buf_low + (p - sbo);
	sbo = sb->offset_high;
	sbs = sb->size_high;
	if (p >= sbo && p < sbo + sbs)
		return sb->buf_high + (p - sbo);
	/* p is not within the low or high buffer range */
	remap_high_sb(control, sb, p);
	return sb->buf_high + (p - sb->offset_high);
}

/* Ensure p is mapped; return pointer and contiguous forward length from p. */
static inline uchar *sliding_map_fwd(rzip_control *control, i64 p, i64 *contig)
{
	struct sliding_buffer *sb = &control->sb;
	i64 sbo, sbs;

	sbo = sb->offset_low;
	sbs = sb->size_low;
	if (p >= sbo && p < sbo + sbs) {
		*contig = sbs - (p - sbo);
		return sb->buf_low + (p - sbo);
	}
	sbo = sb->offset_high;
	sbs = sb->size_high;
	if (p >= sbo && p < sbo + sbs) {
		*contig = sbs - (p - sbo);
		return sb->buf_high + (p - sbo);
	}
	remap_high_sb(control, sb, p);
	*contig = sb->size_high - (p - sb->offset_high);
	return sb->buf_high + (p - sb->offset_high);
}

/* Contiguous bytes available strictly before p (for reverse matching). */
static inline i64 sliding_contig_before(rzip_control *control, i64 p)
{
	struct sliding_buffer *sb = &control->sb;
	i64 pm1, sbo, sbs;

	if (p <= 0)
		return 0;
	pm1 = p - 1;
	sbo = sb->offset_low;
	sbs = sb->size_low;
	if (pm1 >= sbo && pm1 < sbo + sbs)
		return pm1 - sbo + 1;
	sbo = sb->offset_high;
	sbs = sb->size_high;
	if (pm1 >= sbo && pm1 < sbo + sbs)
		return pm1 - sbo + 1;
	/* Map the byte we need, then recompute. */
	sliding_get_sb(control, pm1);
	sbo = sb->offset_high;
	sbs = sb->size_high;
	if (pm1 >= sbo && pm1 < sbo + sbs)
		return pm1 - sbo + 1;
	sbo = sb->offset_low;
	sbs = sb->size_low;
	if (pm1 >= sbo && pm1 < sbo + sbs)
		return pm1 - sbo + 1;
	return 1;
}

/* The length of continuous range of the sliding buffer starting at P.
 * P must already be mapped (via sliding_get_sb / sliding_map_fwd). */
static inline i64 sliding_get_sb_range(rzip_control *control, i64 p)
{
	struct sliding_buffer *sb = &control->sb;
	i64 sbo, sbs;

	sbo = sb->offset_low;
	sbs = sb->size_low;
	if (p >= sbo && p < sbo + sbs)
		return sbs - (p - sbo);
	sbo = sb->offset_high;
	sbs = sb->size_high;
	if (likely(p >= sbo && p < sbo + sbs))
		return sbs - (p - sbo);

	fatal_return(("sliding_get_sb_range: the pointer is out of range\n"), 0);
}

/* Since the sliding get_sb only allows us to access one byte at a time, we
 * do the same as we did with get_sb with the memcpy since one memcpy is much
 * faster than numerous memcpys 1 byte at a time */
static void single_mcpy(rzip_control *control, unsigned char *buf, i64 offset, i64 len)
{
	memcpy(buf, control->sb.buf_low + offset, len);
}

static void sliding_mcpy(rzip_control *control, unsigned char *buf, i64 offset, i64 len)
{
	i64 n = 0;

	while (n < len) {
		uchar *srcbuf = sliding_get_sb(control, offset + n);
		i64 m = MIN(sliding_get_sb_range(control, offset + n), len - n);

		memcpy(buf + n, srcbuf, m);
		n += m;
	}
}

/* ---- Stream 0 (rzip control) write batching ---- */
static void s0_flush(rzip_control *control, struct rzip_state *st)
{
	if (st->s0_len) {
		write_stream(control, st->ss, 0, st->s0_buf, st->s0_len);
		st->s0_len = 0;
	}
}

static void s0_write(rzip_control *control, struct rzip_state *st,
		     const uchar *p, unsigned len)
{
	while (len) {
		unsigned n = RZIP_S0_BUFSIZE - st->s0_len;

		if (n > len)
			n = len;
		memcpy(st->s0_buf + st->s0_len, p, n);
		st->s0_len += n;
		p += n;
		len -= n;
		if (st->s0_len == RZIP_S0_BUFSIZE)
			s0_flush(control, st);
	}
}

static inline void put_u8(rzip_control *control, struct rzip_state *st, uchar b)
{
	s0_write(control, st, &b, 1);
}

/* Put a variable length of bytes dependant on how big the chunk is */
static void put_vchars(rzip_control *control, struct rzip_state *st, i64 s, int length)
{
	s = htole64(s);
	s0_write(control, st, (uchar *)&s, (unsigned)length);
}

static void put_header(rzip_control *control, struct rzip_state *st, uchar head, i64 len)
{
	put_u8(control, st, head);
	put_vchars(control, st, len, 2);
}

static inline void put_match(rzip_control *control, struct rzip_state *st,
			     i64 p, i64 offset, i64 len)
{
	do {
		i64 ofs;
		i64 n = len;
		if (n > 0xFFFF)
			n = 0xFFFF;

		ofs = (p - offset);
		put_header(control, st, 1, n);
		put_vchars(control, st, ofs, st->chunk_bytes);
		st->stats.matches++;
		st->stats.match_bytes += n;
		len -= n;
		p += n;
		offset += n;
	} while (len);
}

/* write some data to a stream mmap encoded. Return -1 on failure */
static inline void write_sbstream(rzip_control *control, void *ss, int stream,
				 i64 p, i64 len)
{
	struct stream_info *sinfo = ss;

	while (len) {
		i64 n = MIN(sinfo->bufsize - sinfo->s[stream].buflen, len);

		control->do_mcpy(control, sinfo->s[stream].buf + sinfo->s[stream].buflen, p, n);

		sinfo->s[stream].buflen += n;
		p += n;
		len -= n;

		if (sinfo->s[stream].buflen == sinfo->bufsize)
			flush_buffer(control, sinfo, stream);
	}
}

static void put_literal(rzip_control *control, struct rzip_state *st, i64 last, i64 p)
{
	do {
		i64 len = p - last;

		if (len > 0xFFFF)
			len = 0xFFFF;

		st->stats.literals++;
		st->stats.literal_bytes += len;

		put_header(control, st, 0, len);

		if (len)
			write_sbstream(control, st->ss, 1, last, len);
		last += len;
	} while (p > last);
}

/* All zero means empty.  We might miss the first chunk this way. */
static inline bool empty_hash_n(uint32_t t, i64 offset)
{
	return !(offset | t);
}

static i64 primary_hash(struct rzip_state *st, tag t)
{
	return t & ((1U << st->hash_bits) - 1);
}

static inline tag increase_mask(tag tag_mask)
{
	/* Get more precise. */
	return (tag_mask << 1) | 1;
}

static inline bool minimum_bitness(struct rzip_state *st, tag t)
{
	tag better_than_min = increase_mask(st->minimum_tag_mask);

	return (t & better_than_min) != better_than_min;
}

/* Is a going to be cleaned before b?  ie. does a have fewer low bits set? */
static inline bool lesser_bitness(tag a, tag b)
{
	a = ~a;
	b = ~b;
	return (__builtin_ffs((int)a) < __builtin_ffs((int)b));
}

static inline size_t hash_entry_bytes(struct rzip_state *st)
{
	return st->hash_wide ? sizeof(struct hash_entry_wide) : sizeof(struct hash_entry);
}

static inline void hash_get(struct rzip_state *st, i64 h, tag *t, i64 *offset)
{
	if (st->hash_wide) {
		struct hash_entry_wide *he = &((struct hash_entry_wide *)st->hash_table)[h];

		*t = he->t;
		*offset = he->offset;
	} else {
		struct hash_entry *he = &((struct hash_entry *)st->hash_table)[h];

		*t = he->t;
		*offset = he->offset;
	}
}

static inline void hash_set(struct rzip_state *st, i64 h, tag t, i64 offset)
{
	if (st->hash_wide) {
		struct hash_entry_wide *he = &((struct hash_entry_wide *)st->hash_table)[h];

		he->t = t;
		he->offset = offset;
	} else {
		struct hash_entry *he = &((struct hash_entry *)st->hash_table)[h];

		he->t = t;
		he->offset = (uint32_t)offset;
	}
}

static inline void hash_clear_slot(struct rzip_state *st, i64 h)
{
	hash_set(st, h, 0, 0);
}

/* If hash bucket is taken, we spill into next bucket(s).  Secondary hashing
   works better in theory, but modern caches make this 20% faster. */
static void insert_hash(struct rzip_state *st, tag t, i64 offset)
{
	i64 h, victim_h = 0, round = 0;
	/* If we need to kill one, this will be it. */
	static i64 victim_round = 0;
	i64 mask = (1U << st->hash_bits) - 1;
	tag he_t;
	i64 he_off;

	h = primary_hash(st, t);
	hash_get(st, h, &he_t, &he_off);
	while (!empty_hash_n(he_t, he_off)) {
		/* If this due for cleaning anyway, just replace it:
		   rehashing might move it behind tag_clean_ptr. */
		if (minimum_bitness(st, he_t)) {
			st->hash_count--;
			break;
		}
		/* If we are better than current occupant, we can't
		   jump over it: it will be cleaned before us, and
		   noone would then find us in the hash table.  Rehash
		   it, then take its place. */
		if (lesser_bitness(he_t, t)) {
			insert_hash(st, he_t, he_off);
			break;
		}

		/* If we have lots of identical patterns, we end up
		   with lots of the same hash number.  Discard random. */
		if (he_t == t) {
			if (round == victim_round)
				victim_h = h;
			if (++round == st->level->max_chain_len) {
				h = victim_h;
				st->hash_count--;
				victim_round++;
				if (victim_round == st->level->max_chain_len)
					victim_round = 0;
				break;
			}
		}

		h = (h + 1) & mask;
		hash_get(st, h, &he_t, &he_off);
	}

	hash_set(st, h, t, offset);
}

/* Eliminate one hash entry with minimum number of lower bits set.
   Returns tag requirement for any new entries. */
static inline tag clean_one_from_hash(rzip_control *control, struct rzip_state *st)
{
	tag better_than_min;
	tag he_t;
	i64 he_off;

again:
	better_than_min = increase_mask(st->minimum_tag_mask);
	if (!st->tag_clean_ptr)
		print_maxverbose("Starting sweep for mask %u\n", (unsigned int)st->minimum_tag_mask);

	for (; st->tag_clean_ptr < (1U << st->hash_bits); st->tag_clean_ptr++) {
		hash_get(st, st->tag_clean_ptr, &he_t, &he_off);
		if (empty_hash_n(he_t, he_off))
			continue;
		if ((he_t & better_than_min) != better_than_min) {
			hash_clear_slot(st, st->tag_clean_ptr);
			st->hash_count--;
			return better_than_min;
		}
	}

	/* We hit the end: everthing in hash satisfies the better mask. */
	st->minimum_tag_mask = better_than_min;
	st->tag_clean_ptr = 0;
	goto again;
}

static inline void single_next_tag(rzip_control *control, struct rzip_state *st, i64 p, tag *t)
{
	uchar u;

	u = control->sb.buf_low[p - 1];
	*t ^= st->hash_index[u];
	u = control->sb.buf_low[p + MINIMUM_MATCH - 1];
	*t ^= st->hash_index[u];
}

static inline void sliding_next_tag(rzip_control *control, struct rzip_state *st, i64 p, tag *t)
{
	struct sliding_buffer *sb = &control->sb;
	i64 p1 = p - 1;
	i64 p2 = p + MINIMUM_MATCH - 1;

	/* Common case: both tag bytes sit in the low map — no high remap. */
	if (sliding_in_low(sb, p1, 1) && sliding_in_low(sb, p2, 1)) {
		*t ^= st->hash_index[sb->buf_low[p1 - sb->offset_low]];
		*t ^= st->hash_index[sb->buf_low[p2 - sb->offset_low]];
		return;
	}
	*t ^= st->hash_index[*sliding_get_sb(control, p1)];
	*t ^= st->hash_index[*sliding_get_sb(control, p2)];
}

static inline tag single_full_tag(rzip_control *control, struct rzip_state *st, i64 p)
{
	tag ret = 0;
	int i;

	for (i = 0; i < MINIMUM_MATCH; i++)
		ret ^= st->hash_index[control->sb.buf_low[p + i]];
	return ret;
}

static inline tag sliding_full_tag(rzip_control *control, struct rzip_state *st, i64 p)
{
	struct sliding_buffer *sb = &control->sb;
	tag ret = 0;
	int i;
	i64 contig;
	uchar *s;

	/* Whole MINIMUM_MATCH window in the low map. */
	if (sliding_in_low(sb, p, MINIMUM_MATCH)) {
		s = sb->buf_low + (p - sb->offset_low);
		for (i = 0; i < MINIMUM_MATCH; i++)
			ret ^= st->hash_index[s[i]];
		return ret;
	}

	/* Prefer one contiguous high/low span when possible. */
	s = sliding_map_fwd(control, p, &contig);
	if (contig >= MINIMUM_MATCH) {
		for (i = 0; i < MINIMUM_MATCH; i++)
			ret ^= st->hash_index[s[i]];
		return ret;
	}

	for (i = 0; i < MINIMUM_MATCH; ) {
		i64 n;

		s = sliding_map_fwd(control, p + i, &contig);
		n = contig;
		if (n > MINIMUM_MATCH - i)
			n = MINIMUM_MATCH - i;
		while (n--) {
			ret ^= st->hash_index[s[0]];
			s++;
			i++;
		}
	}
	return ret;
}

/*
 * Core match on a linear buffer where absolute position q is at
 * base[q - base_off]. Used for the non-sliding path and the sliding
 * fast path when both sides lie in the low map.
 */
static i64
match_len_linear(const uchar *base, i64 base_off, struct rzip_state *st,
		 i64 p0, i64 op, i64 end, i64 *rev, i64 best)
{
	const uchar *a, *b;
	i64 max_fwd, f, max_rev, rev_end, op0, p, o, len;

	if (op >= p0)
		return 0;

	op0 = op;
	max_fwd = end - p0;
	a = base + (p0 - base_off);
	b = base + (op0 - base_off);
	f = 0;

	while (f + (i64)sizeof(size_t) <= max_fwd) {
		size_t xa, xb;

		memcpy(&xa, a + f, sizeof(size_t));
		memcpy(&xb, b + f, sizeof(size_t));
		if (xa != xb)
			break;
		f += (i64)sizeof(size_t);
	}
	while (f < max_fwd && a[f] == b[f])
		f++;

	rev_end = MAX((i64)0, st->last_match);
	max_rev = p0 - rev_end;
	if (max_rev > op0)
		max_rev = op0;

	if (f + max_rev < MINIMUM_MATCH || f + max_rev <= best)
		return 0;

	p = p0;
	o = op0;
	while (p > rev_end && o > 0 &&
	       base[(o - 1) - base_off] == base[(p - 1) - base_off]) {
		o--;
		p--;
	}
	*rev = p0 - p;

	len = f + *rev;
	if (len < MINIMUM_MATCH || len <= best)
		return 0;
	return len;
}

static i64
single_match_len(rzip_control *control, struct rzip_state *st, i64 p0, i64 op,
		 i64 end, i64 *rev, i64 best)
{
	return match_len_linear(control->sb.buf_low, 0, st, p0, op, end, rev, best);
}

/* Sliding match: low-map fast path, else span-wise forward/reverse compares. */
static i64
sliding_match_len(rzip_control *control, struct rzip_state *st, i64 p0, i64 op,
		  i64 end, i64 *rev, i64 best)
{
	struct sliding_buffer *sb = &control->sb;
	i64 max_fwd, f, max_rev, rev_end, p, o, len, op0;
	i64 low = sb->offset_low, lsz = sb->size_low;

	if (op >= p0)
		return 0;

	rev_end = MAX((i64)0, st->last_match);
	max_fwd = end - p0;

	/* Both sides fully in the low map (forward + reverse extent). */
	if (rev_end >= low && p0 >= low && end <= low + lsz &&
	    op >= low && op + max_fwd <= low + lsz) {
		return match_len_linear(sb->buf_low, low, st, p0, op, end, rev, best);
	}

	op0 = op;
	f = 0;
	while (f < max_fwd) {
		i64 c1, c2, n, m;
		uchar *a = sliding_map_fwd(control, p0 + f, &c1);
		uchar *b = sliding_map_fwd(control, op0 + f, &c2);

		n = c1 < c2 ? c1 : c2;
		if (n > max_fwd - f)
			n = max_fwd - f;
		if (n <= 0)
			break;
		if (memcmp(a, b, (size_t)n) == 0) {
			f += n;
			continue;
		}
		m = 0;
		while (m < n && a[m] == b[m])
			m++;
		f += m;
		break;
	}

	max_rev = p0 - rev_end;
	if (max_rev > op0)
		max_rev = op0;

	if (f + max_rev < MINIMUM_MATCH || f + max_rev <= best)
		return 0;

	/* Reverse-extend using contiguous runs ending at p-1 / o-1. */
	p = p0;
	o = op0;
	while (p > rev_end && o > 0) {
		i64 n1 = sliding_contig_before(control, p);
		i64 n2 = sliding_contig_before(control, o);
		i64 n = n1 < n2 ? n1 : n2;
		uchar *a, *b;
		i64 m;

		if (n > p - rev_end)
			n = p - rev_end;
		if (n > o)
			n = o;
		if (n <= 0)
			break;

		a = sliding_get_sb(control, p - 1);
		b = sliding_get_sb(control, o - 1);
		m = 0;
		while (m < n && a[-m] == b[-m])
			m++;
		p -= m;
		o -= m;
		if (m < n)
			break;
	}
	*rev = p0 - p;

	len = f + *rev;
	if (len < MINIMUM_MATCH || len <= best)
		return 0;
	return len;
}

static inline i64
match_len(rzip_control *control, struct rzip_state *st, i64 p0, i64 op,
	  i64 end, i64 *rev, i64 best)
{
	if (st->sliding)
		return sliding_match_len(control, st, p0, op, end, rev, best);
	return single_match_len(control, st, p0, op, end, rev, best);
}

static inline void
next_tag(rzip_control *control, struct rzip_state *st, i64 p, tag *t)
{
	if (st->sliding)
		sliding_next_tag(control, st, p, t);
	else
		single_next_tag(control, st, p, t);
}

static inline tag
full_tag(rzip_control *control, struct rzip_state *st, i64 p)
{
	if (st->sliding)
		return sliding_full_tag(control, st, p);
	return single_full_tag(control, st, p);
}

static inline i64
find_best_match(rzip_control *control, struct rzip_state *st, tag t, i64 p,
		i64 end, i64 *offset, i64 *reverse)
{
	i64 length = 0;
	i64 rev = 0;
	i64 h, probes = 0, tag_hits = 0;
	i64 mask = (1U << st->hash_bits) - 1;
	/* Cap probes: same-tag hits like insert_hash, plus a modest total walk. */
	const i64 max_tag = st->level->max_chain_len;
	const i64 max_probes = max_tag * 4 + 16;
	tag he_t;
	i64 he_off;

	*reverse = 0;
	*offset = 0;

	h = primary_hash(st, t);
	hash_get(st, h, &he_t, &he_off);
	while (!empty_hash_n(he_t, he_off) && probes < max_probes) {
		probes++;
		if (t == he_t) {
			i64 mlen;

			if (tag_hits >= max_tag)
				break;
			tag_hits++;
			mlen = match_len(control, st, p, he_off, end, &rev, length);
			if (mlen) {
				length = mlen;
				*offset = he_off - rev;
				*reverse = rev;
				st->stats.tag_hits++;
			} else
				st->stats.tag_misses++;
		}

		h = (h + 1) & mask;
		hash_get(st, h, &he_t, &he_off);
	}

	return length;
}

static void show_distrib(rzip_control *control, struct rzip_state *st)
{
	i64 primary = 0;
	i64 total = 0;
	i64 i;
	tag he_t;
	i64 he_off;

	for (i = 0; i < (1U << st->hash_bits); i++) {
		hash_get(st, i, &he_t, &he_off);
		if (empty_hash_n(he_t, he_off))
			continue;
		total++;
		if (primary_hash(st, he_t) == i)
			primary++;
	}

	if (total != st->hash_count)
		print_err("WARNING: hash_count says total %"PRId64"\n", st->hash_count);

	if (!total)
		print_output("0 total hashes\n");
	else {
		print_output("%"PRId64" total hashes -- %"PRId64" in primary bucket (%-2.3f%%)\n",
			     total, primary, primary * 100.0 / total);
	}
}

/* Dedicated MD5 worker: one long-lived thread, large batches, reused buffer. */
static void *md5_worker(void *data)
{
	rzip_control *control = (rzip_control *)data;

	while (42) {
		cksem_wait(control, &control->cksum_worksem);
		if (control->checksum.shutdown) {
			cksem_post(control, &control->cksumsem);
			break;
		}
		if (control->checksum.len > 0)
			md5_process_bytes(control->checksum.buf, control->checksum.len,
					  &control->ctx);
		cksem_post(control, &control->cksumsem);
	}
	return NULL;
}

static void md5_thread_start(rzip_control *control)
{
	i64 cap = CKSUM_CHUNK;

	round_to_page(&cap);
	control->checksum.capacity = cap;
	control->checksum.buf = malloc((size_t)cap);
	if (unlikely(!control->checksum.buf))
		failure("Failed to allocate MD5 batch buffer\n");
	control->checksum.len = 0;
	control->checksum.shutdown = 0;

	cksem_init(control, &control->cksumsem);
	cksem_post(control, &control->cksumsem);
	cksem_init(control, &control->cksum_worksem);

	if (unlikely(!create_pthread(control, &control->md5_thread, NULL, md5_worker, control)))
		failure("Failed to start MD5 worker thread\n");
}

static void md5_thread_stop(rzip_control *control)
{
	/* Wait until the worker is idle, then ask it to exit. */
	cksem_wait(control, &control->cksumsem);
	control->checksum.shutdown = 1;
	control->checksum.len = 0;
	cksem_post(control, &control->cksum_worksem);
	if (unlikely(!join_pthread(control, control->md5_thread, NULL)))
		failure("Failed to join MD5 worker thread\n");
	dealloc(control->checksum.buf);
	control->checksum.capacity = 0;
}

/* Queue [offset, offset+len) from the sliding/single map for MD5. */
static void md5_queue(rzip_control *control, i64 offset, i64 len)
{
	while (len > 0) {
		i64 n = MIN(len, control->checksum.capacity);

		cksem_wait(control, &control->cksumsem);
		control->do_mcpy(control, control->checksum.buf, offset, n);
		control->checksum.len = n;
		cksem_post(control, &control->cksum_worksem);
		offset += n;
		len -= n;
	}
}

/* Wait until all submitted MD5 work is finished. */
static void md5_drain(rzip_control *control)
{
	cksem_wait(control, &control->cksumsem);
	cksem_post(control, &control->cksumsem);
}

static inline void hash_search(rzip_control *control, struct rzip_state *st,
			       double pct_base, double pct_multiple)
{
	i64 cksum_limit = 0, p, end, progress_at;
	tag t = 0, tag_mask = (1 << st->level->initial_freq) - 1;
	struct sliding_buffer *sb = &control->sb;
	int lastpct = 0, last_chunkpct = 0;
	struct {
		i64 p;
		i64 ofs;
		i64 len;
	} current;
	/* Progress at most every 64KiB or each 1% of the chunk. */
	const i64 progress_bytes = 64 * 1024;

	{
		/* Target slot count as in rzip-2.1 (8-byte entries per mb_used). */
		i64 hashsize = (i64)st->level->mb_used *
				(1024 * 1024 / HASH_ENTRY_SIZE_NARROW);
		/* Do not build a table larger than the chunk can use. */
		i64 cap = st->chunk_size;
		int bits;
		int wide;
		size_t esize;
		i64 nslots, mem;

		if (cap < (1 << 16))
			cap = 1 << 16;
		if (hashsize > cap)
			hashsize = cap;

		wide = (st->chunk_size > (i64)UINT32_MAX);
		esize = wide ? sizeof(struct hash_entry_wide) : sizeof(struct hash_entry);

		for (bits = 0; (1U << bits) < hashsize; bits++)
			;
		nslots = (i64)1 << bits;
		mem = nslots * (i64)esize;

		if (!st->hash_table || st->hash_bits != bits || st->hash_wide != wide) {
			dealloc(st->hash_table);
			st->hash_bits = bits;
			st->hash_wide = wide;
			st->hash_table = calloc((size_t)nslots, esize);
			if (unlikely(!st->hash_table))
				failure("Failed to allocate hash table in hash_search\n");
			print_maxverbose("hash slots = %"PRId64" bits = %d wide = %d (%.1fMB, level %luMB)\n",
					 nslots, bits, wide, mem / (1024.0 * 1024.0),
					 st->level->mb_used);
		} else
			memset(st->hash_table, 0, (size_t)mem);

		/* 66% full at max. */
		st->hash_limit = nslots / 3 * 2;
	}

	st->minimum_tag_mask = tag_mask;
	st->tag_clean_ptr = 0;
	st->hash_count = 0;
	st->s0_len = 0;

	p = 0;
	end = st->chunk_size - MINIMUM_MATCH;
	st->last_match = p;
	current.len = 0;
	current.p = p;
	current.ofs = 0;
	progress_at = progress_bytes;
	if (end > 0) {
		i64 one_pct = end / 100;

		if (one_pct > 0 && one_pct < progress_at)
			progress_at = one_pct;
	}

	if (likely(end > 0))
		t = full_tag(control, st, p);

	while (p < end) {
		i64 reverse, mlen, offset;

		sb->offset_search = ++p;
		if (unlikely(sb->offset_search > sb->offset_low + sb->size_low))
			remap_low_sb(control, &control->sb);

		if (unlikely(st->chunk_size && p >= progress_at)) {
			i64 chunk_pct;
			int pct;
			i64 one_pct, next_pct_at, next_byte_at;

			pct = pct_base + (pct_multiple * (100.0 * p) / st->chunk_size);
			chunk_pct = end ? (p * 100 / end) : 100;
			if (pct != lastpct || chunk_pct != last_chunkpct) {
				if (!STDIN || st->stdin_eof)
					print_progress("Total: %2d%%  ", pct);
				print_progress("Chunk: %2"PRId64"%%\r", chunk_pct);
				if (control->info_cb)
					control->info_cb(control->info_data,
						(!STDIN || st->stdin_eof) ? pct : -1, chunk_pct);
				lastpct = pct;
				last_chunkpct = chunk_pct;
			}
			next_byte_at = p + progress_bytes;
			one_pct = end / 100;
			if (one_pct > 0) {
				next_pct_at = ((p / one_pct) + 1) * one_pct;
				if (next_pct_at < next_byte_at)
					next_byte_at = next_pct_at;
			}
			progress_at = next_byte_at;
		}

		next_tag(control, st, p, &t);

		/* Don't look for a match if there are no tags with
		   this number of bits in the hash table. */
		if ((t & st->minimum_tag_mask) != st->minimum_tag_mask)
			continue;

		offset = 0;
		mlen = find_best_match(control, st, t, p, end, &offset, &reverse);

		/* Only insert occasionally into hash. */
		if ((t & tag_mask) == tag_mask) {
			st->stats.inserts++;
			st->hash_count++;
			insert_hash(st, t, p);
			if (st->hash_count > st->hash_limit)
				tag_mask = clean_one_from_hash(control, st);
		}

		if (mlen > current.len) {
			current.p = p - reverse;
			current.len = mlen;
			current.ofs = offset;
		}

		if ((current.len >= GREAT_MATCH || p >= current.p + MINIMUM_MATCH)
		    && current.len >= MINIMUM_MATCH) {
			if (st->last_match < current.p)
				put_literal(control, st, st->last_match, current.p);
			put_match(control, st, current.p, current.ofs, current.len);
			st->last_match = current.p + current.len;
			current.p = p = st->last_match;
			current.len = 0;
			t = full_tag(control, st, p);
		}

		/* Feed MD5 in large batches on the worker thread as search advances. */
		if (!NO_MD5 && !st->chunk_md5_done) {
			while (cksum_limit < st->chunk_size && p > cksum_limit) {
				i64 n = MIN(control->checksum.capacity,
					    st->chunk_size - cksum_limit);

				md5_queue(control, cksum_limit, n);
				cksum_limit += n;
			}
		}
	}

	if (MAX_VERBOSE)
		show_distrib(control, st);

	if (st->last_match < st->chunk_size)
		put_literal(control, st, st->last_match, st->chunk_size);

	/* Finish any unhashed tail of this chunk, then wait for the worker. */
	if (!NO_MD5) {
		if (!st->chunk_md5_done && cksum_limit < st->chunk_size)
			md5_queue(control, cksum_limit, st->chunk_size - cksum_limit);
		md5_drain(control);
	}

	/* End-of-stream marker only (head=0, len=0). No trailing CRC32;
	 * integrity is MD5 when magic[21] is set. Match/literal offsets are
	 * complete before this marker and do not depend on a CRC field. */
	put_literal(control, st, 0, 0);
	s0_flush(control, st);
}


static inline void init_hash_indexes(struct rzip_state *st)
{
	int i;

	for (i = 0; i < 256; i++)
		st->hash_index[i] = (tag)(((unsigned)random() << 16) ^ (unsigned)random());
}

#if !defined(__linux)
# define mremap fake_mremap

static inline void *fake_mremap(void *old_address, size_t old_size, size_t new_size, int flags __UNUSED__)
{
	if (new_size > old_size) {
		/* No control pointer here; match process-wide failure status. */
		fprintf(stderr, "fake_mremap: This should only be used to shrink things.\n");
		exit(LRZIP_EXIT_FAILURE);
	} else {
		/* new_size occupies N pages; old_size occupies M > N pages;
		 we want to unmap the M - N pages at the end.
		 note the idiom: ceiling(n/k) = (n+k-1) div k */
		size_t kept_n = (new_size + PAGE_SIZE - 1) / PAGE_SIZE;
		int ret = munmap(old_address + (kept_n * PAGE_SIZE), old_size - (kept_n * PAGE_SIZE));

		if (ret < 0)
			return MAP_FAILED;

		return old_address;
	}
}
#endif

/* stdin is not file backed so we have to emulate the mmap by mapping
 * anonymous ram and reading stdin into it. It means the maximum ram
 * we can use will be less but we will already have determined this in
 * rzip_chunk */
static inline void mmap_stdin(rzip_control *control, uchar *buf,
			      struct rzip_state *st)
{
	i64 len = st->chunk_size;
	uchar *offset_buf = buf;
	ssize_t ret;
	i64 total;

	total = 0;
	while (len > 0) {
		ret = read(fileno(control->inFILE), offset_buf, (size_t)MIN(len, MAX_RW_COUNT));
		if (unlikely(ret < 0))
			failure("Failed to read in mmap_stdin\n");
		total += ret;
		if (ret == 0) {
			/* Should be EOF */
			print_maxverbose("Shrinking chunk to %"PRId64"\n", total);
			if (likely(total)) {
				buf = (uchar *)mremap(buf, st->chunk_size, total, 0);
				st->mmap_size = st->chunk_size = total;
			} else {
				/* Empty file */
				buf = (uchar *)mremap(buf, st->chunk_size, control->page_size, 0);
				st->mmap_size = control->page_size;
				st->chunk_size = 0;
			}
			if (unlikely(buf == MAP_FAILED))
				failure("Failed to remap to smaller buf in mmap_stdin\n");
			control->eof = st->stdin_eof = 1;
			break;
		}
		offset_buf += ret;
		len -= ret;
	}
	control->st_size += total;
}

static inline void
init_sliding_mmap(rzip_control *control, struct rzip_state *st, int fd_in,
		  i64 offset)
{
	struct sliding_buffer *sb = &control->sb;

	/* Initialise the high buffer. One page size is fastest to manipulate */
	if (!STDIN) {
		sb->high_length = control->page_size;
		sb->buf_high = (uchar *)mmap(NULL, sb->high_length, PROT_READ, MAP_SHARED, fd_in, offset);
		if (unlikely(sb->buf_high == MAP_FAILED))
			failure("Unable to mmap buf_high in init_sliding_mmap\n");
		sb->size_high = sb->high_length;
		sb->offset_high = 0;
	}
	sb->offset_low = 0;
	sb->offset_search = 0;
	sb->size_low = st->mmap_size;
	sb->orig_size = st->chunk_size;
	sb->fd = fd_in;
}

static void add_to_sslist(rzip_control *control, struct rzip_state *st)
{
	struct node *node = calloc(1, sizeof(struct node));

	if (unlikely(!node))
		failure("Failed to calloc struct node in add_to_sslist\n");
	node->data = st->ss;
	node->prev = st->sslist;
	st->head = node;
}

/* compress a chunk of an open file. Assumes that the file is able to
   be mmap'd and is seekable */
static inline void
rzip_chunk(rzip_control *control, struct rzip_state *st, int fd_in, int fd_out,
	   i64 offset, double pct_base, double pct_multiple)
{
	struct sliding_buffer *sb = &control->sb;

	init_sliding_mmap(control, st, fd_in, offset);

	/* Chunk prefilter: executable code compresses several percent better
	 * when branch converted BEFORE rzip sees it, because conversion on
	 * raw, aligned data creates the repeats that both rzip and the back
	 * end can find. Only attempted under --filter when the whole chunk
	 * is mapped (no sliding mmap). With auto selection a duplicate
	 * window probe must approve first: identical code duplicated at
	 * different offsets converts differently, so converting a dedup
	 * heavy chunk would cost more than it gains and is refused. A
	 * forced x86 or arm64 filter converts the chunk unconditionally;
	 * forced delta stays a block level filter. */
	st->chunk_md5_done = false;
	control->chunk_filter = LRZ_FILTER_NONE;
	if (control->filter_mode && LZMA_COMPRESS &&
	    st->mmap_size == st->chunk_size && st->chunk_size >= (1 << 20)) {
		int kind;

		if (control->filter_mode > 0)
			kind = control->filter_mode <= LRZ_CHUNK_FILTER_MAX ?
				control->filter_mode : LRZ_FILTER_NONE;
		else
			kind = lrz_chunk_filter_pick(control, sb->buf_low, st->chunk_size);

		if (kind != LRZ_FILTER_NONE && !STDIN) {
			/* Remap the chunk privately so it can be converted
			 * in place. Failure just means no filter. */
			uchar *newbuf;

			if (unlikely(munmap(sb->buf_low, sb->size_low)))
				failure("Failed to munmap in rzip_chunk\n");
			newbuf = (uchar *)mmap(NULL, sb->size_low, PROT_READ | PROT_WRITE,
					       MAP_PRIVATE, sb->fd, sb->orig_offset + sb->offset_low);
			if (unlikely(newbuf == MAP_FAILED)) {
				/* Restore a read only shared map and carry on
				 * unfiltered */
				newbuf = (uchar *)mmap(NULL, sb->size_low, PROT_READ,
						       MAP_SHARED, sb->fd, sb->orig_offset + sb->offset_low);
				if (unlikely(newbuf == MAP_FAILED))
					failure("Failed to re mmap in rzip_chunk\n");
				kind = LRZ_FILTER_NONE;
			}
			sb->buf_low = newbuf;
		}
		if (kind != LRZ_FILTER_NONE) {
			/* The stored md5 must be of the original bytes, so
			 * hash the chunk before converting it. */
			if (!NO_MD5) {
				md5_queue(control, 0, st->chunk_size);
				st->chunk_md5_done = true;
			}
			lrz_filter_convert_mem(sb->buf_low, st->chunk_size, kind, true);
			control->chunk_filter = kind;
			print_verbose("Chunk prefiltered with %s branch conversion\n",
				      kind == LRZ_FILTER_X86 ? "x86" : "arm64");
		}
	}

	st->ss = open_stream_out(control, fd_out, NUM_STREAMS, st->chunk_size, st->chunk_bytes);
	if (unlikely(!st->ss))
		failure("Failed to open streams in rzip_chunk\n");

	print_verbose("Beginning rzip pre-processing phase\n");
	hash_search(control, st, pct_base, pct_multiple);

	/* unmap buffer before closing and reallocating streams */
	if (unlikely(munmap(sb->buf_low, sb->size_low))) {
		close_stream_out(control, st->ss);
		failure("Failed to munmap in rzip_chunk\n");
	}
	if (!STDIN) {
		if (unlikely(munmap(sb->buf_high, sb->size_high))) {
			close_stream_out(control, st->ss);
			failure("Failed to munmap in rzip_chunk\n");
		}
	}

	if (unlikely(close_stream_out(control, st->ss)))
		failure("Failed to flush/close streams in rzip_chunk\n");

	/* Save the sinfo data to a list to be safely released after all
	 * threads have been shut down. */
	add_to_sslist(control, st);
}

static void clear_sslist(struct rzip_state *st)
{
	while (st->head) {
		struct node *node = st->head;
		struct stream_info *sinfo = node->data;

		dealloc(sinfo->s);
		dealloc(sinfo);
		st->head = node->prev;
		dealloc(node);
	}
}

/* compress a whole file chunks at a time */
void rzip_fd(rzip_control *control, int fd_in, int fd_out)
{
	struct sliding_buffer *sb = &control->sb;

	/* add timers for ETA estimates
	 * Base it off the file size and number of iterations required
	 * depending on compression window size
	 * Track elapsed time and estimated time to go
	 * If file size < compression window, can't do
	 */
	struct timeval current, start, last;
	i64 len = 0, last_chunk = 0;
	int pass = 0, passes, j;
	double chunkmbs, tdiff;
	struct rzip_state *st;
	struct statvfs fbuf;
	struct stat s, s2;
	i64 free_space;

	if (!NO_MD5)
		md5_init_ctx(&control->ctx);

	st = calloc(1, sizeof(*st));
	if (unlikely(!st))
		failure("Failed to allocate control state in rzip_fd\n");

	if (LZO_COMPRESS) {
		if (unlikely(lzo_init() != LZO_E_OK)) {
			dealloc(st);
			failure("lzo_init() failed\n");
		}
	}

	if (unlikely(fstat(fd_in, &s))) {
		dealloc(st);
		failure("Failed to stat fd_in in rzip_fd\n");
	}

	/* Start MD5 worker after early setup so failures above need no join. */
	if (!NO_MD5)
		md5_thread_start(control);

	if (!STDIN) {
		len = control->st_size = s.st_size;
		print_verbose("File size: %"PRId64"\n", len);
	} else
		control->st_size = 0;

	if (!STDOUT) {
		/* Check if there's enough free space on the device chosen to fit the
		* compressed file, based on the compressed file being as large as the
		* uncompressed file. */
		if (unlikely(fstatvfs(fd_out, &fbuf))) {
			dealloc(st);
			failure("Failed to fstatvfs in compress_file\n");
		}
		free_space = (i64)fbuf.f_bsize * (i64)fbuf.f_bavail;
		if (free_space < control->st_size) {
			if (FORCE_REPLACE)
				print_output("Warning, possibly inadequate free space detected, but attempting to compress due to -f option being used.\n");
			else {
				dealloc(st);
				failure("Possibly inadequate free space to compress file, use -f to override.\n");
			}
		}
	}

	/* Optimal use of ram involves using no more than 2/3 of it, so we
	 * allocate 1/3 of it to the main buffer and use a sliding mmap
	 * buffer to work on 2/3 ram size, leaving enough ram for the
	 * compression backends */
	control->max_mmap = control->maxram;
	round_to_page(&control->max_mmap);

	/* Set maximum chunk size to 2/3 of ram if not unlimited or specified
	 * by a control window. When it's smaller than the file size, round it
	 * to page size for efficiency. */
	if (UNLIMITED)
		control->max_chunk = control->st_size;
	else if (control->window)
		control->max_chunk = control->window * CHUNK_MULTIPLE;
	else
		control->max_chunk = control->ramsize / 3 * 2;
	control->max_mmap = MIN(control->max_mmap, control->max_chunk);
	if (control->max_chunk < control->st_size)
		round_to_page(&control->max_chunk);

	if (!STDIN)
		st->chunk_size = MIN(control->max_chunk, len);
	else
		st->chunk_size = control->max_mmap;
	if (st->chunk_size < len)
		round_to_page(&st->chunk_size);

	st->level = &levels[control->compression_level];
	st->fd_in = fd_in;
	st->fd_out = fd_out;
	st->stdin_eof = 0;

	init_hash_indexes(st);

	passes = 0;

	/* set timers and chunk counter */
	last.tv_sec = last.tv_usec = 0;
	gettimeofday(&start, NULL);

	prepare_streamout_threads(control);
	control->do_mcpy = single_mcpy;
	st->sliding = 0;

	while (!pass || len > 0 || (STDIN && !st->stdin_eof)) {
		double pct_base, pct_multiple;
		i64 offset = s.st_size - len;
		int bits = 8;

		st->chunk_size = control->max_chunk;
		st->mmap_size = control->max_mmap;
		if (!STDIN) {
			st->chunk_size = MIN(st->chunk_size, len);
			if (likely(st->chunk_size))
				st->mmap_size = MIN(st->mmap_size, len);
			else
				st->mmap_size = control->page_size;
		}

retry:
		if (STDIN) {
			/* NOTE the buf is saved here for STDIN mode */
			sb->buf_low = mmap(NULL, st->mmap_size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
			/* Better to shrink the window to the largest size that works than fail */
			if (sb->buf_low == MAP_FAILED) {
				if (unlikely(errno != ENOMEM)) {
					close_streamout_threads(control);
					dealloc(st->hash_table);
					dealloc(st);
					failure("Failed to mmap %s\n", control->infile);
				}
				st->mmap_size = st->mmap_size / 10 * 9;
				round_to_page(&st->mmap_size);
				if (unlikely(!st->mmap_size)) {
					close_streamout_threads(control);
					dealloc(st->hash_table);
					dealloc(st);
					failure("Unable to mmap any ram\n");
				}
				goto retry;
			}
			st->chunk_size = st->mmap_size;
			mmap_stdin(control, sb->buf_low, st);
		} else {
			/* NOTE The buf is saved here for !STDIN mode */
			sb->buf_low = (uchar *)mmap(sb->buf_low, st->mmap_size, PROT_READ, MAP_SHARED, fd_in, offset);
			if (sb->buf_low == MAP_FAILED) {
				if (unlikely(errno != ENOMEM)) {
					close_streamout_threads(control);
					dealloc(st->hash_table);
					dealloc(st);
					failure("Failed to mmap %s\n", control->infile);
				}
				st->mmap_size = st->mmap_size / 10 * 9;
				round_to_page(&st->mmap_size);
				if (unlikely(!st->mmap_size)) {
					close_streamout_threads(control);
					dealloc(st->hash_table);
					dealloc(st);
					failure("Unable to mmap any ram\n");
				}
				goto retry;
			}
			if (st->mmap_size < st->chunk_size) {
				print_maxverbose("Enabling sliding mmap mode and using mmap of %"PRId64" bytes with window of %"PRId64" bytes\n", st->mmap_size, st->chunk_size);
				control->do_mcpy = &sliding_mcpy;
				st->sliding = 1;
			}
		}
		print_maxverbose("Succeeded in testing %"PRId64" sized mmap for rzip pre-processing\n", st->mmap_size);

		if (st->chunk_size > control->ramsize)
			print_verbose("Compression window is larger than ram, will proceed with unlimited mode possibly much slower\n");

		if (!passes && !STDIN && st->chunk_size) {
			passes = s.st_size / st->chunk_size + !!(s.st_size % st->chunk_size);
			if (passes == 1)
				print_verbose("Will take 1 pass\n");
			else
				print_verbose("Will take %d passes\n", passes);
		}

		sb->orig_offset = offset;
		print_maxverbose("Chunk size: %"PRId64"\n", st->chunk_size);

		/* Determine the chunk byte width to write to the file
		 * This allows archives of different chunk sizes to have
		 * optimal byte width entries. When working with stdin we
		 * won't know in advance how big it is so it will always be
		 * rounded up to the window size. */
		while (st->chunk_size >> bits > 0)
			bits++;
		st->chunk_bytes = bits / 8;
		if (bits % 8)
			st->chunk_bytes++;
		print_maxverbose("Byte width: %d\n", st->chunk_bytes);

		if (STDIN)
			pct_base = (100.0 * -len) / control->st_size;
		else
			pct_base = (100.0 * (control->st_size - len)) / control->st_size;
		pct_multiple = ((double)st->chunk_size) / control->st_size;
		pass++;
		if (st->stdin_eof)
			passes = pass;

		gettimeofday(&current, NULL);
		/* this will count only when size > window */
		if (last.tv_sec > 0 && pct_base > 100) {
			unsigned int eta_hours, eta_minutes, eta_seconds, elapsed_time, finish_time,
				elapsed_hours, elapsed_minutes, elapsed_seconds, diff_seconds;

			elapsed_time = current.tv_sec - start.tv_sec;
			finish_time = elapsed_time / (pct_base / 100.0);
			elapsed_hours = elapsed_time / 3600;
			elapsed_minutes = (elapsed_time / 60) % 60;
			elapsed_seconds = elapsed_time % 60;
			diff_seconds = finish_time - elapsed_time;
			eta_hours = diff_seconds / 3600;
			eta_minutes = (diff_seconds / 60) % 60;
			eta_seconds = diff_seconds % 60;

			chunkmbs = (last_chunk / 1024 / 1024) / (double)(current.tv_sec-last.tv_sec);
			if (!STDIN || st->stdin_eof)
				print_verbose("\nPass %d / %d -- Elapsed Time: %02d:%02d:%02d. ETA: %02d:%02d:%02d. Compress Speed: %3.3fMB/s.\n",
					pass, passes, elapsed_hours, elapsed_minutes, elapsed_seconds,
					eta_hours, eta_minutes, eta_seconds, chunkmbs);
			else
				print_verbose("\nPass %d -- Elapsed Time: %02d:%02d:%02d. Compress Speed: %3.3fMB/s.\n",
					pass, elapsed_hours, elapsed_minutes, elapsed_seconds, chunkmbs);
		}
		last.tv_sec = current.tv_sec;
		last.tv_usec = current.tv_usec;

		if (st->chunk_size == len)
			control->eof = 1;
		rzip_chunk(control, st, fd_in, fd_out, offset, pct_base, pct_multiple);

		/* st->chunk_size may be shrunk in rzip_chunk */
		last_chunk = st->chunk_size;
		len -= st->chunk_size;
		if (unlikely(len > 0 && control->eof)) {
			close_streamout_threads(control);
			dealloc(st->hash_table);
			dealloc(st);
			failure("Wrote EOF to file yet chunk_size was shrunk, corrupting archive.\n");
		}

		/* Progressive STDOUT: finish all compress threads for this
		 * block, finalise magic / patch LRZC c_size, flush immutable
		 * block to the pipe, then continue with the next block. */
		if (STDOUT) {
			if (unlikely(!wait_streamout_threads(control))) {
				close_streamout_threads(control);
				dealloc(st->hash_table);
				dealloc(st);
				failure("Failed to wait_streamout_threads in rzip_fd\n");
			}
			/* LZMA props may clear magic_written after the first
			 * header write; rewrite once before the first flush. */
			if (!control->blocks_done && !control->magic_written) {
				if (unlikely(!write_magic(control))) {
					close_streamout_threads(control);
					dealloc(st->hash_table);
					dealloc(st);
					failure("Failed to finalise magic before flush\n");
				}
			}
			if (control->blocks_done > 0) {
				if (unlikely(!patch_lrzc_c_size(control, fd_out))) {
					close_streamout_threads(control);
					dealloc(st->hash_table);
					dealloc(st);
					failure("Failed to patch LRZC c_size in rzip_fd\n");
				}
			}
			if (unlikely(!flush_tmpout(control))) {
				close_streamout_threads(control);
				dealloc(st->hash_table);
				dealloc(st);
				failure("Failed to flush_tmpout after streaming block\n");
			}
			control->blocks_done++;
			/* Magic is immutable after the first block is emitted */
			control->magic_written = 1;
			print_maxverbose("Flushed streaming block %u (eof=%u)\n",
					 control->blocks_done, control->eof);
		}
	}

	if (likely(st->hash_table))
		dealloc(st->hash_table);
	if (unlikely(!close_streamout_threads(control))) {
		dealloc(st);
		failure("Failed to close_streamout_threads in rzip_fd\n");
	}

	if (!NO_MD5) {
		md5_thread_stop(control);
		/* Temporary workaround till someone fixes apple md5 */
		md5_finish_ctx(&control->ctx, control->md5_resblock);
		if (HASH_CHECK || MAX_VERBOSE) {
			print_output("MD5: ");
			for (j = 0; j < MD5_DIGEST_SIZE; j++)
				print_output("%02x", control->md5_resblock[j] & 0xFF);
			print_output("\n");
		}
		/* When encrypting data, we encrypt the MD5 value as well */
		if (ENCRYPT_AEAD) {
			uchar sealed[LRZ_AEAD_NONCE_LEN + MD5_DIGEST_SIZE + LRZ_AEAD_TAG_LEN];
			uchar aad[8];
			size_t aad_len = 0, slen = sizeof(sealed);

			/* Match stream AAD layout: ctx 0x03 = MD5 trailer */
			aad[0] = 'L'; aad[1] = 'R'; aad[2] = 'Z'; aad[3] = 'I';
			aad[4] = (uchar)control->major_version;
			aad[5] = (uchar)control->minor_version;
			aad[6] = 3;
			aad[7] = 0x03;
			aad_len = 8;
			if (unlikely(!lrz_aead_seal(control, LRZ_AEAD_KEY_DATA, aad, aad_len,
						    control->md5_resblock, MD5_DIGEST_SIZE,
						    sealed, &slen))) {
				dealloc(st);
				failure("Failed to AEAD-seal MD5 in rzip_fd\n");
			}
			if (unlikely(write_all(control, sealed, (i64)slen) != (i64)slen)) {
				dealloc(st);
				failure("Failed to write AEAD MD5 in rzip_fd\n");
			}
		} else {
			if (ENCRYPT)
				if (unlikely(!lrz_encrypt(control, control->md5_resblock, MD5_DIGEST_SIZE, control->salt_pass))) {
					dealloc(st);
					failure("Failed to lrz_encrypt in rzip_fd\n");
				}
			if (unlikely(write_all(control, control->md5_resblock, MD5_DIGEST_SIZE) != MD5_DIGEST_SIZE)) {
				dealloc(st);
				failure("Failed to write md5 in rzip_fd\n");
			}
		}
	}

	if (unlikely(!flush_tmpout(control))) {
			dealloc(st);
			failure("Failed to flush_tmpout in rzip_fd\n");
	}

	gettimeofday(&current, NULL);
	if (STDIN)
		s.st_size = control->st_size;
	tdiff = current.tv_sec - start.tv_sec;
	if (!tdiff)
		tdiff = 1;
	chunkmbs = (s.st_size / 1024 / 1024) / tdiff;

	fstat(fd_out, &s2);

	print_maxverbose("matches=%u match_bytes=%u\n",
	       (unsigned int)st->stats.matches, (unsigned int)st->stats.match_bytes);
	print_maxverbose("literals=%u literal_bytes=%u\n",
	       (unsigned int)st->stats.literals, (unsigned int)st->stats.literal_bytes);
	print_maxverbose("true_tag_positives=%u false_tag_positives=%u\n",
	       (unsigned int)st->stats.tag_hits, (unsigned int)st->stats.tag_misses);
	print_maxverbose("inserts=%u match %.3f\n",
	       (unsigned int)st->stats.inserts,
	       (1.0 + st->stats.match_bytes) / st->stats.literal_bytes);

	if (!STDIN)
		print_output("%s - ", control->infile);
	print_output("Compression Ratio: %.3f. Average Compression Speed: %6.3fMB/s.\n",
		       1.0 * s.st_size / s2.st_size, chunkmbs);

	clear_sslist(st);
	dealloc(st);
}
