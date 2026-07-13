/**
 * AES-GCM implementation for lrzip (NIST SP 800-38D style).
 * Copyright (C) 2026 Con Kolivas
 *
 * Built on PolarSSL AES ECB (aes.h). Public-domain algorithm; this
 * packaging is GPL-2+ to match lrzip.
 */

#include "gcm.h"
#include "aes.h"

#include <string.h>

static void xor_block(unsigned char *d, const unsigned char *a,
		      const unsigned char *b)
{
	int i;

	for (i = 0; i < 16; i++)
		d[i] = a[i] ^ b[i];
}

/* GF(2^128) multiply x * y into r (big-endian bit string as in GCM). */
static void gcm_mult(const unsigned char x[16], const unsigned char y[16],
		     unsigned char r[16])
{
	unsigned char z[16], v[16];
	int i, j;

	memset(z, 0, 16);
	memcpy(v, y, 16);
	for (i = 0; i < 16; i++) {
		for (j = 0; j < 8; j++) {
			if (x[i] & (1 << (7 - j)))
				xor_block(z, z, v);
			{
				int lsb = v[15] & 1;
				int k;

				for (k = 15; k > 0; k--)
					v[k] = (unsigned char)((v[k] >> 1) | ((v[k - 1] & 1) << 7));
				v[0] >>= 1;
				if (lsb)
					v[0] ^= 0xe1;
			}
		}
	}
	memcpy(r, z, 16);
}

static void ghash(const unsigned char H[16],
		  const unsigned char *aad, size_t aad_len,
		  const unsigned char *ct, size_t ct_len,
		  unsigned char y[16])
{
	unsigned char x[16], tmp[16];
	size_t i;

	memset(y, 0, 16);

	/* AAD */
	for (i = 0; i + 16 <= aad_len; i += 16) {
		xor_block(tmp, y, aad + i);
		gcm_mult(tmp, H, y);
	}
	if (i < aad_len) {
		memset(x, 0, 16);
		memcpy(x, aad + i, aad_len - i);
		xor_block(tmp, y, x);
		gcm_mult(tmp, H, y);
	}

	/* Ciphertext */
	for (i = 0; i + 16 <= ct_len; i += 16) {
		xor_block(tmp, y, ct + i);
		gcm_mult(tmp, H, y);
	}
	if (i < ct_len) {
		memset(x, 0, 16);
		memcpy(x, ct + i, ct_len - i);
		xor_block(tmp, y, x);
		gcm_mult(tmp, H, y);
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
	xor_block(tmp, y, x);
	gcm_mult(tmp, H, y);
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
