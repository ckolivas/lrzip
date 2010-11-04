/*
   Copyright (C) Andrew Tridgell 1998,
   Con Kolivas 2006-2010

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
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/
/* rzip compression algorithm */
#include "rzip.h"

#define CHUNK_MULTIPLE (100 * 1024 * 1024)
#define CKSUM_CHUNK 1024*1024
#define GREAT_MATCH 1024
#define MINIMUM_MATCH 31

/* Hash table works as follows.  We start by throwing tags at every
 * offset into the table.  As it fills, we start eliminating tags
 * which don't have lower bits set to one (ie. first we eliminate all
 * even tags, then all tags divisible by four, etc.).  This ensures
 * that on average, all parts of the file are covered by the hash, if
 * sparsely. */
typedef i64 tag;

/* All zero means empty.  We might miss the first chunk this way. */
struct hash_entry {
	i64 offset;
	tag t;
};

/* Levels control hashtable size and bzip2 level. */
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

struct rzip_state {
	void *ss;
	struct level *level;
	tag hash_index[256];
	struct hash_entry *hash_table;
	i64 hash_bits;
	i64 hash_count;
	i64 hash_limit;
	tag minimum_tag_mask;
	i64 tag_clean_ptr;
	i64 last_match;
	i64 chunk_size;
	char chunk_bytes;
	uint32_t cksum;
	int fd_in, fd_out;
	int stdin_eof;
	struct {
		i64 inserts;
		i64 literals;
		i64 literal_bytes;
		i64 matches;
		i64 match_bytes;
		i64 tag_hits;
		i64 tag_misses;
	} stats;
};

struct sliding_buffer {
	uchar *buf_low;	/* The low window buffer */
	uchar *buf_high;/* "" high "" */
	i64 orig_offset;/* Where the original buffer started */
	i64 offset_high;/* What the current offset the high buffer has */
	i64 orig_size;	/* How big the full buffer would be */
	i64 size_low;	/* How big the low buffer is */
	i64 size_high;
	int fd;		/* The fd of the mmap */
} sb;	/* Sliding buffer */

static void remap_high_sb(i64 p)
{
	if (munmap(sb.buf_high, sb.size_high) != 0)
		fatal("Failed to munmap in remap_high_sb\n");
	sb.size_high = 4096; /* In case we shrunk it when we hit the end of the file */
	sb.offset_high = p;
	if ((sb.offset_high + sb.orig_offset) % 4096)
		sb.offset_high -= (sb.offset_high + sb.orig_offset) % 4096;
	if (sb.offset_high + sb.size_high > sb.orig_size)
		sb.size_high = sb.orig_size - sb.offset_high;
	sb.buf_high = (uchar *)mmap(NULL, sb.size_high, PROT_READ, MAP_SHARED, sb.fd, sb.orig_offset + sb.offset_high);
	if (sb.buf_high == MAP_FAILED)
		fatal("Failed to re mmap in remap_high_sb\n");
}

/* We use a "sliding mmap" to effectively read more than we can fit into the
 * compression window. This is done by using a maximally sized lower mmap at
 * the beginning of the block, and a one-page-sized mmap block that slides up
 * and down as is required for any offsets beyond the lower one. This is
 * 100x slower than mmap but makes it possible to have unlimited sized
 * compression windows. */
static uchar *get_sb(i64 p)
{
	if (p < sb.size_low)
		return (sb.buf_low + p);
	if (p >= sb.offset_high && p < (sb.offset_high + sb.size_high))
		return (sb.buf_high + (p - sb.offset_high));
	/* (p > sb.size_low &&  p < sb.offset_high) */
	remap_high_sb(p);
	return (sb.buf_high + (p - sb.offset_high));
}

static inline void put_u8(void *ss, int stream, uchar b)
{
	if (write_stream(ss, stream, &b, 1) != 0)
		fatal("Failed to put_u8\n");
}

static inline void put_u32(void *ss, int stream, uint32_t s)
{
	if (write_stream(ss, stream, (uchar *)&s, 4))
		fatal("Failed to put_u32\n");
}

/* Put a variable length of bytes dependant on how big the chunk is */
static inline void put_vchars(void *ss, int stream, i64 s, int length)
{
	int bytes;

	for (bytes = 0; bytes < length; bytes++) {
		int bits = bytes * 8;
		uchar sb = (s >> bits) & (i64)0XFF;

		put_u8(ss, stream, sb);
	}
}

