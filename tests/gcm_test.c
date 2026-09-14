/* AES-GCM vectors cross-checked with an independent implementation.
 * Keys and nonces are consecutive bytes; payload and AAD patterns below
 * cover all key sizes, empty inputs, partial blocks and full blocks. */
#include "gcm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct {
	int bits;
	size_t len, aad_len;
	const char *sealed;
} vectors[] = {
	{ 128, 0, 0,
	  "435b9ba12d75a4be8a977ea3cd011890" },
	{ 128, 16, 0,
	  "985cf2b4f9df1e5a788a1c28f14f613eec811b7440c90c5185e03e8bddc595c8" },
	{ 128, 17, 1,
	  "985cf2b4f9df1e5a788a1c28f14f613ee86bae35a75825273427a9da452a36ba"
	  "dd" },
	{ 192, 15, 17,
	  "edc977e1067d2000e36ba1203845cd0ad522b91077f8c1d1da5a4813de55e0" },
	{ 192, 32, 16,
	  "edc977e1067d2000e36ba1203845cd46551f96c1d7b56fe0fcf7a6cadaaa40da"
	  "d893b431b994a31e05c3c66195e8e5ca" },
	{ 192, 0, 33,
	  "9c936cccde8733365b8d33b49bd2709e" },
	{ 256, 31, 15,
	  "4c3283615a212b15be19ea297605695bd85622fe1f6f6622bbcf28770a55613b"
	  "52ab31abe8d190835a63631d5816fe" },
	{ 256, 65, 33,
	  "4c3283615a212b15be19ea297605695bd85622fe1f6f6622bbcf28770a556134"
	  "aac05be690a59b36a75c62afef0b99ee157925e7d5627a241cdf478baf3ff4c8"
	  "bb1d8ba770722ecaac4d86a07637b538f3" },
	{ 256, 1, 0,
	  "4ce7ddc5d7bed27b723ab1d4d5cb697b7b" },
};

static void require(int ok)
{
	if (!ok) {
		fputs("AES-GCM test failed\n", stderr);
		exit(1);
	}
}

static unsigned char hex_digit(char c)
{
	return c <= '9' ? c - '0' : c - 'a' + 10;
}

int main(void)
{
	unsigned char key[32], nonce[12], aad[33], pt[65], ct[65];
	unsigned char expected[81], tag[16], buf[65];
	size_t v, i, n, na;

	for (i = 0; i < sizeof(key); i++)
		key[i] = (unsigned char)i;
	for (i = 0; i < sizeof(nonce); i++)
		nonce[i] = (unsigned char)i;
	for (i = 0; i < sizeof(aad); i++)
		aad[i] = (unsigned char)(i * 13 + 7);
	for (i = 0; i < sizeof(pt); i++)
		pt[i] = (unsigned char)(i * 37 + 11);

	for (v = 0; v < sizeof(vectors) / sizeof(vectors[0]); v++) {
		n = vectors[v].len;
		na = vectors[v].aad_len;
		for (i = 0; i < n + 16; i++)
			expected[i] = (hex_digit(vectors[v].sealed[2 * i]) << 4) |
				      hex_digit(vectors[v].sealed[2 * i + 1]);
		require(gcm_aes_encrypt(key, vectors[v].bits, nonce, aad, na,
					pt, n, ct, tag) == 0);
		require(!memcmp(ct, expected, n) && !memcmp(tag, expected + n, 16));
		require(gcm_aes_decrypt(key, vectors[v].bits, nonce, aad, na,
					ct, n, tag, buf) == 0);
		require(!memcmp(buf, pt, n));

		memcpy(buf, pt, n);
		require(gcm_aes_encrypt(key, vectors[v].bits, nonce, aad, na,
					buf, n, buf, tag) == 0);
		require(!memcmp(buf, expected, n) && !memcmp(tag, expected + n, 16));
		require(gcm_aes_decrypt(key, vectors[v].bits, nonce, aad, na,
					buf, n, tag, buf) == 0);
		require(!memcmp(buf, pt, n));

		/* Failed authentication must reject and clear the plaintext. */
		for (i = 0; i < sizeof(tag); i++) {
			size_t j;

			tag[i] ^= 1;
			memset(buf, 0xa5, sizeof(buf));
			require(gcm_aes_decrypt(key, vectors[v].bits, nonce, aad, na,
						ct, n, tag, buf) == -1);
			for (j = 0; j < n; j++)
				require(buf[j] == 0);
			tag[i] ^= 1;
		}
		if (n) {
			ct[n - 1] ^= 1;
			require(gcm_aes_decrypt(key, vectors[v].bits, nonce, aad, na,
						ct, n, tag, buf) == -1);
			ct[n - 1] ^= 1;
		}
		if (na) {
			aad[na - 1] ^= 1;
			require(gcm_aes_decrypt(key, vectors[v].bits, nonce, aad, na,
						ct, n, tag, buf) == -1);
			aad[na - 1] ^= 1;
		}
	}
	puts("AES-GCM tests passed");
	return 0;
}
