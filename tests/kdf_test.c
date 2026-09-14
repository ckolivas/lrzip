/* PBKDF2/HKDF results cross-checked with Python hashlib and hmac. */
#include "lrzip_private.h"
#include "util.h"
#include <string.h>

static const struct {
	size_t pass_len;
	unsigned iters;
	const char *keys;
} vectors[] = {
	{ 1, 1,
	  "cef2c7495d2348df01cacbb4d8bbeddce74c9c049f2b0c511ac743d924f6cb2a"
	  "dec55e7dcdbc4952069e57d1d9a53df02055c6847800dc9b4d8fef502f3ba757" },
	{ 8, 2,
	  "a776a79ba96e2dc7156b0980cfb44fd099c1cca1c60c9713d545870772f43fb5"
	  "b0ffb524d4d8ff3063853b7bb2cd5fa5811768137918e02f1cadfe7813d8ba80" },
	{ 127, 7,
	  "bf1cb32c47c6f020c6afb0defcfa15e620a54e6ce3e7954bd2a10f0070b8f8e9"
	  "f84406ccc32ce8cde3c61334da6a42a8b8682c195e4ab671a663695f27ceaebd" },
	{ 128, 7,
	  "e23b91c166fc28d2ad558905221221d366f9e04c4425c42da4bdc1d546080df7"
	  "8e21676d3e5a1d17a01216dd1af13ccfda54dae7f3d423b155625f638832e9d3" },
	{ 129, 7,
	  "b6d88d1a22d0a38af4d8d9a6304da68e703c2ec98aa363d960845b6799a8cf67"
	  "f40e3aa05cef4e9c86061aaea3af5aa7f16ab52e7bc2f4db3cc493a37c2ade46" },
	{ 255, 1000,
	  "9f19f6b441fb1d8273c71cef641bb066d2c601b02a6d866513942b187351a1e7"
	  "93d005471944525f95b9406eb3930deccab636f767d31d603f885804e455b847" },
	{ 256, 2,
	  "3d286da66f489a7df7ce4515aee298d2f3dc0317a360e7df958301c3b2b7d979"
	  "db001bbc48858ad26eb103237a1a1072552eb26e960b3aa0ffb7a79ec77cf0d3" },
	{ 257, 2,
	  "ac92f6454b863b59ed25097e800055033109528b173b74636cd898abfb6e7901"
	  "a14445805e267ebf6d0c8f29dbb4c4e795cb960722aec03a12bbef33b184558e" },
};

static unsigned char hex_digit(char c)
{
	return c <= '9' ? c - '0' : c - 'a' + 10;
}

int main(void)
{
	rzip_control control;
	uchar password[SALT_LEN + 257], expected[64];
	size_t i, v;

	memset(&control, 0, sizeof(control));
	memset(password, 0, sizeof(password));
	control.msgout = stderr;
	control.salt_pass = password;
	for (i = 0; i < LRZ_AEAD_SALT_LEN; i++)
		control.aead_salt[i] = (uchar)i;
	for (i = 0; i < 257; i++)
		password[SALT_LEN + i] = (uchar)(i * 7 + 1);
	for (v = 0; v < sizeof(vectors) / sizeof(vectors[0]); v++) {
		control.salt_pass_len = SALT_LEN + vectors[v].pass_len;
		control.aead_iters = vectors[v].iters;
		for (i = 0; i < sizeof(expected); i++)
			expected[i] = (hex_digit(vectors[v].keys[2 * i]) << 4) |
				      hex_digit(vectors[v].keys[2 * i + 1]);
		if (!lrz_aead_kdf_setup(&control) ||
		    memcmp(control.aead_key_hdr, expected, LRZ_AEAD_KEY_LEN) ||
		    memcmp(control.aead_key_data, expected + LRZ_AEAD_KEY_LEN,
			   LRZ_AEAD_KEY_LEN)) {
			fprintf(stderr, "KDF vector %zu failed\n", v);
			return 1;
		}
	}
	control.aead_iters = 0;
	if (lrz_aead_kdf_setup(&control))
		return 1;
	control.aead_iters = 1;
	control.salt_pass_len = SALT_LEN;
	if (lrz_aead_kdf_setup(&control))
		return 1;
	puts("PBKDF2/HKDF tests passed");
	return 0;
}
