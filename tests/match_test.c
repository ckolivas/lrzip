/* Boundary and scalar-reference tests for the match primitives. */
#include "match.h"
#include <sys/mman.h>
#include <unistd.h>

static void check_span(const uchar *a, const uchar *b, i64 len)
{
	i64 f = 0, r = 0;

	while (f < len && a[f] == b[f])
		f++;
	while (r < len && a[len - r - 1] == b[len - r - 1])
		r++;
	if (match_forward(a, b, len) != f ||
	    match_reverse(a + len, b + len, len) != r) {
		fprintf(stderr, "Match comparison failed at length %lld\n",
			(long long)len);
		exit(1);
	}
}

static void check_comparisons(void)
{
	uchar a[160], b[160];
	i64 len, i;
	unsigned x, y;

	/* All alignments, short tails, and every mismatch position in a word. */
	for (x = 0; x < 16; x++) {
		for (y = 0; y < 16; y++) {
			for (len = 0; len <= 128; len++) {
				for (i = 0; i < len; i++)
					a[x + i] = b[y + i] = (uchar)(i * 37);
				check_span(a + x, b + y, len);
				for (i = 0; i < len; i++) {
					b[y + i] ^= 1U << (i % 8);
					check_span(a + x, b + y, len);
					b[y + i] ^= 1U << (i % 8);
				}
			}
		}
	}
}

static void check_guard_pages(void)
{
	long page = sysconf(_SC_PAGESIZE);
	uchar *a, *b;
	i64 len;

	if (page <= 0)
		exit(1);
	a = mmap(NULL, 3 * page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	b = mmap(NULL, 3 * page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (a == MAP_FAILED || b == MAP_FAILED ||
	    mprotect(a + page, page, PROT_READ | PROT_WRITE) ||
	    mprotect(b + page, page, PROT_READ | PROT_WRITE)) {
		perror("match test guard pages");
		exit(1);
	}
	memset(a + page, 0x5a, page);
	memset(b + page, 0x5a, page);
	for (len = 0; len <= page; len++) {
		/* The readable span touches either the leading or trailing guard. */
		check_span(a + page, b + page, len);
		check_span(a + 2 * page - len, b + 2 * page - len, len);
	}
	munmap(a, 3 * page);
	munmap(b, 3 * page);
}

static void check_expansion(i64 len, i64 offset)
{
	i64 period = MIN(len, offset), i;
	uchar *buf = malloc((size_t)len + 2);
	uchar *expected = malloc((size_t)len + 2);

	if (!buf || !expected)
		exit(1);
	memset(buf, 0xa5, (size_t)len + 2);
	memset(expected, 0xa5, (size_t)len + 2);
	for (i = 0; i < period; i++)
		buf[i + 1] = expected[i + 1] = (uchar)(i * 37 + i / 256);
	/* Literal LZ semantics: each output byte refers offset bytes back. */
	for (i = period; i < len; i++)
		expected[i + 1] = expected[i + 1 - offset];
	match_expand(buf + 1, period, len);
	if (memcmp(buf, expected, (size_t)len + 2)) {
		fprintf(stderr, "Match expansion failed: length %lld offset %lld\n",
			(long long)len, (long long)offset);
		exit(1);
	}
	free(buf);
	free(expected);
}

int main(void)
{
	const i64 offsets[] = { 1, 2, 3, 7, 31, 32, 63, 255, 256, 1023,
				32767, 32768, 65534, 65535, 65536 };
	i64 len, offset;
	size_t i;

	check_comparisons();
	check_guard_pages();
	for (len = 0; len <= 256; len++) {
		for (offset = 1; offset <= 300; offset++)
			check_expansion(len, offset);
	}
	for (i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
		check_expansion(65534, offsets[i]);
		check_expansion(65535, offsets[i]);
		/* Older archives can contain matches longer than 64 KiB. */
		check_expansion(1024 * 1024 + 3, offsets[i]);
	}
	puts("Match primitive tests passed");
	return 0;
}
