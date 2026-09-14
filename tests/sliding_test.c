/* Exercise the private matcher with both operands outside its main map. */
#include "../rzip.c"

static void require(int ok)
{
	if (!ok) {
		fputs("Sliding match test failed\n", stderr);
		exit(1);
	}
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
	sb->high_length = sb->size_high = page;
	sb->buf_high = mmap(NULL, page, PROT_READ, MAP_SHARED, fd, page);
	require(sb->buf_low != MAP_FAILED && sb->buf_high != MAP_FAILED);
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
	require(munmap(sb->buf_high, sb->size_high) == 0);
	if (sb->buf_high_prev) {
		require(sb->buf_high_prev != sb->buf_high);
		require(munmap(sb->buf_high_prev, sb->size_high_prev) == 0);
	}
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
	require(close(fd) == 0);
	free(data);
	puts("Sliding match mapping lifetime tests passed");
	return 0;
}
