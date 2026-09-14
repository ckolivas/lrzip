/**
 * AES-GCM implementation for lrzip (NIST SP 800-38D style).
 * Copyright (C) 2026 Con Kolivas
 *
 * Built on PolarSSL AES ECB (aes.h). Public-domain algorithm; this
 * packaging is GPL-2+ to match lrzip.
 */

#include "gcm.h"
#include "aes.h"

#include <stdint.h>
#include <string.h>
#if (defined(__PCLMUL__) || defined(__AES__)) && defined(__SSE2__)
#include <wmmintrin.h>
#endif

static void xor_block(unsigned char *d, const unsigned char *a,
		      const unsigned char *b)
{
	int i;

	for (i = 0; i < 16; i++)
		d[i] = a[i] ^ b[i];
}

#if defined(__PCLMUL__) && defined(__SSE2__)
/* GCM numbers bits from the high bit of each byte; CLMUL numbers them
 * from the low bit. Reverse within bytes without alignment assumptions. */
static __m128i gcm_reverse_bits(__m128i v)
{
	__m128i mask = _mm_set1_epi8(0x55);

	v = _mm_or_si128(_mm_and_si128(_mm_srli_epi16(v, 1), mask),
			 _mm_slli_epi16(_mm_and_si128(v, mask), 1));
	mask = _mm_set1_epi8(0x33);
	v = _mm_or_si128(_mm_and_si128(_mm_srli_epi16(v, 2), mask),
			 _mm_slli_epi16(_mm_and_si128(v, mask), 2));
	mask = _mm_set1_epi8(0x0f);
	return _mm_or_si128(_mm_and_si128(_mm_srli_epi16(v, 4), mask),
			    _mm_slli_epi16(_mm_and_si128(v, mask), 4));
}

/* Multiply polynomials already in the hardware bit order. */
static __m128i gcm_mult(__m128i a, __m128i b)
{
	__m128i low = _mm_clmulepi64_si128(a, b, 0x00);
	__m128i high = _mm_clmulepi64_si128(a, b, 0x11);
	__m128i cross = _mm_xor_si128(_mm_clmulepi64_si128(a, b, 0x01),
				     _mm_clmulepi64_si128(a, b, 0x10));
	uint64_t h0, h1, t0, t1, overflow;

	low = _mm_xor_si128(low, _mm_slli_si128(cross, 8));
	high = _mm_xor_si128(high, _mm_srli_si128(cross, 8));
	h0 = (uint64_t)_mm_cvtsi128_si64(high);
	h1 = (uint64_t)_mm_cvtsi128_si64(_mm_srli_si128(high, 8));

	/* Reduce modulo x^128 + x^7 + x^2 + x + 1. Multiplying the
	 * high half by 0x87 overflows by at most seven bits; its second
	 * reduction fits entirely in the low word. */
	t0 = h0 ^ (h0 << 1) ^ (h0 << 2) ^ (h0 << 7);
	t1 = h1 ^ (h1 << 1) ^ (h1 << 2) ^ (h1 << 7) ^
	     (h0 >> 63) ^ (h0 >> 62) ^ (h0 >> 57);
	overflow = (h1 >> 63) ^ (h1 >> 62) ^ (h1 >> 57);
	t0 ^= overflow ^ (overflow << 1) ^ (overflow << 2) ^ (overflow << 7);
	low = _mm_xor_si128(low, _mm_set_epi64x(t1, t0));
	return low;
}
#else
/* GF(2^128) multiply x * y into r (big-endian bit string as in GCM). */
static void gcm_mult(const unsigned char x[16], const unsigned char y[16],
		     unsigned char r[16])
{
	uint64_t zh = 0, zl = 0, vh = 0, vl = 0;
	int i, j;

	/* Keep the big-endian polynomial in two words, avoiding a 16-byte
	 * shift for every bit. Explicit loads also support unaligned input. */
	for (i = 0; i < 8; i++) {
		vh = (vh << 8) | y[i];
		vl = (vl << 8) | y[i + 8];
	}
	for (i = 0; i < 16; i++) {
		for (j = 0; j < 8; j++) {
			uint64_t mask = (uint64_t)0 - ((x[i] >> (7 - j)) & 1);
			uint64_t reduce = (uint64_t)0 - (vl & 1);

			zh ^= vh & mask;
			zl ^= vl & mask;
			vl = (vl >> 1) | (vh << 63);
			vh = (vh >> 1) ^ (UINT64_C(0xe100000000000000) & reduce);
		}
	}
	for (i = 0; i < 8; i++) {
		r[i] = (unsigned char)(zh >> (56 - 8 * i));
		r[i + 8] = (unsigned char)(zl >> (56 - 8 * i));
	}
}
#endif

