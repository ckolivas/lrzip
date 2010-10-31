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
	uchar *last_match;
	i64 chunk_size;
	char chunk_bytes;
	uint32_t cksum;
	int fd_in, fd_out;
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

static void put_match(struct rzip_state *st, uchar *p, uchar *buf, i64 offset, i64 len)
{
	do {
		i64 ofs;
		i64 n = len;
		if (n > 0xFFFF) n = 0xFFFF;

		ofs = (p - (buf + offset));
		put_header(st->ss, 1, n);
		put_vchars(st->ss, 0, ofs, st->chunk_bytes);

		st->stats.matches++;
		st->stats.match_bytes += n;
		len -= n;
		p += n;
		offset += n;
	} while (len);
}

static void put_literal(struct rzip_state *st, uchar *last, uchar *p)
{
	do {
		i64 len = (i64)(p - last);
		if (len > 0xFFFF) len = 0xFFFF;

		st->stats.literals++;
		st->stats.literal_bytes += len;

		put_header(st->ss, 0, len);

		if (len && write_stream(st->ss, 1, last, len) != 0)
			fatal(NULL);
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
			if (round == victim_round) {
				victim_h = h;
			}
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
	if (control.flags & FLAG_VERBOSITY_MAX) {
		if (!st->tag_clean_ptr)
			fprintf(control.msgout, "\nStarting sweep for mask %u\n", (unsigned int)st->minimum_tag_mask);
	}

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

static inline tag next_tag(struct rzip_state *st, uchar *p, tag t)
{
	t ^= st->hash_index[p[-1]];
	t ^= st->hash_index[p[-1]];
	return t;
}

static inline tag full_tag(struct rzip_state *st, uchar *p)
{
	tag ret = 0;
	int i;

	for (i = 0; i < MINIMUM_MATCH; i++)
		ret ^= st->hash_index[p[i]];
	return ret;
}

static inline i64 match_len(struct rzip_state *st,
			    uchar *p0, uchar *op, uchar *buf, uchar *end, i64 *rev)
{
	uchar *p = p0;
	i64 len = 0;

	if (op >= p0)
		return 0;

	while ((*p == *op) && (p < end)) {
		p++;
		op++;
	}
	len = p - p0;

	p = p0;
	op -= len;

	end = buf;
	if (end < st->last_match)
		end = st->last_match;

	while (p > end && op > buf && op[-1] == p[-1]) {
		op--;
		p--;
	}

	(*rev) = p0 - p;
	len += p0 - p;

	if (len < MINIMUM_MATCH)
		return 0;

	return len;
}

static i64 find_best_match(struct rzip_state *st,
			   tag t, uchar *p, uchar *buf, uchar *end,
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
			mlen = match_len(st, p, buf+st->hash_table[h].offset,
					 buf, end, &rev);

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
		fprintf(control.msgout, "/tWARNING: hash_count says total %lld\n", st->hash_count);

	fprintf(control.msgout, "\t%lld total hashes -- %lld in primary bucket (%-2.3f%%)\n", total, primary,
	       primary*100.0/total);
}

static void hash_search(struct rzip_state *st, uchar *buf,
			double pct_base, double pct_multiple)
{
	i64 cksum_limit = 0, pct, lastpct=0;
	uchar *p, *end;
	tag t = 0;
	struct {
		uchar *p;
		i64 ofs;
		i64 len;
	} current;

	tag tag_mask = (1 << st->level->initial_freq) - 1;

	if (st->hash_table) {
		memset(st->hash_table, 0, sizeof(st->hash_table[0]) * (1<<st->hash_bits));
	} else {
		i64 hashsize = st->level->mb_used *
				(1024 * 1024 / sizeof(st->hash_table[0]));
		for (st->hash_bits = 0; (1U << st->hash_bits) < hashsize; st->hash_bits++);

		if (control.flags & FLAG_VERBOSITY_MAX)
			fprintf(control.msgout, "hashsize = %lld.  bits = %lld. %luMB\n",
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

	p = buf;
	end = buf + st->chunk_size - MINIMUM_MATCH;
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

		mlen = find_best_match(st, t, p, buf, end, &offset, &reverse);

		/* Only insert occasionally into hash. */
		if ((t & tag_mask) == tag_mask) {
			st->stats.inserts++;
			st->hash_count++;
			insert_hash(st, t, (i64)(p - buf));
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
			put_match(st, current.p, buf, current.ofs, current.len);
			st->last_match = current.p + current.len;
			current.p = p = st->last_match;
			current.len = 0;
			t = full_tag(st, p);
		}

		if ((control.flags & FLAG_SHOW_PROGRESS) && (p - buf) % 100 == 0) {
			pct = pct_base + (pct_multiple * (100.0 * (p - buf)) /
			      st->chunk_size);
			if (pct != lastpct) {
				struct stat s1, s2;

				fstat(st->fd_in, &s1);
				fstat(st->fd_out, &s2);
				fprintf(control.msgout, "%2lld%%\r", pct);
				fflush(control.msgout);
				lastpct = pct;
			}
		}

		if (p - buf > (i64)cksum_limit) {
			i64 n = st->chunk_size - (p - buf);
			st->cksum = CrcUpdate(st->cksum, buf + cksum_limit, n);
			cksum_limit += n;
		}
	}


	if (control.flags & FLAG_VERBOSITY_MAX)
		show_distrib(st);

	if (st->last_match < buf + st->chunk_size)
		put_literal(st, st->last_match, buf + st->chunk_size);

	if (st->chunk_size > cksum_limit) {
		i64 n = st->chunk_size - cksum_limit;
		st->cksum = CrcUpdate(st->cksum, buf+cksum_limit, n);
		cksum_limit += n;
	}

	put_literal(st, NULL, 0);
	put_u32(st->ss, 0, st->cksum);
}


static void init_hash_indexes(struct rzip_state *st)
{
	int i;

	for (i = 0; i < 256; i++)
		st->hash_index[i] = ((random() << 16) ^ random());
}

/* compress a chunk of an open file. Assumes that the file is able to
   be mmap'd and is seekable */
static void rzip_chunk(struct rzip_state *st, int fd_in, int fd_out, i64 offset,
		       double pct_base, double pct_multiple, i64 limit)
{
	uchar *buf;

	/* Malloc'ing first will tell us if we can allocate this much ram
	 * faster than slowly reading in the file and then failing. Filling
	 * it with zeroes has a defragmenting effect on ram before the real
	 * read in. */
	if (control.flags & FLAG_VERBOSE)
		fprintf(control.msgout, "Preallocating ram...\n");
	buf = malloc(st->chunk_size);
	if (!buf)
		fatal("Failed to premalloc in rzip_chunk\n");
	if (!memset(buf, 0, st->chunk_size))
		fatal("Failed to memset in rzip_chunk\n");
	free(buf);
	if (control.flags & FLAG_VERBOSE)
		fprintf(control.msgout, "Reading file into mmapped ram...\n");
	buf = (uchar *)mmap(buf, st->chunk_size, PROT_READ, MAP_SHARED, fd_in, offset);
	if (buf == (uchar *)-1)
		fatal("Failed to map buffer in rzip_chunk\n");

	st->ss = open_stream_out(fd_out, NUM_STREAMS, limit);
	if (!st->ss)
		fatal("Failed to open streams in rzip_chunk\n");
	hash_search(st, buf, pct_base, pct_multiple);
	/* unmap buffer before closing and reallocating streams */
	munmap(buf, st->chunk_size);

	if (close_stream_out(st->ss) != 0)
		fatal("Failed to flush/close streams in rzip_chunk\n");
}

/* Windows must be the width of _SC_PAGE_SIZE for offset to work in mmap */
static void round_to_page_size(i64 *chunk)
{
	unsigned long page_size = sysconf(_SC_PAGE_SIZE);
	i64 pages = *chunk / page_size + 1;

	*chunk = pages * page_size;
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
	i64 len, chunk_window, last_chunk = 0;
	int pass = 0, passes, bits = 8;
	unsigned int eta_hours, eta_minutes, eta_seconds, elapsed_hours,
		     elapsed_minutes, elapsed_seconds;
	double finish_time, elapsed_time, chunkmbs;

	st = calloc(sizeof(*st), 1);
	if (!st)
		fatal("Failed to allocate control state in rzip_fd\n");

	if (control.flags & FLAG_LZO_COMPRESS) {
		if (lzo_init() != LZO_E_OK)
			fatal("lzo_init() failed\n");
	}

	if (fstat(fd_in, &s))
		fatal("Failed to stat fd_in in rzip_fd - %s\n", strerror(errno));

	len = s.st_size;
	if (control.flags & FLAG_VERBOSE)
		fprintf(control.msgout, "File size: %lld\n", len);
	while (len >> bits > 0)
		bits++;
	st->chunk_bytes = bits / 8;
	if (bits % 8)
		st->chunk_bytes++;
	if (control.flags & FLAG_VERBOSE)
		fprintf(control.msgout, "Byte width: %d\n", st->chunk_bytes);

	chunk_window = control.window * CHUNK_MULTIPLE;

	st->level = &levels[MIN(9, control.window)];
	st->fd_in = fd_in;
	st->fd_out = fd_out;

	init_hash_indexes(st);

	passes = 1 + s.st_size / chunk_window;

	/* set timers and chunk counter */
	last.tv_sec = last.tv_usec = 0;
	gettimeofday(&start, NULL);

	while (len > 0) {
		double pct_base, pct_multiple;
		i64 chunk, limit = 0;

		/* Flushing the dirty data will decrease our chances of
		 * running out of memory when we allocate ram again on the
		 * next chunk. It will also prevent thrashing on-disk due to
		 * concurrent reads and writes if we're on the same device. */
		if (last_chunk && control.flags & FLAG_VERBOSE)
			fprintf(control.msgout, "Flushing data to disk.\n");
		fsync(fd_out);
		chunk = chunk_window;
		if (chunk > len)
			chunk = len;
		round_to_page_size(&chunk);
		limit = chunk;
		st->chunk_size = chunk;
		if (control.flags & FLAG_VERBOSE)
			fprintf(control.msgout, "Chunk size: %lld\n\n", chunk);

		pct_base = (100.0 * (s.st_size - len)) / s.st_size;
		pct_multiple = ((double)chunk) / s.st_size;
		pass++;

		gettimeofday(&current, NULL);
		/* this will count only when size > window */
		if (last.tv_sec > 0) {
			if (control.flags & FLAG_VERBOSE) {
				elapsed_time = current.tv_sec - start.tv_sec;
				finish_time = elapsed_time / (pct_base / 100.0);
				elapsed_hours = (unsigned int)(elapsed_time) / 3600;
				elapsed_minutes = (unsigned int)(elapsed_time - elapsed_hours * 3600) / 60;
				elapsed_seconds = (unsigned int) elapsed_time - elapsed_hours * 60 - elapsed_minutes * 60;
				eta_hours = (unsigned int)(finish_time - elapsed_time) / 3600;
				eta_minutes = (unsigned int)((finish_time - elapsed_time) - eta_hours * 3600) / 60;
				eta_seconds = (unsigned int)(finish_time - elapsed_time) - eta_hours * 60 - eta_minutes * 60;
				chunkmbs=(last_chunk / 1024 / 1024) / (double)(current.tv_sec-last.tv_sec);
				fprintf(control.msgout, "\nPass %d / %d -- Elapsed Time: %02d:%02d:%02d. ETA: %02d:%02d:%02d. Compress Speed: %3.3fMB/s.\n",
						pass, passes, elapsed_hours, elapsed_minutes, elapsed_seconds,
						eta_hours, eta_minutes, eta_seconds, chunkmbs);
			}
		}
		last.tv_sec = current.tv_sec;
		last.tv_usec = current.tv_usec;
		last_chunk = chunk;
		rzip_chunk(st, fd_in, fd_out, s.st_size - len, pct_base, pct_multiple, limit);
		len -= chunk;
	}

	gettimeofday(&current, NULL);
	chunkmbs = (s.st_size / 1024 / 1024) / ((double)(current.tv_sec-start.tv_sec)? : 1);

	fstat(fd_out, &s2);

	if (control.flags & FLAG_VERBOSITY_MAX) {
		fprintf(control.msgout, "matches=%u match_bytes=%u\n",
		       (unsigned int)st->stats.matches, (unsigned int)st->stats.match_bytes);
		fprintf(control.msgout, "literals=%u literal_bytes=%u\n",
		       (unsigned int)st->stats.literals, (unsigned int)st->stats.literal_bytes);
		fprintf(control.msgout, "true_tag_positives=%u false_tag_positives=%u\n",
		       (unsigned int)st->stats.tag_hits, (unsigned int)st->stats.tag_misses);
		fprintf(control.msgout, "inserts=%u match %.3f\n",
		       (unsigned int)st->stats.inserts,
		       (1.0 + st->stats.match_bytes) / st->stats.literal_bytes);
	}

	if (control.flags & FLAG_SHOW_PROGRESS) {
		if (!(control.flags & FLAG_STDIN))
			fprintf(control.msgout, "%s - ", control.infile);
		fprintf(control.msgout, "Compression Ratio: %.3f. Average Compression Speed: %6.3fMB/s.\n",
		        1.0 * s.st_size / s2.st_size, chunkmbs);
	}

	if (st->hash_table)
		free(st->hash_table);
	free(st);
}