static void put_header(void *ss, uchar head, i64 len)
{
	put_u8(ss, 0, head);
	put_vchars(ss, 0, len, 2);
}

static void put_match(struct rzip_state *st, i64 p, i64 offset, i64 len)
{
	do {
		i64 ofs;
		i64 n = len;
		if (n > 0xFFFF)
			n = 0xFFFF;

		ofs = (p - offset);
		put_header(st->ss, 1, n);
		put_vchars(st->ss, 0, ofs, st->chunk_bytes);

		st->stats.matches++;
		st->stats.match_bytes += n;
		len -= n;
		p += n;
		offset += n;
	} while (len);
}

/* write some data to a stream mmap encoded. Return -1 on failure */
int write_sbstream(void *ss, int stream, i64 p, i64 len)
{
	struct stream_info *sinfo = ss;

	while (len) {
		i64 n, i;

		n = MIN(sinfo->bufsize - sinfo->s[stream].buflen, len);

		for (i = 0; i < n; i++) {
			memcpy(sinfo->s[stream].buf+sinfo->s[stream].buflen + i,
			       get_sb(p + i), 1);
		}
		sinfo->s[stream].buflen += n;
		p += n;
		len -= n;
		if (sinfo->s[stream].buflen == sinfo->bufsize) {
			if (flush_buffer(sinfo, stream) != 0)
				return -1;
		}
	}
	return 0;
}

static void put_literal(struct rzip_state *st, i64 last, i64 p)
{
	do {
		i64 len = p - last;
		if (len > 0xFFFF)
			len = 0xFFFF;

		st->stats.literals++;
		st->stats.literal_bytes += len;

		put_header(st->ss, 0, len);

		if (len && write_sbstream(st->ss, 1, last, len) != 0)
			fatal("Failed to write_stream in put_literal\n");
		last += len;
	} while (p > last);
}

/* Could give false positive on offset 0.  Who cares. */
static int empty_hash(struct rzip_state *st, i64 h)
{
	return !st->hash_table[h].offset && !st->hash_table[h].t;
}

static i64 primary_hash(struct rzip_state *st, tag t)
{
	return t & ((1 << st->hash_bits) - 1);
}

static inline tag increase_mask(tag tag_mask)
{
	/* Get more precise. */
	return (tag_mask << 1) | 1;
}

static int minimum_bitness(struct rzip_state *st, tag t)
{
	tag better_than_min = increase_mask(st->minimum_tag_mask);

	if ((t & better_than_min) != better_than_min)
		return 1;
	return 0;
}

/* Is a going to be cleaned before b?  ie. does a have fewer low bits
 * set than b? */
static int lesser_bitness(tag a, tag b)
{
	tag mask;

	for (mask = 0; mask != (tag) - 1; mask = ((mask << 1) | 1)) {
		if ((a & b & mask) != mask)
			break;
	}
	return ((a & mask) < (b & mask));
}

/* If hash bucket is taken, we spill into next bucket(s).  Secondary hashing
   works better in theory, but modern caches make this 20% faster. */