struct ghash_state {
#if defined(__PCLMUL__) && defined(__SSE2__)
	__m128i h, y;
#else
	const unsigned char *h;
	unsigned char y[16];
#endif
};

static inline void ghash_block(struct ghash_state *state, const unsigned char block[16])
{
#if defined(__PCLMUL__) && defined(__SSE2__)
	__m128i x = gcm_reverse_bits(_mm_loadu_si128((const __m128i *)block));

	state->y = gcm_mult(_mm_xor_si128(state->y, x), state->h);
#else
	unsigned char tmp[16];

	xor_block(tmp, state->y, block);
	gcm_mult(tmp, state->h, state->y);
#endif
}

static void ghash(const unsigned char H[16],
		  const unsigned char *aad, size_t aad_len,
		  const unsigned char *ct, size_t ct_len,
		  unsigned char y[16])
{
	struct ghash_state state;
	unsigned char x[16];
	size_t i;

#if defined(__PCLMUL__) && defined(__SSE2__)
	/* Keep the key and accumulator in polynomial bit order throughout
	 * the pass, converting each input block once and the result once. */
	state.h = gcm_reverse_bits(_mm_loadu_si128((const __m128i *)H));
	state.y = _mm_setzero_si128();
#else
	state.h = H;
	memset(state.y, 0, 16);
#endif

	/* AAD */
	for (i = 0; i + 16 <= aad_len; i += 16) {
		ghash_block(&state, aad + i);
	}
	if (i < aad_len) {
		memset(x, 0, 16);
		memcpy(x, aad + i, aad_len - i);
		ghash_block(&state, x);
	}

	/* Ciphertext */
	for (i = 0; i + 16 <= ct_len; i += 16) {
		ghash_block(&state, ct + i);
	}
	if (i < ct_len) {
		memset(x, 0, 16);
		memcpy(x, ct + i, ct_len - i);
		ghash_block(&state, x);
	}

	/* Lengths block: bit lengths of AAD and CT as 64-bit BE each */
	{
		unsigned long long abits = (unsigned long long)aad_len * 8ULL;
		unsigned long long cbits = (unsigned long long)ct_len * 8ULL;

		memset(x, 0, 16);
		for (i = 0; i < 8; i++) {
			x[7 - i] = (unsigned char)(abits >> (8 * i));
			x[15 - i] = (unsigned char)(cbits >> (8 * i));
		}
	}
	ghash_block(&state, x);
#if defined(__PCLMUL__) && defined(__SSE2__)
	_mm_storeu_si128((__m128i *)y, gcm_reverse_bits(state.y));
#else
	memcpy(y, state.y, 16);
#endif
}


static void inc32(unsigned char counter[16])
{
	int i;

	for (i = 15; i >= 12; i--) {
		if (++counter[i] != 0)
			break;
	}
}

static void gcm_ctr(aes_context *ctx, unsigned char counter[16],
		    const unsigned char *in, size_t len, unsigned char *out)
{
	unsigned char stream[16];
	size_t i, n;

#if defined(__AES__) && defined(__SSE2__)
	/* Independent counters let the hardware overlap AES round latency. */
	while (len >= 64) {
		__m128i a, b, c, d, key;
		int round;

		inc32(counter);
		a = _mm_loadu_si128((const __m128i *)counter);
		inc32(counter);
		b = _mm_loadu_si128((const __m128i *)counter);
		inc32(counter);
		c = _mm_loadu_si128((const __m128i *)counter);
		inc32(counter);
		d = _mm_loadu_si128((const __m128i *)counter);
		key = _mm_loadu_si128((const __m128i *)ctx->rk);
		a = _mm_xor_si128(a, key);
		b = _mm_xor_si128(b, key);
		c = _mm_xor_si128(c, key);
		d = _mm_xor_si128(d, key);
		for (round = 1; round < ctx->nr; round++) {
			key = _mm_loadu_si128((const __m128i *)(ctx->rk + 4 * round));
			a = _mm_aesenc_si128(a, key);
			b = _mm_aesenc_si128(b, key);
			c = _mm_aesenc_si128(c, key);
			d = _mm_aesenc_si128(d, key);
		}
		key = _mm_loadu_si128((const __m128i *)(ctx->rk + 4 * round));
		a = _mm_aesenclast_si128(a, key);
		b = _mm_aesenclast_si128(b, key);
		c = _mm_aesenclast_si128(c, key);
		d = _mm_aesenclast_si128(d, key);
		a = _mm_xor_si128(a, _mm_loadu_si128((const __m128i *)in));
		b = _mm_xor_si128(b, _mm_loadu_si128((const __m128i *)(in + 16)));
		c = _mm_xor_si128(c, _mm_loadu_si128((const __m128i *)(in + 32)));
		d = _mm_xor_si128(d, _mm_loadu_si128((const __m128i *)(in + 48)));
		_mm_storeu_si128((__m128i *)out, a);
		_mm_storeu_si128((__m128i *)(out + 16), b);
		_mm_storeu_si128((__m128i *)(out + 32), c);
		_mm_storeu_si128((__m128i *)(out + 48), d);
		in += 64;
		out += 64;
		len -= 64;
	}
#endif
	while (len > 0) {
		inc32(counter);
		aes_crypt_ecb(ctx, AES_ENCRYPT, counter, stream);
		n = len < 16 ? len : 16;
		for (i = 0; i < n; i++)
			out[i] = in[i] ^ stream[i];
		in += n;
		out += n;
		len -= n;
	}
	memset(stream, 0, sizeof(stream));
}

