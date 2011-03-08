/*
   Copyright (C) 2006-2011 Con Kolivas
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
	i64 mmap_size;
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
	i64 offset_low;	/* What the current offset the low buffer has */
	i64 offset_high;/* "" high buffer "" */
	i64 offset_search;/* Where the search is up to */
	i64 orig_size;	/* How big the full buffer would be */
	i64 size_low;	/* How big the low buffer is */
	i64 size_high;	/* "" high "" */
	i64 high_length;/* How big the high buffer should be */
	int fd;		/* The fd of the mmap */
} sb;	/* Sliding buffer */

static void remap_low_sb(rzip_control *control)
{
	i64 new_offset;

	new_offset = sb.offset_search;
	round_to_page(&new_offset);
	print_maxverbose("Sliding main buffer to offset %lld\n", new_offset);
	if (unlikely(munmap(sb.buf_low, sb.size_low)))
		fatal("Failed to munmap in remap_low_sb\n");
	if (new_offset + sb.size_low > sb.orig_size)
		sb.size_low = sb.orig_size - new_offset;
	sb.offset_low = new_offset;
	sb.buf_low = (uchar *)mmap(sb.buf_low, sb.size_low, PROT_READ, MAP_SHARED, sb.fd, sb.orig_offset + sb.offset_low);
	if (unlikely(sb.buf_low == MAP_FAILED))
		fatal("Failed to re mmap in remap_low_sb\n");
}

static inline void remap_high_sb(rzip_control *control, i64 p)
{
	if (unlikely(munmap(sb.buf_high, sb.size_high)))
		fatal("Failed to munmap in remap_high_sb\n");
	sb.size_high = sb.high_length; /* In case we shrunk it when we hit the end of the file */
	sb.offset_high = p;
	/* Make sure offset is rounded to page size of total offset */
	sb.offset_high -= (sb.offset_high + sb.orig_offset) % control->page_size;
	if (unlikely(sb.offset_high + sb.size_high > sb.orig_size))
		sb.size_high = sb.orig_size - sb.offset_high;
	sb.buf_high = (uchar *)mmap(sb.buf_high, sb.size_high, PROT_READ, MAP_SHARED, sb.fd, sb.orig_offset + sb.offset_high);
	if (unlikely(sb.buf_high == MAP_FAILED))
		fatal("Failed to re mmap in remap_high_sb\n");
}

/* We use a "sliding mmap" to effectively read more than we can fit into the
 * compression window. This is done by using a maximally sized lower mmap at
 * the beginning of the block which slides up once the hash search moves beyond
 * it, and a 64k mmap block that slides up and down as is required for any
 * offsets outside the range of the lower one. This is much slower than mmap
 * but makes it possible to have unlimited sized compression windows. */
static uchar *get_sb(rzip_control *control, i64 p)
{
	i64 low_end = sb.offset_low + sb.size_low;

	if (unlikely(sb.offset_search > low_end))
		remap_low_sb(control);
	if (p >= sb.offset_low && p < low_end)
		return (sb.buf_low + p - sb.offset_low);
	if (p >= sb.offset_high && p < (sb.offset_high + sb.size_high))
		return (sb.buf_high + (p - sb.offset_high));
	/* p is not within the low or high buffer range */
	remap_high_sb(control, p);
	return (sb.buf_high + (p - sb.offset_high));
}

/* All put_u8/u32/vchars go to stream 0 */
static inline void put_u8(rzip_control *control, void *ss, uchar b)
{
	if (unlikely(write_stream(control, ss, 0, &b, 1)))
		fatal("Failed to put_u8\n");
}

static inline void put_u32(rzip_control *control, void *ss, uint32_t s)
{
	if (unlikely(write_stream(control, ss, 0, (uchar *)&s, 4)))
		fatal("Failed to put_u32\n");
}

/* Put a variable length of bytes dependant on how big the chunk is */
static inline void put_vchars(rzip_control *control, void *ss, i64 s, int length)
{
	int bytes;

	for (bytes = 0; bytes < length; bytes++) {
		int bits = bytes * 8;
		uchar sb = (s >> bits) & (i64)0XFF;

		put_u8(control, ss, sb);
	}
}

