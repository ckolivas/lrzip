/* Exercise the private matcher with both operands outside its main map. */
#include "config.h"
#include "lrzip_private.h"
#include <unistd.h>
#include <errno.h>

static unsigned history_reads;
static int short_history_reads;

static ssize_t history_pread(int fd, void *buf, size_t count, off_t offset)
{
	if (short_history_reads) {
		if (++history_reads % 7 == 0) {
			errno = EINTR;
			return -1;
		}
		if (count > 17)
			count = 17;
	}
	return pread(fd, buf, count, offset);
}

static int fail_history_cache;
static unsigned history_alloc_failures;

static void *history_calloc(size_t count, size_t size)
{
	if (fail_history_cache && count == RZIP_HISTORY_CACHE_SIZE &&
	    size == sizeof(struct history_page)) {
		history_alloc_failures++;
		return NULL;
	}
	return calloc(count, size);
}

#define calloc history_calloc
#define pread history_pread
#include "../rzip.c"
#undef pread
#undef calloc

static void require(int ok)
{
	if (!ok) {
		fputs("Sliding match test failed\n", stderr);
		exit(1);
	}
}

static void release_history(struct sliding_buffer *sb)
{
	unsigned i;

	free(sb->buf_high);
	for (i = 0; sb->history_cache && i <= sb->history_mask; i++) {
		struct history_page *page = &sb->history_cache[i];

		if (page->buf) {
			require(page->buf != sb->buf_high);
			free(page->buf);
		}
	}
	if (sb->history_cache != &sb->history_fallback)
		free(sb->history_cache);
}

static void check_match(int fd, uchar *data, size_t size, size_t page,
			size_t start, size_t history, size_t forward, size_t reverse)
{
	rzip_control control = {0};
	struct rzip_state st = {0};
	struct sliding_buffer *sb = &control.sb;
	i64 rev = 0, got, expected = forward + reverse;

	memcpy(data + start - reverse, data + history - reverse, forward + reverse);
	data[start + forward] = data[history + forward] ^ 0xff;
	require(pwrite(fd, data, size, page) == (ssize_t)size);
	control.page_size = page;
	sb->fd = fd;
	sb->orig_offset = page;
	sb->orig_size = size;
	sb->offset_low = page * 128;
	sb->size_low = page * 16;
	sb->buf_low = mmap(NULL, sb->size_low, PROT_READ, MAP_SHARED, fd,
			   sb->orig_offset + sb->offset_low);
	sb->high_length = page;
	load_high_sb(&control, sb, 0);
	require(sb->buf_low != MAP_FAILED);
	st.last_match = start - reverse;
	got = sliding_match_len(&control, &st, start, history,
				size - MINIMUM_MATCH, &rev, 0);
	if (expected < MINIMUM_MATCH)
		expected = 0;
	require(got == expected);
	if (got)
		require(rev == (i64)reverse);
	/* Reuse and swap cached maps, then reject a match that cannot win. */
	got = sliding_match_len(&control, &st, start, history,
				size - MINIMUM_MATCH, &rev, expected);
	require(got == 0);
	require(munmap(sb->buf_low, sb->size_low) == 0);
	release_history(sb);
}

static void check_collision(int fd, uchar *data, size_t page)
{
	rzip_control control = {0};
	struct sliding_buffer *sb = &control.sb;
	i64 offsets[3] = {0};
	uchar values[] = {0x35, 0xc7, 0x69};
	unsigned i;

	for (i = 1; i < 3; i++) {
		offsets[i] = offsets[i - 1] + page;
		while (history_page_slot(offsets[i]) != history_page_slot(0)) {
			offsets[i] += page;
			require(offsets[i] < (i64)page * RZIP_HISTORY_CACHE_SIZE * 64);
		}
	}
	require(ftruncate(fd, offsets[2] + page) == 0);
	for (i = 0; i < 3; i++) {
		memset(data, values[i], page);
		require(pwrite(fd, data, page, offsets[i]) == (ssize_t)page);
	}
	/* The final cache entry ends with a partial page. */
	require(ftruncate(fd, offsets[2] + 67) == 0);
	control.page_size = page;
	sb->fd = fd;
	sb->orig_size = offsets[2] + 67;
	sb->high_length = page;
	load_high_sb(&control, sb, 0);
	for (i = 0; i < 64; i++) {
		unsigned ai = i % 3, bi = (i + 1) % 3;
		uchar *a = sliding_get_sb(&control, offsets[ai]);
		uchar *b = sliding_get_sb(&control, offsets[bi]);

		require(a != b);
		require(a[0] == values[ai] && a[ai == 2 ? 66 : page - 1] == values[ai]);
		require(b[0] == values[bi] && b[bi == 2 ? 66 : page - 1] == values[bi]);
	}
	release_history(sb);
}

int main(void)
{
	size_t page = sysconf(_SC_PAGESIZE), size = page * 256;
	size_t alignments[] = {0, 1, 7, 31, page - 1};
	uchar *data = malloc(size);
	uint32_t random = 543219;
	char name[] = "/tmp/lrzip-sliding-XXXXXX";
	size_t i, alignment;
	int fd = mkstemp(name);

	require(fd >= 0 && data);
	require(unlink(name) == 0);
	for (i = 0; i < size; i++) {
		random ^= random << 13;
		random ^= random >> 17;
		random ^= random << 5;
		data[i] = random;
	}
	for (i = 0; i < sizeof(alignments) / sizeof(alignments[0]); i++) {
		alignment = alignments[i];
		/* Forward comparison leaves the low map; history is already outside. */
		check_match(fd, data, size, page, page * 143 + alignment,
			    page * 8 + alignment, page * 2 + 17, 17);
		/* Reverse comparison leaves the low map with both operands live. */
		check_match(fd, data, size, page, page * 128 + alignment,
			    page * 8 + alignment, 33, page * 2 + alignment);
	}
	check_match(fd, data, size, page, page * 143, page * 8, 0, 0);
	check_match(fd, data, size, page, page * 143, page * 8, 31, 0);
	short_history_reads = 1;
	check_collision(fd, data, page);
	require(history_reads > 64);
	fail_history_cache = 1;
	check_match(fd, data, size, page, page * 143 + 1,
		    page * 8 + 1, page * 2 + 17, 17);
	check_collision(fd, data, page);
	require(history_alloc_failures == 2);
	require(close(fd) == 0);
	free(data);
	puts("Sliding match mapping lifetime tests passed");
	return 0;
}