static int gcm_prepare(aes_context *ctx, const unsigned char *key, int keybits,
		       const unsigned char nonce[GCM_NONCE_LEN],
		       unsigned char H[16], unsigned char J0[16],
		       unsigned char E0[16])
{
	unsigned char zero[16];

	if (keybits != 128 && keybits != 192 && keybits != 256)
		return -1;
	if (aes_setkey_enc(ctx, key, keybits) != 0)
		return -1;

	memset(zero, 0, 16);
	aes_crypt_ecb(ctx, AES_ENCRYPT, zero, H);

	/* J0 for 96-bit IV: nonce || 0x00000001 */
	memcpy(J0, nonce, 12);
	J0[12] = 0;
	J0[13] = 0;
	J0[14] = 0;
	J0[15] = 1;

	aes_crypt_ecb(ctx, AES_ENCRYPT, J0, E0);
	return 0;
}

int gcm_aes_encrypt(const unsigned char *key, int keybits,
		    const unsigned char nonce[GCM_NONCE_LEN],
		    const unsigned char *aad, size_t aad_len,
		    const unsigned char *pt, size_t pt_len,
		    unsigned char *ct,
		    unsigned char tag[GCM_TAG_LEN])
{
	aes_context ctx;
	unsigned char H[16], J0[16], E0[16], counter[16], S[16];
	int rc = -1;

	if (!key || !nonce || !tag || (pt_len && (!pt || !ct)) || (aad_len && !aad))
		return -1;

	memset(&ctx, 0, sizeof(ctx));
	if (gcm_prepare(&ctx, key, keybits, nonce, H, J0, E0) != 0)
		goto out;

	memcpy(counter, J0, 16);
	if (pt_len)
		gcm_ctr(&ctx, counter, pt, pt_len, ct);
	ghash(H, aad, aad_len, ct, pt_len, S);
	xor_block(tag, E0, S);
	rc = 0;
out:
	memset(H, 0, sizeof(H));
	memset(J0, 0, sizeof(J0));
	memset(E0, 0, sizeof(E0));
	memset(counter, 0, sizeof(counter));
	memset(S, 0, sizeof(S));
	memset(&ctx, 0, sizeof(ctx));
	return rc;
}

int gcm_aes_decrypt(const unsigned char *key, int keybits,
		    const unsigned char nonce[GCM_NONCE_LEN],
		    const unsigned char *aad, size_t aad_len,
		    const unsigned char *ct, size_t ct_len,
		    const unsigned char tag[GCM_TAG_LEN],
		    unsigned char *pt)
{
	aes_context ctx;
	unsigned char H[16], J0[16], E0[16], counter[16], S[16], expect[16];
	int i, diff, rc = -1;

	if (!key || !nonce || !tag || (ct_len && (!ct || !pt)) || (aad_len && !aad))
		return -1;

	memset(&ctx, 0, sizeof(ctx));
	if (gcm_prepare(&ctx, key, keybits, nonce, H, J0, E0) != 0)
		goto out;

	ghash(H, aad, aad_len, ct, ct_len, S);
	xor_block(expect, E0, S);
	diff = 0;
	for (i = 0; i < 16; i++)
		diff |= expect[i] ^ tag[i];
	if (diff != 0) {
		if (pt && ct_len)
			memset(pt, 0, ct_len);
		goto out;
	}

	memcpy(counter, J0, 16);
	if (ct_len)
		gcm_ctr(&ctx, counter, ct, ct_len, pt);
	rc = 0;
out:
	memset(H, 0, sizeof(H));
	memset(J0, 0, sizeof(J0));
	memset(E0, 0, sizeof(E0));
	memset(counter, 0, sizeof(counter));
	memset(S, 0, sizeof(S));
	memset(expect, 0, sizeof(expect));
	memset(&ctx, 0, sizeof(ctx));
	return rc;
}
