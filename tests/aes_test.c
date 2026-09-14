/* AES ECB/CBC vectors checked against an independent implementation.
 * Exercise every key size, both key schedules, unaligned buffers and
 * overlapping input/output used by legacy archive encryption. */
#include "aes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct {
	int bits;
	const char *ecb, *cbc;
} vectors[] = {
	{ 128,
	  "26017e8ff83c5b3df993c68daff05f037928180b7b618c3a4609831802cbaa13"
	  "340d62401cee04469d5fc211260fe229",
	  "83999037441a3b9bf4808dd326145a9e3c2c61d7e82d0fd45dd02db2e8f76137"
	  "f329390a25fef0691b522595926650d8" },
	{ 192,
	  "a56d361eeb824c0192dfad17985027c280e60ff0b29e7f494323f2ce3c0911af"
	  "ccfa4c283cfb8edcbdbc35038ece39cb",
	  "1281aa0427ac0dd158c7b04164dec16d964e9235f6883dc35120f31a2c5a77d3"
	  "b144588c5e5c51ae5af697b3a3d0af96" },
	{ 256,
	  "babdf707cfb130f349e5e128f1c379ea58e861e6fa698becf368eaa4051a719a"
	  "4940056e3787d4bdd9d9c74fbad60bc1",
	  "216dc6b83e67974be3217a843a1038ef07ce2bd5d8bc49250c91ffb68811a544"
	  "72a23a3b2f9f7e69ff51588a84877ea4" },
};

static void require(int ok)
{
	if (!ok) {
		fputs("AES test failed\n", stderr);
		exit(1);
	}
}

static unsigned char hex_digit(char c)
{
	return c <= '9' ? c - '0' : c - 'a' + 10;
}

int main(void)
{
	aes_context ctx;
	unsigned char key[32], pt[49], ct[49], buf[49], iv[16];
	size_t v, i;
	int cbc, mode, inplace;

	for (i = 0; i < sizeof(key); i++)
		key[i] = (unsigned char)i;
	for (i = 0; i < 48; i++)
		pt[i + 1] = (unsigned char)(i * 37 + 11);
	for (v = 0; v < sizeof(vectors) / sizeof(vectors[0]); v++) {
		for (cbc = 0; cbc <= 1; cbc++) {
			const char *expected = cbc ? vectors[v].cbc : vectors[v].ecb;

			for (i = 0; i < 48; i++)
				ct[i + 1] = (hex_digit(expected[2 * i]) << 4) |
					    hex_digit(expected[2 * i + 1]);
			for (mode = AES_DECRYPT; mode <= AES_ENCRYPT; mode++) {
				const unsigned char *src = mode == AES_ENCRYPT ? pt + 1 : ct + 1;
				const unsigned char *want = mode == AES_ENCRYPT ? ct + 1 : pt + 1;

				if (mode == AES_ENCRYPT)
					require(aes_setkey_enc(&ctx, key, vectors[v].bits) == 0);
				else
					require(aes_setkey_dec(&ctx, key, vectors[v].bits) == 0);
				for (inplace = 0; inplace <= 1; inplace++) {
					const unsigned char *input = inplace ? buf + 1 : src;

					memcpy(buf + 1, src, 48);
					for (i = 0; i < sizeof(iv); i++)
						iv[i] = (unsigned char)i;
					if (cbc) {
						require(aes_crypt_cbc(&ctx, mode, 48, iv, input, buf + 1) == 0);
						require(!memcmp(iv, ct + 33, 16));
					} else {
						for (i = 0; i < 48; i += 16)
							require(aes_crypt_ecb(&ctx, mode, input + i, buf + 1 + i) == 0);
					}
					require(!memcmp(buf + 1, want, 48));
				}
			}
		}
	}
	require(aes_setkey_enc(&ctx, key, 129) == POLARSSL_ERR_AES_INVALID_KEY_LENGTH);
	require(aes_setkey_dec(&ctx, key, 129) == POLARSSL_ERR_AES_INVALID_KEY_LENGTH);
	require(aes_crypt_cbc(&ctx, AES_ENCRYPT, 15, iv, pt + 1, buf + 1) ==
		POLARSSL_ERR_AES_INVALID_INPUT_LENGTH);
	puts("AES tests passed");
	return 0;
}