static void put_header(rzip_control *control, void *ss, uchar head, i64 len)
{
	put_u8(control, ss, head);
	put_vchars(control, ss, len, 2);
}

static void put_match(rzip_control *control, struct rzip_state *st, i64 p, i64 offset, i64 len)
{
	do {
		i64 ofs;
		i64 n = len;
		if (n > 0xFFFF)
			n = 0xFFFF;

		ofs = (p - offset);
		put_header(control, st->ss, 1, n);
		put_vchars(control, st->ss, ofs, st->chunk_bytes);

		st->stats.matches++;
		st->stats.match_bytes += n;
		len -= n;
		p += n;
		offset += n;
	} while (len);
}

/* write some data to a stream mmap encoded. Return -1 on failure */
int write_sbstream(rzip_control *control, void *ss, int stream, i64 p, i64 len)
{
	struct stream_info *sinfo = ss;

	while (len) {
		i64 n, i;

		n = MIN(sinfo->bufsize - sinfo->s[stream].buflen, len);

		for (i = 0; i < n; i++) {
			memcpy(sinfo->s[stream].buf + sinfo->s[stream].buflen + i,
			       get_sb(control, p + i), 1);
		}
		sinfo->s[stream].buflen += n;
		p += n;
		len -= n;

		if (sinfo->s[stream].buflen == sinfo->bufsize)
			flush_buffer(control, sinfo, stream);
	}
	return 0;
}

