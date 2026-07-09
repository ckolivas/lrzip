/*
 * lrzip glue for libzpaq 7.15 — memory-buffer Reader/Writer, C API, error handler.
 * Preserves write-bounds checks (malicious/corrupt archives) and progress display.
 *
 * Copyright (C) 2011-2026 Con Kolivas
 * libzpaq 7.15 is public domain (Matt Mahoney).
 */

#include "libzpaq.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifndef uchar
#define uchar unsigned char
#endif

typedef int64_t i64;

#define likely(x)	__builtin_expect(!!(x), 1)
#define unlikely(x)	__builtin_expect(!!(x), 0)

/* Application-provided error sink required by libzpaq 7.x */
namespace libzpaq {
void error(const char* msg)
{
	fprintf(stderr, "libzpaq error: %s\n", msg);
	exit(1);
}
}

/*
 * Clamp to classic Compressor::startBlock levels 1..3 (min/mid/max.cfg).
 *
 * libzpaq 7.x method strings "1".."5" are a *different* family (fast LZ77 /
 * mixed methods for the zpaq archiver). Using those made -z much faster and
 * weaker than pre-7.15 lrzip (and often weaker than LZMA). Docs map classic
 * levels ~ to methods "3"/"4"/"5", but startBlock(level) is the real old path.
 *
 * lrzip still passes compression_level/4+1 (1..3) from stream.c.
 */
static int zpaq_classic_level(int level)
{
	if (level < 1)
		return 1;
	if (level > 3)
		return 3;
	return level;
}

struct bufRead: public libzpaq::Reader {
	uchar *s_buf;
	i64 *s_len;
	i64 total_len;
	int *last_pct;
	bool progress;
	long thread;
	FILE *msgout;

	bufRead(uchar *buf_, i64 *n_, i64 total_len_, int *last_pct_,
		bool progress_, long thread_, FILE *msgout_)
		: s_buf(buf_), s_len(n_), total_len(total_len_),
		  last_pct(last_pct_), progress(progress_),
		  thread(thread_), msgout(msgout_) {}

	int get() {
		/* CVE-2017-8842: never divide by zero when total_len == 0 */
		if (progress && total_len > 0 && !(*s_len % 128)) {
			int pct = (int)((total_len - *s_len) * 100 / total_len);

			if (pct / 10 != *last_pct / 10) {
				int i;

				fprintf(msgout, "\r\t\t\tZPAQ\t");
				for (i = 0; i < thread; i++)
					fprintf(msgout, "\t");
				fprintf(msgout, "%ld:%i%%  \r",
					thread + 1, pct);
				fflush(msgout);
				*last_pct = pct;
			}
		}

		if (likely(*s_len > 0)) {
			(*s_len)--;
			return (int)(uchar)*s_buf++;
		}
		return -1;
	}

	int read(char *buf, int n) {
		if (unlikely(n < 0))
			return 0;
		if (unlikely((i64)n > *s_len))
			n = (int)*s_len;

		if (likely(n > 0)) {
			*s_len -= n;
			memcpy(buf, s_buf, (size_t)n);
			s_buf += n;
		}
		return n;
	}
};

struct bufWrite: public libzpaq::Writer {
	uchar *c_buf;
	i64 *c_len;
	i64 max_len;

	bufWrite(uchar *buf_, i64 *n_, i64 max_ = -1LL)
		: c_buf(buf_), c_len(n_), max_len(max_) {}

	void put(int c) {
		if (*c_len < 0)
			return;
		if (max_len >= 0 && *c_len >= max_len) {
			*c_len = -1;
			return;
		}
		c_buf[(*c_len)++] = static_cast<uchar>(c);
	}

	void write(const char *buf, int n) {
		if (n <= 0 || *c_len < 0)
			return;
		if (max_len < 0) {
			memcpy(c_buf + *c_len, buf, (size_t)n);
			*c_len += n;
			return;
		}
		{
			i64 avail = max_len - *c_len;

			if (avail <= 0) {
				*c_len = -1;
				return;
			}
			if ((i64)n > avail) {
				*c_len = -1;
				return;
			}
		}
		memcpy(c_buf + *c_len, buf, (size_t)n);
		*c_len += n;
	}
};

extern "C" void zpaq_compress(uchar *c_buf, i64 *c_len, uchar *s_buf, i64 s_len,
			      int level, FILE *msgout, bool progress, long thread)
{
	i64 total_len = s_len;
	int last_pct = 100;
	int classic = zpaq_classic_level(level);

	bufRead bufR(s_buf, &s_len, total_len, &last_pct, progress, thread, msgout);
	bufWrite bufW(c_buf, c_len);

	/* Classic min/mid/max models (same family as pre-7.15 lrzip -z).
	 * Do not use libzpaq::compress(..., "1".."5") — those are the
	 * archiver's fast methods, not the old context-mixing levels. */
	libzpaq::Compressor c;

	c.setInput(&bufR);
	c.setOutput(&bufW);
	c.startBlock(classic);
	c.startSegment();
	c.postProcess();
	c.compress();
	c.endSegment(); /* no SHA-1; lrzip has its own integrity */
	c.endBlock();
}

extern "C" int zpaq_decompress(uchar *s_buf, i64 *d_len, uchar *c_buf, i64 c_len,
			       FILE *msgout, bool progress, long thread,
			       i64 expected_len)
{
	i64 total_len = c_len;
	int last_pct = 100;

	bufRead bufR(c_buf, &c_len, total_len, &last_pct, progress, thread, msgout);
	bufWrite bufW(s_buf, d_len, expected_len);

	libzpaq::decompress(&bufR, &bufW);
	if (*d_len < 0)
		return -1; /* write past expected_len / corrupt input */
	return 0;
}