static void insert_hash(struct rzip_state *st, tag t, i64 offset)
{
	i64 h, victim_h = 0, round = 0;
	/* If we need to kill one, this will be it. */
	static i64 victim_round = 0;

	h = primary_hash(st, t);
	while (!empty_hash(st, h)) {
		/* If this due for cleaning anyway, just replace it:
		   rehashing might move it behind tag_clean_ptr. */
		if (minimum_bitness(st, st->hash_table[h].t)) {
			st->hash_count--;
			break;
		}
		/* If we are better than current occupant, we can't
		   jump over it: it will be cleaned before us, and
		   noone would then find us in the hash table.  Rehash
		   it, then take its place. */
		if (lesser_bitness(st->hash_table[h].t, t)) {
			insert_hash(st, st->hash_table[h].t,
				    st->hash_table[h].offset);
			break;
		}

		/* If we have lots of identical patterns, we end up
		   with lots of the same hash number.  Discard random. */
		if (st->hash_table[h].t == t) {
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

		h++;
		h &= ((1 << st->hash_bits) - 1);
	}

	st->hash_table[h].t = t;
	st->hash_table[h].offset = offset;
}

/* Eliminate one hash entry with minimum number of lower bits set.
   Returns tag requirement for any new entries. */
static tag clean_one_from_hash(struct rzip_state *st)
{
	tag better_than_min;

again:
	better_than_min = increase_mask(st->minimum_tag_mask);
	if (!st->tag_clean_ptr)
		print_maxverbose("\nStarting sweep for mask %u\n", (unsigned int)st->minimum_tag_mask);

	for (; st->tag_clean_ptr < (1U << st->hash_bits); st->tag_clean_ptr++) {
		if (empty_hash(st, st->tag_clean_ptr))
			continue;
		if ((st->hash_table[st->tag_clean_ptr].t & better_than_min)
		    != better_than_min) {
			st->hash_table[st->tag_clean_ptr].offset = 0;
			st->hash_table[st->tag_clean_ptr].t = 0;
			st->hash_count--;
			return better_than_min;
		}
	}

	/* We hit the end: everthing in hash satisfies the better mask. */
	st->minimum_tag_mask = better_than_min;
	st->tag_clean_ptr = 0;
	goto again;
}

static inline tag next_tag(struct rzip_state *st, i64 p, tag t)
{
	t ^= st->hash_index[*get_sb(p - 1)];
	t ^= st->hash_index[*get_sb(p + MINIMUM_MATCH - 1)];
	return t;
}

static inline tag full_tag(struct rzip_state *st, i64 p)
{
	tag ret = 0;
	int i;

	for (i = 0; i < MINIMUM_MATCH; i++)
		ret ^= st->hash_index[*get_sb(p + i)];
	return ret;
}

static inline i64 match_len(struct rzip_state *st, i64 p0, i64 op, i64 end,
			    i64 *rev)
{
	i64 p = p0;
	i64 len = 0;

	if (op >= p0)
		return 0;

	while ((*get_sb(p) == *get_sb(op)) && (p < end)) {
		p++;
		op++;
	}
	len = p - p0;

	p = p0;
	op -= len;

	end = 0;
	if (end < st->last_match)
		end = st->last_match;

	while (p > end && op > 0 && *get_sb(op - 1) == *get_sb(p-1)) {
		op--;
		p--;
	}

	(*rev) = p0 - p;
	len += p0 - p;

	if (len < MINIMUM_MATCH)
		return 0;

	return len;
}

static i64 find_best_match(struct rzip_state *st, tag t, i64 p, i64 end,
			   i64 *offset, i64 *reverse)
{
	i64 length = 0;
	i64 h, best_h;
	i64 rev;

	rev = 0;
	*reverse = 0;

	/* Could optimise: if lesser goodness, can stop search.  But
	 * chains are usually short anyway. */
	h = primary_hash(st, t);
	while (!empty_hash(st, h)) {
		i64 mlen;

		if (t == st->hash_table[h].t) {
			mlen = match_len(st, p, st->hash_table[h].offset, end,
					 &rev);

			if (mlen)
				st->stats.tag_hits++;
			else
				st->stats.tag_misses++;

			if (mlen >= length) {
				length = mlen;
				(*offset) = st->hash_table[h].offset - rev;
				(*reverse) = rev;
				best_h = h;
			}
		}

		h++;
		h &= ((1 << st->hash_bits) - 1);
	}

	return length;
}

static void show_distrib(struct rzip_state *st)
{
	i64 primary = 0;
	i64 total = 0;
	i64 i;

	for (i = 0; i < (1U << st->hash_bits); i++) {
		if (empty_hash(st, i))
			continue;
		total++;
		if (primary_hash(st, st->hash_table[i].t) == i)
			primary++;
	}

	if (total != st->hash_count)
		print_output("/tWARNING: hash_count says total %lld\n", st->hash_count);

	print_output("\t%lld total hashes -- %lld in primary bucket (%-2.3f%%)\n", total, primary,
	       primary*100.0/total);
}

static void hash_search(struct rzip_state *st, double pct_base, double pct_multiple)
{
	i64 cksum_limit = 0, pct, lastpct=0;
	i64 p, end;
	tag t = 0;
	struct {
		i64 p;
		i64 ofs;
		i64 len;
	} current;

	tag tag_mask = (1 << st->level->initial_freq) - 1;

	if (st->hash_table)
		memset(st->hash_table, 0, sizeof(st->hash_table[0]) * (1<<st->hash_bits));
	else {
		i64 hashsize = st->level->mb_used *
				(1024 * 1024 / sizeof(st->hash_table[0]));
		for (st->hash_bits = 0; (1U << st->hash_bits) < hashsize; st->hash_bits++);

		print_maxverbose("hashsize = %lld.  bits = %lld. %luMB\n",
				 hashsize, st->hash_bits, st->level->mb_used);

		/* 66% full at max. */
		st->hash_limit = (1 << st->hash_bits) / 3 * 2;
		st->hash_table = calloc(sizeof(st->hash_table[0]), (1 << st->hash_bits));
	}

	if (!st->hash_table)
		fatal("Failed to allocate hash table in hash_search\n");

	st->minimum_tag_mask = tag_mask;
	st->tag_clean_ptr = 0;
	st->cksum = 0;
	st->hash_count = 0;

	p = 0;
	end = st->chunk_size - MINIMUM_MATCH;
	st->last_match = p;
	current.len = 0;
	current.p = p;
	current.ofs = 0;

	t = full_tag(st, p);

	while (p < end) {
		i64 offset = 0;
		i64 mlen;
		i64 reverse;

		p++;
		t = next_tag(st, p, t);

		/* Don't look for a match if there are no tags with
		   this number of bits in the hash table. */
		if ((t & st->minimum_tag_mask) != st->minimum_tag_mask)
			continue;

		mlen = find_best_match(st, t, p, end, &offset, &reverse);

		/* Only insert occasionally into hash. */
		if ((t & tag_mask) == tag_mask) {
			st->stats.inserts++;
			st->hash_count++;
			insert_hash(st, t, p);
			if (st->hash_count > st->hash_limit)
				tag_mask = clean_one_from_hash(st);
		}

		if (mlen > current.len) {
			current.p = p - reverse;
			current.len = mlen;
			current.ofs = offset;
		}

		if ((current.len >= GREAT_MATCH || p >= current.p + MINIMUM_MATCH)
		    && current.len >= MINIMUM_MATCH) {
			if (st->last_match < current.p)
				put_literal(st, st->last_match, current.p);
			put_match(st, current.p, current.ofs, current.len);
			st->last_match = current.p + current.len;
			current.p = p = st->last_match;
			current.len = 0;
			t = full_tag(st, p);
		}

		if (p % 100 == 0) {
			pct = pct_base + (pct_multiple * (100.0 * p) /
			      st->chunk_size);
			if (pct != lastpct) {
				struct stat s1, s2;

				fstat(st->fd_in, &s1);
				fstat(st->fd_out, &s2);
				if (!STDIN)
					print_progress("%2lld%%\r", pct);
				lastpct = pct;
			}
		}

		if (p > (i64)cksum_limit) {
			i64 i, n = st->chunk_size - p;

			for (i = 0; i < n; i++)
				st->cksum = CrcUpdate(st->cksum, get_sb(cksum_limit + i), 1);
			cksum_limit += n;
		}
	}


	if (MAX_VERBOSE)
		show_distrib(st);

	if (st->last_match < st->chunk_size)
		put_literal(st, st->last_match, st->chunk_size);

	if (st->chunk_size > cksum_limit) {
		i64 i, n = st->chunk_size - cksum_limit;

		for (i = 0; i < n; i++)
			st->cksum = CrcUpdate(st->cksum, get_sb(cksum_limit + i), 1);
		cksum_limit += n;
	}

	put_literal(st, 0, 0);
	put_u32(st->ss, 0, st->cksum);
}


static void init_hash_indexes(struct rzip_state *st)
{
	int i;

	for (i = 0; i < 256; i++)
		st->hash_index[i] = ((random() << 16) ^ random());
}

extern const i64 one_g;

static inline void *fake_mremap(void *old_address, size_t old_size, size_t new_size, int flags)
{
	flags = 0;
	munmap(old_address, old_size);
	return mmap(old_address, new_size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
}

/* stdin is not file backed so we have to emulate the mmap by mapping
 * anonymous ram and reading stdin into it. It means the maximum ram
 * we can use will be less but we will already have determined this in
 * rzip_chunk */
static void mmap_stdin(uchar *buf, struct rzip_state *st)
{
	i64 len = st->chunk_size;
	uchar *offset_buf = buf;
	ssize_t ret;
	i64 total;

	total = 0;
	print_verbose("Reading stdin into mmapped ram...\n");
	while (len > 0) {
		if (len > one_g)
			ret = one_g;
		else
			ret = len;
		ret = read(0, offset_buf, (size_t)ret);
		if (ret < 0)
			fatal("Failed to read in mmap_stdin\n");
		total += ret;
		if (ret == 0) {
			/* Should be EOF */
			print_maxverbose("Shrinking chunk to %lld\n", total);
			buf = mremap(buf, st->chunk_size, total, 0);
			if (buf == MAP_FAILED)
				fatal("Failed to remap to smaller buf in mmap_stdin\n");
			st->chunk_size = total;
			control.st_size += total;
			st->stdin_eof = 1;
			break;
		}
		offset_buf += ret;
		len -= ret;
	}
}

static void init_sliding_mmap(struct rzip_state *st, int fd_in, i64 offset)
{
	i64 size = st->chunk_size;

	sb.orig_offset = offset;
retry:
	/* Mmapping anonymously first will tell us how much ram we can use in
	 * advance and zeroes it which has a defragmenting effect on ram
	 * before the real read in. We can map a lot more file backed ram than
	 * anonymous ram so do not do this preallocation in MAXRAM mode. Using
	 * the larger mmapped window will cause a lot more ram trashing of the
	 * system so we do not use MAXRAM mode by default. */
	if (!MAXRAM || STDIN) {
		sb.buf_low = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
		/* Better to shrink the window to the largest size that works than fail */
		if (sb.buf_low == MAP_FAILED) {
			size = size / 10 * 9;
			size -= size % 4096; /* Round to page size */
			if (!size)
				fatal("Unable to mmap any ram\n");
			goto retry;
		}
		print_maxverbose("Succeeded in preallocating %lld sized mmap\n", size);
		if (!STDIN) {
			if (munmap(sb.buf_low, size) != 0)
				fatal("Failed to munmap\n");
		} else
			st->chunk_size = size;
	}
	if (!STDIN) {
		sb.buf_low = (uchar *)mmap(sb.buf_low, size, PROT_READ, MAP_SHARED, fd_in, offset);
		if (sb.buf_low == MAP_FAILED) {
			size = size / 10 * 9;
			size -= size % 4096; /* Round to page size */
			if (!size)
				fatal("Unable to mmap any ram\n");
			goto retry;
		}
	} else
		mmap_stdin(sb.buf_low, st);
	print_maxverbose("Succeeded in allocating %lld sized mmap\n", size);
	if (size < st->chunk_size) {
		if (UNLIMITED && !STDIN)
			print_verbose("File is beyond window size, will proceed MUCH slower in unlimited mode beyond\nthe window size with a sliding_mmap\n");
		else {
			print_verbose("Needed to shrink window size to %lld\n", size);
			st->chunk_size = size;
		}
	}
	if (UNLIMITED && !STDIN) {
		sb.buf_high = (uchar *)mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd_in, offset);
		if (sb.buf_high == MAP_FAILED)
			fatal("Unable to mmap buf_high in init_sliding_mmap\n");
		sb.size_high = 4096;
		sb.offset_high = 0;
	}
	sb.size_low = size;
	sb.orig_size = st->chunk_size;
	sb.fd = fd_in;
}

/* compress a chunk of an open file. Assumes that the file is able to
   be mmap'd and is seekable */
static void rzip_chunk(struct rzip_state *st, int fd_in, int fd_out, i64 offset,
		       double pct_base, double pct_multiple)
{
	init_sliding_mmap(st, fd_in, offset);
	st->ss = open_stream_out(fd_out, NUM_STREAMS, st->chunk_size);
	if (!st->ss)
		fatal("Failed to open streams in rzip_chunk\n");
	hash_search(st, pct_base, pct_multiple);
	/* unmap buffer before closing and reallocating streams */
	if (munmap(sb.buf_low, sb.size_low) != 0)
		fatal("Failed to munmap in rzip_chunk\n");
	if (UNLIMITED && !STDIN) {
		if (munmap(sb.buf_high, sb.size_high) != 0)
			fatal("Failed to munmap in rzip_chunk\n");
	}

	if (close_stream_out(st->ss) != 0)
		fatal("Failed to flush/close streams in rzip_chunk\n");
}

/* compress a whole file chunks at a time */
void rzip_fd(int fd_in, int fd_out)
{
	/* add timers for ETA estimates
	 * Base it off the file size and number of iterations required
	 * depending on compression window size
	 * Track elapsed time and estimated time to go
	 * If file size < compression window, can't do
	 */
	struct timeval current, start, last;
	struct stat s, s2;
	struct rzip_state *st;
	i64 len = 0, chunk_window, last_chunk = 0;
	int pass = 0, passes;
	unsigned int eta_hours, eta_minutes, eta_seconds, elapsed_hours,
		     elapsed_minutes, elapsed_seconds;
	double finish_time, elapsed_time, chunkmbs;

	st = calloc(sizeof(*st), 1);
	if (!st)
		fatal("Failed to allocate control state in rzip_fd\n");

	if (LZO_COMPRESS) {
		if (lzo_init() != LZO_E_OK)
			fatal("lzo_init() failed\n");
	}

	if (fstat(fd_in, &s))
		fatal("Failed to stat fd_in in rzip_fd - %s\n", strerror(errno));

	if (!STDIN) {
		len = control.st_size = s.st_size;
		print_verbose("File size: %lld\n", len);
	} else
		control.st_size = 0;

	if (control.window)
		chunk_window = control.window * CHUNK_MULTIPLE;
	else {
		if (STDIN)
			chunk_window = control.ramsize;
		else
			chunk_window = len;
	}
	st->chunk_size = chunk_window;

	st->level = &levels[control.compression_level];
	st->fd_in = fd_in;
	st->fd_out = fd_out;
	st->stdin_eof = 0;

	init_hash_indexes(st);

	passes = 1 + s.st_size / chunk_window;

	/* set timers and chunk counter */
	last.tv_sec = last.tv_usec = 0;
	gettimeofday(&start, NULL);

	while (len > 0 || (STDIN && !st->stdin_eof)) {
		double pct_base, pct_multiple;
		int bits = 8;

		/* Flushing the dirty data will decrease our chances of
		 * running out of memory when we allocate ram again on the
		 * next chunk. It will also prevent thrashing on-disk due to
		 * concurrent reads and writes if we're on the same device. */
		if (last_chunk)
			print_verbose("Flushing data to disk.\n");
		fsync(fd_out);
		if (st->chunk_size > len && !STDIN)
			st->chunk_size = len;
		print_maxverbose("Chunk size: %lld\n", st->chunk_size);

		/* Determine the chunk byte width and write it to the file
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
		if (write(fd_out, &st->chunk_bytes, 1) != 1)
			fatal("Failed to write chunk_bytes size in rzip_fd\n");

		pct_base = (100.0 * (s.st_size - len)) / s.st_size;
		pct_multiple = ((double)st->chunk_size) / s.st_size;
		pass++;

		gettimeofday(&current, NULL);
		/* this will count only when size > window */
		if (last.tv_sec > 0) {
			elapsed_time = current.tv_sec - start.tv_sec;
			finish_time = elapsed_time / (pct_base / 100.0);
			elapsed_hours = (unsigned int)(elapsed_time) / 3600;
			elapsed_minutes = (unsigned int)(elapsed_time - elapsed_hours * 3600) / 60;
			elapsed_seconds = (unsigned int) elapsed_time - elapsed_hours * 60 - elapsed_minutes * 60;
			eta_hours = (unsigned int)(finish_time - elapsed_time) / 3600;
			eta_minutes = (unsigned int)((finish_time - elapsed_time) - eta_hours * 3600) / 60;
			eta_seconds = (unsigned int)(finish_time - elapsed_time) - eta_hours * 60 - eta_minutes * 60;
			chunkmbs=(last_chunk / 1024 / 1024) / (double)(current.tv_sec-last.tv_sec);
			print_verbose("\nPass %d / %d -- Elapsed Time: %02d:%02d:%02d. ETA: %02d:%02d:%02d. Compress Speed: %3.3fMB/s.\n",
					pass, passes, elapsed_hours, elapsed_minutes, elapsed_seconds,
					eta_hours, eta_minutes, eta_seconds, chunkmbs);
		}
		last.tv_sec = current.tv_sec;
		last.tv_usec = current.tv_usec;
		rzip_chunk(st, fd_in, fd_out, s.st_size - len, pct_base, pct_multiple);
		/* st->chunk_bytes may be shrunk in rzip_chunk */
		last_chunk = st->chunk_size;
		len -= st->chunk_size;
	}

	gettimeofday(&current, NULL);
	chunkmbs = (s.st_size / 1024 / 1024) / ((double)(current.tv_sec-start.tv_sec)? : 1);

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
		print_progress("%s - ", control.infile);
	print_progress("Compression Ratio: %.3f. Average Compression Speed: %6.3fMB/s.\n",
		       1.0 * s.st_size / s2.st_size, chunkmbs);

	free(st);
}