static void put_literal(rzip_control *control, struct rzip_state *st, i64 last, i64 p)
{
	do {
		i64 len = p - last;
		if (len > 0xFFFF)
			len = 0xFFFF;

		st->stats.literals++;
		st->stats.literal_bytes += len;

		put_header(control, st->ss, 0, len);

		if (unlikely(len && write_sbstream(control, st->ss, 1, last, len)))
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
static tag clean_one_from_hash(rzip_control *control, struct rzip_state *st)
{
	tag better_than_min;

again:
	better_than_min = increase_mask(st->minimum_tag_mask);
	if (!st->tag_clean_ptr)
		print_maxverbose("Starting sweep for mask %u\n", (unsigned int)st->minimum_tag_mask);

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

static inline tag next_tag(rzip_control *control, struct rzip_state *st, i64 p, tag t)
{
	t ^= st->hash_index[*get_sb(control, p - 1)];
	t ^= st->hash_index[*get_sb(control, p + MINIMUM_MATCH - 1)];
	return t;
}

static inline tag full_tag(rzip_control *control, struct rzip_state *st, i64 p)
{
	tag ret = 0;
	int i;

	for (i = 0; i < MINIMUM_MATCH; i++)
		ret ^= st->hash_index[*get_sb(control, p + i)];
	return ret;
}

static inline i64 match_len(rzip_control *control, struct rzip_state *st, i64 p0, i64 op, i64 end,
			    i64 *rev)
{
	i64 p = p0;
	i64 len = 0;

	if (op >= p0)
		return 0;

	while ((*get_sb(control, p) == *get_sb(control, op)) && (p < end)) {
		p++;
		op++;
	}
	len = p - p0;

	p = p0;
	op -= len;

	end = 0;
	if (end < st->last_match)
		end = st->last_match;

	while (p > end && op > 0 && *get_sb(control, op - 1) == *get_sb(control, p - 1)) {
		op--;
		p--;
	}

	(*rev) = p0 - p;
	len += p0 - p;

	if (len < MINIMUM_MATCH)
		return 0;

	return len;
}

static i64 find_best_match(rzip_control *control, struct rzip_state *st, tag t, i64 p, i64 end,
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
			mlen = match_len(control, st, p, st->hash_table[h].offset, end,
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

static void show_distrib(rzip_control *control, struct rzip_state *st)
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
		print_err("WARNING: hash_count says total %lld\n", st->hash_count);

	print_output("%lld total hashes -- %lld in primary bucket (%-2.3f%%)\n", total, primary,
	       primary*100.0/total);
}

static void hash_search(rzip_control *control, struct rzip_state *st, double pct_base, double pct_multiple)
{
	i64 cksum_limit = 0, p, end;
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

	if (unlikely(!st->hash_table))
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

	t = full_tag(control, st, p);

	while (p < end) {
		int lastpct = 0, last_chunkpct = 0;
		i64 reverse, mlen, offset = 0;

		p++;
		sb.offset_search = p;
		t = next_tag(control, st, p, t);

		/* Don't look for a match if there are no tags with
		   this number of bits in the hash table. */
		if ((t & st->minimum_tag_mask) != st->minimum_tag_mask)
			continue;

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

		if (unlikely(p % 128 == 0)) {
			int pct, chunk_pct;

			pct = pct_base + (pct_multiple * (100.0 * p) /
			      st->chunk_size);
			chunk_pct = p / (end / 100);
			if (pct != lastpct || chunk_pct != last_chunkpct) {
				if (!STDIN || st->stdin_eof)
					print_progress("Total: %2d%%  ", pct);
				print_progress("Chunk: %2d%%\r", chunk_pct);
				lastpct = pct;
				last_chunkpct = chunk_pct;
			}
		}

		if (p > (i64)cksum_limit) {
			i64 i, n = MIN(st->chunk_size - p, control->page_size);
			uchar *ckbuf = malloc(n);

			if (unlikely(!ckbuf))
				fatal("Failed to malloc ckbuf in hash_search\n");
			for (i = 0; i < n; i++)
				memcpy(ckbuf + i, get_sb(control, cksum_limit + i), 1);
			st->cksum = CrcUpdate(st->cksum, ckbuf, n);
			md5_process_bytes(ckbuf, n, &control->ctx);
			cksum_limit += n;
			free(ckbuf);
		}
	}

	if (MAX_VERBOSE)
		show_distrib(control, st);

	if (st->last_match < st->chunk_size)
		put_literal(control, st, st->last_match, st->chunk_size);

	if (st->chunk_size > cksum_limit) {
		i64 i, n = st->chunk_size - cksum_limit;
		uchar *ckbuf = malloc(n);

		if (unlikely(!ckbuf))
			fatal("Failed to malloc ckbuf in hash_search\n");
		for (i = 0; i < n; i++)
			memcpy(ckbuf + i, get_sb(control, cksum_limit + i), 1);
		st->cksum = CrcUpdate(st->cksum, ckbuf, n);
		md5_process_bytes(ckbuf, n, &control->ctx);
		cksum_limit += n;
		free(ckbuf);
	}

	put_literal(control, st, 0, 0);
	put_u32(control, st->ss, st->cksum);
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
static void mmap_stdin(rzip_control *control, uchar *buf, struct rzip_state *st)
{
	i64 len = st->chunk_size;
	uchar *offset_buf = buf;
	ssize_t ret;
	i64 total;

	total = 0;
	while (len > 0) {
		if (len > one_g)
			ret = one_g;
		else
			ret = len;
		ret = read(0, offset_buf, (size_t)ret);
		if (unlikely(ret < 0))
			fatal("Failed to read in mmap_stdin\n");
		total += ret;
		if (ret == 0) {
			/* Should be EOF */
			print_maxverbose("Shrinking chunk to %lld\n", total);
			buf = (uchar *)mremap(buf, st->chunk_size, total, 0);
			if (unlikely(buf == MAP_FAILED))
				fatal("Failed to remap to smaller buf in mmap_stdin\n");
			st->mmap_size = st->chunk_size = total;
			st->stdin_eof = 1;
			break;
		}
		offset_buf += ret;
		len -= ret;
	}
	control->st_size += total;
}

static void init_sliding_mmap(rzip_control *control, struct rzip_state *st, int fd_in, i64 offset)
{
	/* Initialise the high buffer */
	if (!STDIN) {
		sb.high_length = 65536;
		/* Round up to the next biggest page size */
		if (sb.high_length % control->page_size)
			sb.high_length += control->page_size - (sb.high_length % control->page_size);
		sb.buf_high = (uchar *)mmap(NULL, sb.high_length, PROT_READ, MAP_SHARED, fd_in, offset);
		if (unlikely(sb.buf_high == MAP_FAILED))
			fatal("Unable to mmap buf_high in init_sliding_mmap\n");
		sb.size_high = sb.high_length;
		sb.offset_high = 0;
	}
	sb.offset_low = 0;
	sb.offset_search = 0;
	sb.size_low = st->mmap_size;
	sb.orig_size = st->chunk_size;
	sb.fd = fd_in;
}

/* compress a chunk of an open file. Assumes that the file is able to
   be mmap'd and is seekable */
static void rzip_chunk(rzip_control *control, struct rzip_state *st, int fd_in, int fd_out, i64 offset,
		       double pct_base, double pct_multiple)
{
	init_sliding_mmap(control, st, fd_in, offset);

	st->ss = open_stream_out(control, fd_out, NUM_STREAMS, st->chunk_size, st->chunk_bytes);
	if (unlikely(!st->ss))
		fatal("Failed to open streams in rzip_chunk\n");

	print_verbose("Beginning rzip pre-processing phase\n");
	hash_search(control, st, pct_base, pct_multiple);

	/* unmap buffer before closing and reallocating streams */
	if (unlikely(munmap(sb.buf_low, sb.size_low)))
		fatal("Failed to munmap in rzip_chunk\n");
	if (!STDIN) {
		if (unlikely(munmap(sb.buf_high, sb.size_high)))
			fatal("Failed to munmap in rzip_chunk\n");
	}

	if (unlikely(close_stream_out(control, st->ss)))
		fatal("Failed to flush/close streams in rzip_chunk\n");
}

/* Needs to be less than 31 bits and page aligned on 32 bits */
const i64 two_gig = (1ull << 31) - 4096;

/* compress a whole file chunks at a time */
void rzip_fd(rzip_control *control, int fd_in, int fd_out)
{
	/* add timers for ETA estimates
	 * Base it off the file size and number of iterations required
	 * depending on compression window size
	 * Track elapsed time and estimated time to go
	 * If file size < compression window, can't do
	 */
	struct timeval current, start, last;
	char md5_resblock[MD5_DIGEST_SIZE];
	i64 len = 0, last_chunk = 0;
	int pass = 0, passes, j;
	struct rzip_state *st;
	struct statvfs fbuf;
	struct stat s, s2;
	double chunkmbs;
	i64 free_space;

	md5_init_ctx (&control->ctx);

	st = calloc(sizeof(*st), 1);
	if (unlikely(!st))
		fatal("Failed to allocate control state in rzip_fd\n");

	if (LZO_COMPRESS) {
		if (unlikely(lzo_init() != LZO_E_OK))
			fatal("lzo_init() failed\n");
	}

	if (unlikely(fstat(fd_in, &s)))
		fatal("Failed to stat fd_in in rzip_fd\n");

	if (!STDIN) {
		len = control->st_size = s.st_size;
		print_verbose("File size: %lld\n", len);
	} else
		control->st_size = 0;

	/* Check if there's enough free space on the device chosen to fit the
	 * compressed file, based on the compressed file being as large as the
	 * uncompressed file. */
	if (unlikely(fstatvfs(fd_out, &fbuf)))
		fatal("Failed to fstatvfs in compress_file\n");
	free_space = fbuf.f_bsize * fbuf.f_bavail;
	if (free_space < control->st_size) {
		if (FORCE_REPLACE)
			print_err("Warning, possibly inadequate free space detected, but attempting to compress due to -f option being used.\n");
		else
			failure("Possibly inadequate free space to compress file, use -f to override.\n");
	}

	/* Optimal use of ram involves using no more than 2/3 of it, so we
	 * allocate 1/3 of it to the main buffer and use a sliding mmap
	 * buffer to work on 2/3 ram size, leaving enough ram for the
	 * compression backends */
	control->max_mmap = control->maxram;

	/* On 32 bits we can have a big window with sliding mmap, but can
	 * not enable much per mmap/malloc */
	if (BITS32)
		control->max_mmap = MIN(control->max_mmap, two_gig);
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

	while (len > 0 || (STDIN && !st->stdin_eof)) {
		double pct_base, pct_multiple;
		i64 offset = s.st_size - len;
		int bits = 8;

		st->chunk_size = control->max_chunk;
		st->mmap_size = control->max_mmap;
		if (!STDIN) {
			st->chunk_size = MIN(st->chunk_size, len);
			st->mmap_size = MIN(st->mmap_size, len);
		}

retry:
		if (STDIN) {
			/* NOTE the buf is saved here for STDIN mode */
			sb.buf_low = mmap(NULL, st->mmap_size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
			/* Better to shrink the window to the largest size that works than fail */
			if (sb.buf_low == MAP_FAILED) {
				if (unlikely(errno != ENOMEM))
					fatal("Failed to mmap %s\n", control->infile);
				st->mmap_size = st->mmap_size / 10 * 9;
				round_to_page(&st->mmap_size);
				if (unlikely(!st->mmap_size))
					fatal("Unable to mmap any ram\n");
				goto retry;
			}
			st->chunk_size = st->mmap_size;
			mmap_stdin(control, sb.buf_low, st);
		} else {
			/* NOTE The buf is saved here for !STDIN mode */
			sb.buf_low = (uchar *)mmap(sb.buf_low, st->mmap_size, PROT_READ, MAP_SHARED, fd_in, offset);
			if (sb.buf_low == MAP_FAILED) {
				if (unlikely(errno != ENOMEM))
					fatal("Failed to mmap %s\n", control->infile);
				st->mmap_size = st->mmap_size / 10 * 9;
				round_to_page(&st->mmap_size);
				if (unlikely(!st->mmap_size))
					fatal("Unable to mmap any ram\n");
				goto retry;
			}
			if (st->mmap_size < st->chunk_size)
				print_maxverbose("Enabling sliding mmap mode and using mmap of %lld bytes with window of %lld bytes\n", st->mmap_size, st->chunk_size);
		}
		print_maxverbose("Succeeded in testing %lld sized mmap for rzip pre-processing\n", st->mmap_size);

		if (st->chunk_size > control->ramsize)
			print_verbose("Compression window is larger than ram, will proceed with unlimited mode possibly much slower\n");

		if (!passes && !STDIN) {
			passes = s.st_size / st->chunk_size + !!(s.st_size % st->chunk_size);
			if (passes == 1)
				print_verbose("Will take 1 pass\n");
			else
				print_verbose("Will take %d passes\n", passes);
		}

		sb.orig_offset = offset;
		print_maxverbose("Chunk size: %lld\n", st->chunk_size);

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
		if (last.tv_sec > 0) {
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

		rzip_chunk(control, st, fd_in, fd_out, offset, pct_base, pct_multiple);

		/* st->chunk_size may be shrunk in rzip_chunk */
		last_chunk = st->chunk_size;
		len -= st->chunk_size;
	}

	close_streamout_threads(control);

	md5_finish_ctx (&control->ctx, md5_resblock);
	if (HASH_CHECK || MAX_VERBOSE) {
		print_output("MD5: ");
		for (j = 0; j < MD5_DIGEST_SIZE; j++)
			print_output("%02x", md5_resblock[j] & 0xFF);
		print_output("\n");
	}
	if (unlikely(write(control->fd_out, md5_resblock, MD5_DIGEST_SIZE) != MD5_DIGEST_SIZE))
		fatal("Failed to write md5 in rzip_fd\n");

	gettimeofday(&current, NULL);
	if (STDIN)
		s.st_size = control->st_size;
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
		print_progress("%s - ", control->infile);
	print_progress("Compression Ratio: %.3f. Average Compression Speed: %6.3fMB/s.\n",
		       1.0 * s.st_size / s2.st_size, chunkmbs);

	free(st);
}
