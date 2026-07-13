/*
   Copyright (C) 2006-2016,2021-2022,2026 Con Kolivas
   Copyright (C) 2011 Serge Belyshev
   Copyright (C) 2008, 2011 Peter Hyman
   Copyright (C) 1998 Andrew Tridgell

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

/*
  Utilities used in rzip

  tridge, June 1996
  */

/*
 * Realloc removed
 * Functions added
 *    read_config()
 * Peter Hyman, December 2008
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdarg.h>
#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif
#include <termios.h>

#ifdef _SC_PAGE_SIZE
# define PAGE_SIZE (sysconf(_SC_PAGE_SIZE))
#else
# define PAGE_SIZE (4096)
#endif

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include "lrzip_private.h"
#include "util.h"
#include "sha4.h"
#include "aes.h"
#ifdef HAVE_CTYPE_H
# include <ctype.h>
#endif
#include <inttypes.h>

/* Macros for testing parameters */
#define isparameter( parmstring, value )	(!strcasecmp( parmstring, value ))
#define iscaseparameter( parmvalue, value )	(!strcmp( parmvalue, value ))

void register_infile(rzip_control *control, const char *name, char delete)
{
	control->util_infile = name;
	control->delete_infile = delete;
}

void register_outfile(rzip_control *control, const char *name, char delete)
{
	control->util_outfile = name;
	control->delete_outfile = delete;
}

void register_outputfile(rzip_control *control, FILE *f)
{
	control->outputfile = f;
}

void unlink_files(rzip_control *control)
{
	/* Delete temporary files generated for testing or faking stdio */
	if (control->util_outfile && control->delete_outfile)
		unlink(control->util_outfile);

	if (control->util_infile && control->delete_infile)
		unlink(control->util_infile);
}

void fatal_exit(rzip_control *control)
{
	struct termios termios_p;

	/* Make sure we haven't died after disabling stdin echo */
	tcgetattr(fileno(stdin), &termios_p);
	termios_p.c_lflag |= ECHO;
	tcsetattr(fileno(stdin), 0, &termios_p);

	unlink_files(control);
	if (!STDOUT && !TEST_ONLY && control->outfile) {
		if (!KEEP_BROKEN) {
			print_verbose("Deleting broken file %s\n", control->outfile);
			unlink(control->outfile);
		} else
			print_verbose("Keeping broken file %s as requested\n", control->outfile);
	}
	fprintf(control->outputfile, "Fatal error - exiting\n");
	fflush(control->outputfile);
	exit(LRZIP_EXIT_FAILURE);
}

void setup_overhead(rzip_control *control)
{
	/* Work out the compression overhead per compression thread for the
	 * compression back-ends that need a lot of ram */
	if (LZMA_COMPRESS && ULTRA) {
		int level = control->compression_level;

		if (!level)
			level = 1;
		else if (level > 9)
			level = 9;
		/* Dictionary sizes per direct level, larger than the SDK
		 * defaults: single block ultra hands the encoder whole
		 * streams that are typically hundreds of MB, so a larger
		 * window pays off in ratio. open_stream_out reduces threads
		 * if the overhead does not fit in usable ram. */
		i64 dictsize = (level <= 4 ? (1 << (level * 2 + 16)) :
				(level <= 6 ? (1 << (level + 20)) :
				(level == 7 ? (1 << 26) :
				(level == 8 ? (1 << 27) : ((i64)1 << 28)))));

		/* The encoder needs ~11.5 times the dictionary per thread; cap
		 * the dictionary so it fits the third of ram we will allow
		 * ourselves with room for the block buffers. */
		i64 dictcap = 1 << 20;
		while (dictcap * 13 <= control->ramsize / 3 && dictcap < ((i64)1 << 28))
			dictcap <<= 1;
		if (dictsize > dictcap)
			dictsize = dictcap;

		control->lzma_dictsize = dictsize;
		print_maxverbose("Using lzma dictionary size %"PRId64" for single block ultra\n",
				 dictsize);
		control->overhead = (dictsize * 23 / 2) + (6 * 1024 * 1024) + 16384;
	} else if (LZMA_COMPRESS) {
		int level = control->compression_level * 7 / 9;

		if (!level)
			level = 1;
		i64 dictsize = (level <= 5 ? (1 << (level * 2 + 14)) :
				(level == 6 ? (1 << 25) : (1 << 26)));

		control->overhead = (dictsize * 23 / 2) + (6 * 1024 * 1024) + 16384;
		/* LZMA spec shows memory requirements as 6MB, not 4MB and state size
		 * where default is 16KB */
	} else if (ZPAQ_COMPRESS)
		control->overhead = 112 * 1024 * 1024;
}

void setup_ram(rzip_control *control)
{
	/* Use less ram when using STDOUT to store the temporary output file. */
	if (STDOUT && ((STDIN && DECOMPRESS) || !(DECOMPRESS || TEST_ONLY)))
		control->maxram = control->ramsize / 6;
	else
		control->maxram = control->ramsize / 3;
	control->usable_ram = control->maxram;
	round_to_page(&control->maxram);
}

void round_to_page(i64 *size)
{
	*size -= *size % PAGE_SIZE;
	if (unlikely(!*size))
		*size = PAGE_SIZE;
}

size_t round_up_page(rzip_control *control, size_t len)
{
	int rem = len % control->page_size;

	if (rem)
		len += control->page_size - rem;
	return len;
}

bool get_rand(rzip_control *control, uchar *buf, int len)
{
	int fd;

	/* Fail closed: weak PRNG fallback is unsafe for salts/IVs. */
	fd = open("/dev/urandom", O_RDONLY);
	if (unlikely(fd == -1))
		fatal_return(("Failed to open /dev/urandom in get_rand\n"), false);
	if (unlikely(read(fd, buf, len) != len)) {
		close(fd);
		fatal_return(("Failed to read fd in get_rand\n"), false);
	}
	if (unlikely(close(fd)))
		fatal_return(("Failed to close fd in get_rand\n"), false);
	return true;
}

bool read_config(rzip_control *control)
{
	/* check for lrzip.conf in ., $HOME/.lrzip and /etc/lrzip */
	char *HOME, homeconf[255];
	char *parametervalue;
	char *parameter;
	char line[255];
	FILE *fp;

	fp = fopen("lrzip.conf", "r");
	if (fp)
		fprintf(control->msgout, "Using configuration file ./lrzip.conf\n");
	if (fp == NULL) {
		HOME=getenv("HOME");
		if (HOME) {
			snprintf(homeconf, sizeof(homeconf), "%s/.lrzip/lrzip.conf", HOME);
			fp = fopen(homeconf, "r");
			if (fp)
				fprintf(control->msgout, "Using configuration file %s\n", homeconf);
		}
	}
	if (fp == NULL) {
		fp = fopen("/etc/lrzip/lrzip.conf", "r");
		if (fp)
			fprintf(control->msgout, "Using configuration file /etc/lrzip/lrzip.conf\n");
	}
	if (fp == NULL)
		return false;

	/* if we get here, we have a file. read until no more. */

	while ((fgets(line, 255, fp)) != NULL) {
		if (strlen(line))
			line[strlen(line) - 1] = '\0';
		parameter = strtok(line, " =");
		if (parameter == NULL)
			continue;
		/* skip if whitespace or # */
		if (isspace(*parameter))
			continue;
		if (*parameter == '#')
			continue;

		parametervalue = strtok(NULL, " =");
		if (parametervalue == NULL)
			continue;

		/* have valid parameter line, now assign to control */

		if (isparameter(parameter, "window"))
			control->window = atoi(parametervalue);
		else if (isparameter(parameter, "unlimited")) {
			if (isparameter(parametervalue, "yes"))
				control->flags |= FLAG_UNLIMITED;
		} else if (isparameter(parameter, "compressionlevel")) {
			control->compression_level = atoi(parametervalue);
			if ( control->compression_level < 1 || control->compression_level > 9 )
				failure_return(("CONF.FILE error. Compression Level must between 1 and 9"), false);
		} else if (isparameter(parameter, "compressionmethod")) {
			/* valid are rzip, gzip, bzip2, lzo, lzma (default), and zpaq */
			if (control->flags & FLAG_NOT_LZMA)
				failure_return(("CONF.FILE error. Can only specify one compression method"), false);
			if (isparameter(parametervalue, "bzip2"))
				control->flags |= FLAG_BZIP2_COMPRESS;
			else if (isparameter(parametervalue, "gzip"))
				control->flags |= FLAG_ZLIB_COMPRESS;
			else if (isparameter(parametervalue, "lzo"))
				control->flags |= FLAG_LZO_COMPRESS;
			else if (isparameter(parametervalue, "rzip"))
				control->flags |= FLAG_NO_COMPRESS;
			else if (isparameter(parametervalue, "zpaq"))
				control->flags |= FLAG_ZPAQ_COMPRESS;
			else if (!isparameter(parametervalue, "lzma")) /* oops, not lzma! */
				failure_return(("CONF.FILE error. Invalid compression method %s specified\n",parametervalue), false);
		} else if (isparameter(parameter, "lzotest")) {
			/* default is yes */
			if (isparameter(parametervalue, "no"))
				control->flags &= ~FLAG_THRESHOLD;
		} else if (isparameter(parameter, "hashcheck")) {
			if (isparameter(parametervalue, "yes")) {
				control->flags |= FLAG_CHECK;
				control->flags |= FLAG_HASH;
			}
		} else if (isparameter(parameter, "showhash")) {
			if (isparameter(parametervalue, "yes"))
				control->flags |= FLAG_HASH;
		} else if (isparameter(parameter, "outputdirectory")) {
			control->outdir = malloc(strlen(parametervalue) + 2);
			if (!control->outdir)
				fatal_return(("Fatal Memory Error in read_config"), false);
			strcpy(control->outdir, parametervalue);
			if (strcmp(parametervalue + strlen(parametervalue) - 1, "/"))
				strcat(control->outdir, "/");
		} else if (isparameter(parameter,"verbosity")) {
			if (control->flags & FLAG_VERBOSE)
				failure_return(("CONF.FILE error. Verbosity already defined."), false);
			if (isparameter(parametervalue, "yes"))
				control->flags |= FLAG_VERBOSITY;
			else if (isparameter(parametervalue,"max"))
				control->flags |= FLAG_VERBOSITY_MAX;
			else /* oops, unrecognized value */
				print_err("lrzip.conf: Unrecognized verbosity value %s. Ignored.\n", parametervalue);
		} else if (isparameter(parameter, "showprogress")) {
			/* Yes by default */
			if (isparameter(parametervalue, "NO"))
				control->flags &= ~FLAG_SHOW_PROGRESS;
		} else if (isparameter(parameter,"nice")) {
			control->nice_val = atoi(parametervalue);
			if (control->nice_val < -20 || control->nice_val > 19)
				failure_return(("CONF.FILE error. Nice must be between -20 and 19"), false);
		} else if (isparameter(parameter, "keepbroken")) {
			if (isparameter(parametervalue, "yes" ))
				control->flags |= FLAG_KEEP_BROKEN;
		} else if (iscaseparameter(parameter, "DELETEFILES")) {
			/* delete files must be case sensitive */
			if (iscaseparameter(parametervalue, "YES"))
				control->flags &= ~FLAG_KEEP_FILES;
		} else if (iscaseparameter(parameter, "REPLACEFILE")) {
			/* replace lrzip file must be case sensitive */
			if (iscaseparameter(parametervalue, "YES"))
				control->flags |= FLAG_FORCE_REPLACE;
		} else if (isparameter(parameter, "tmpdir")) {
			control->tmpdir = realloc(NULL, strlen(parametervalue) + 2);
			if (!control->tmpdir)
				fatal_return(("Fatal Memory Error in read_config"), false);
			strcpy(control->tmpdir, parametervalue);
			if (strcmp(parametervalue + strlen(parametervalue) - 1, "/"))
				strcat(control->tmpdir, "/");
		} else if (isparameter(parameter, "encrypt")) {
			if (isparameter(parametervalue, "YES")) {
				control->flags |= FLAG_ENCRYPT;
				if (!ENCRYPT_LEGACY)
					control->flags |= FLAG_ENCRYPT_AEAD;
			}
		} else if (isparameter(parameter, "legacy_encrypt")) {
			if (isparameter(parametervalue, "YES")) {
				control->flags |= FLAG_ENCRYPT_LEGACY;
				control->flags &= ~FLAG_ENCRYPT_AEAD;
			}
		} else
			/* oops, we have an invalid parameter, display */
			print_err("lrzip.conf: Unrecognized parameter value, %s = %s. Continuing.\n",\
				       parameter, parametervalue);
	}

	if (unlikely(fclose(fp)))
		fatal_return(("Failed to fclose fp in read_config\n"), false);

/*	fprintf(stderr, "\nWindow = %d \
		\nCompression Level = %d \
		\nThreshold = %1.2f \
		\nOutput Directory = %s \
		\nFlags = %d\n", control->window,control->compression_level, control->threshold, control->outdir, control->flags);
*/
	return true;
}

static void xor128 (void *pa, const void *pb)
{
	i64 *a = pa;
	const i64 *b = pb;

	a [0] ^= b [0];
	a [1] ^= b [1];
}

static void lrz_keygen(const rzip_control *control, const uchar *salt, uchar *key, uchar *iv)
{
	uchar buf [HASH_LEN + SALT_LEN + PASS_LEN];
	mlock(buf, HASH_LEN + SALT_LEN + PASS_LEN);

	memcpy(buf, control->hash, HASH_LEN);
	memcpy(buf + HASH_LEN, salt, SALT_LEN);
	memcpy(buf + HASH_LEN + SALT_LEN, control->salt_pass, control->salt_pass_len);
	sha4(buf, HASH_LEN + SALT_LEN + control->salt_pass_len, key, 0);

	memcpy(buf, key, HASH_LEN);
	memcpy(buf + HASH_LEN, salt, SALT_LEN);
	memcpy(buf + HASH_LEN + SALT_LEN, control->salt_pass, control->salt_pass_len);
	sha4(buf, HASH_LEN + SALT_LEN + control->salt_pass_len, iv, 0);

	memset(buf, 0, sizeof(buf));
	munlock(buf, sizeof(buf));
}

bool lrz_crypt(const rzip_control *control, uchar *buf, i64 len, const uchar *salt, int encrypt)
{
	/* Encryption requires CBC_LEN blocks so we can use ciphertext
	* stealing to not have to pad the block */
	uchar key[HASH_LEN], iv[HASH_LEN];
	uchar tmp0[CBC_LEN], tmp1[CBC_LEN];
	aes_context aes_ctx = {};
	i64 N, M;
	bool ret = false;

	/* Generate unique key and IV for each block of data based on salt */
	mlock(&aes_ctx, sizeof(aes_ctx));
	mlock(key, HASH_LEN);
	mlock(iv, HASH_LEN);

	lrz_keygen(control, salt, key, iv);

	M = len % CBC_LEN;
	N = len - M;

	if (encrypt == LRZ_ENCRYPT) {
		print_maxverbose("Encrypting data        \n");
		if (unlikely(aes_setkey_enc(&aes_ctx, key, 128)))
			failure_goto(("Failed to aes_setkey_enc in lrz_crypt\n"), error);
		aes_crypt_cbc(&aes_ctx, AES_ENCRYPT, N, iv, buf, buf);

		if (M) {
			memset(tmp0, 0, CBC_LEN);
			memcpy(tmp0, buf + N, M);
			aes_crypt_cbc(&aes_ctx, AES_ENCRYPT, CBC_LEN,
				iv, tmp0, tmp1);
			memcpy(buf + N, buf + N - CBC_LEN, M);
			memcpy(buf + N - CBC_LEN, tmp1, CBC_LEN);
		}
	} else {
		if (unlikely(aes_setkey_dec(&aes_ctx, key, 128)))
			failure_goto(("Failed to aes_setkey_dec in lrz_crypt\n"), error);
		print_maxverbose("Decrypting data        \n");
		if (M) {
			aes_crypt_cbc(&aes_ctx, AES_DECRYPT, N - CBC_LEN,
				      iv, buf, buf);
			aes_crypt_ecb(&aes_ctx, AES_DECRYPT,
				      buf + N - CBC_LEN, tmp0);
			memset(tmp1, 0, CBC_LEN);
			memcpy(tmp1, buf + N, M);
			xor128(tmp0, tmp1);
			memcpy(buf + N, tmp0, M);
			memcpy(tmp1 + M, tmp0 + M, CBC_LEN - M);
			aes_crypt_ecb(&aes_ctx, AES_DECRYPT, tmp1,
				      buf + N - CBC_LEN);
			xor128(buf + N - CBC_LEN, iv);
		} else
			aes_crypt_cbc(&aes_ctx, AES_DECRYPT, len,
				      iv, buf, buf);
	}

	ret = true;
error:
	memset(&aes_ctx, 0, sizeof(aes_ctx));
	memset(iv, 0, HASH_LEN);
	memset(key, 0, HASH_LEN);
	munlock(&aes_ctx, sizeof(aes_ctx));
	munlock(iv, HASH_LEN);
	munlock(key, HASH_LEN);
	return ret;
}

void lrz_stretch(rzip_control *control)
{
	sha4_context ctx = {};
	i64 j, n, counter;

	mlock(&ctx, sizeof(ctx));
	sha4_starts(&ctx, 0);

	n = control->encloops * HASH_LEN / (control->salt_pass_len + sizeof(i64));
	print_maxverbose("Hashing passphrase %"PRId64" (%"PRId64") times \n", control->encloops, n);
	for (j = 0; j < n; j ++) {
		counter = htole64(j);
		sha4_update(&ctx, (uchar *)&counter, sizeof(counter));
		sha4_update(&ctx, control->salt_pass, control->salt_pass_len);
	}
	sha4_finish(&ctx, control->hash);
	memset(&ctx, 0, sizeof(ctx));
	munlock(&ctx, sizeof(ctx));
}

#include "gcm.h"

void lrz_secure_wipe(void *p, size_t n)
{
	if (p && n)
		memset(p, 0, n);
}

/* HMAC-SHA512 one-shot for PBKDF2/HKDF (key any length). */
static void hmac_sha512(const uchar *key, size_t key_len,
			const uchar *msg, size_t msg_len,
			uchar out[64])
{
	sha4_context ctx;
	uchar k_ipad[128], k_opad[128], tk[64], full[64];
	size_t i;

	memset(k_ipad, 0, sizeof(k_ipad));
	memset(k_opad, 0, sizeof(k_opad));
	if (key_len > 128) {
		sha4(key, (int)key_len, tk, 0);
		key = tk;
		key_len = 64;
	}
	memcpy(k_ipad, key, key_len);
	memcpy(k_opad, key, key_len);
	for (i = 0; i < 128; i++) {
		k_ipad[i] ^= 0x36;
		k_opad[i] ^= 0x5c;
	}
	sha4_starts(&ctx, 0);
	sha4_update(&ctx, k_ipad, 128);
	if (msg_len) {
		size_t off = 0;
		while (off < msg_len) {
			int chunk = (int)MIN(msg_len - off, (size_t)(1 << 30));
			sha4_update(&ctx, msg + off, chunk);
			off += (size_t)chunk;
		}
	}
	sha4_finish(&ctx, full);
	sha4_starts(&ctx, 0);
	sha4_update(&ctx, k_opad, 128);
	sha4_update(&ctx, full, 64);
	sha4_finish(&ctx, out);
	lrz_secure_wipe(k_ipad, sizeof(k_ipad));
	lrz_secure_wipe(k_opad, sizeof(k_opad));
	lrz_secure_wipe(tk, sizeof(tk));
	lrz_secure_wipe(full, sizeof(full));
	lrz_secure_wipe(&ctx, sizeof(ctx));
}

/* PBKDF2-HMAC-SHA512 (RFC 8018) → dk_len bytes in out. */
static bool pbkdf2_sha512(const uchar *pass, size_t pass_len,
			  const uchar *salt, size_t salt_len,
			  unsigned int iters, uchar *out, size_t dk_len)
{
	uchar U[64], T[64], block[256];
	unsigned int block_index = 1;
	size_t offset = 0;

	if (!pass || !salt || !out || iters < 1 || dk_len < 1 || salt_len > 200)
		return false;

	while (offset < dk_len) {
		size_t i, j, take;
		size_t blen;

		/* U1 = HMAC(pass, salt || INT(block_index)) */
		memcpy(block, salt, salt_len);
		block[salt_len] = (uchar)(block_index >> 24);
		block[salt_len + 1] = (uchar)(block_index >> 16);
		block[salt_len + 2] = (uchar)(block_index >> 8);
		block[salt_len + 3] = (uchar)block_index;
		blen = salt_len + 4;
		hmac_sha512(pass, pass_len, block, blen, U);
		memcpy(T, U, 64);
		for (i = 1; i < iters; i++) {
			hmac_sha512(pass, pass_len, U, 64, U);
			for (j = 0; j < 64; j++)
				T[j] ^= U[j];
		}
		take = MIN(dk_len - offset, (size_t)64);
		memcpy(out + offset, T, take);
		offset += take;
		block_index++;
	}
	lrz_secure_wipe(U, sizeof(U));
	lrz_secure_wipe(T, sizeof(T));
	lrz_secure_wipe(block, sizeof(block));
	return true;
}

/* HKDF-Expand-SHA512 (RFC 5869) with empty salt extract skipped:
 * OKM = Expand(PRK=master, info, L). */
static bool hkdf_expand_sha512(const uchar *prk, size_t prk_len,
			       const char *info, size_t info_len,
			       uchar *okm, size_t okm_len)
{
	uchar T[64], block[128];
	uchar counter = 1;
	size_t offset = 0, tlen = 0;

	if (!prk || !okm || okm_len < 1 || okm_len > 255 * 64)
		return false;
	while (offset < okm_len) {
		size_t blen = 0, take;
		if (tlen) {
			memcpy(block, T, tlen);
			blen = tlen;
		}
		if (info && info_len) {
			memcpy(block + blen, info, info_len);
			blen += info_len;
		}
		block[blen++] = counter++;
		hmac_sha512(prk, prk_len, block, blen, T);
		tlen = 64;
		take = MIN(okm_len - offset, (size_t)64);
		memcpy(okm + offset, T, take);
		offset += take;
	}
	lrz_secure_wipe(T, sizeof(T));
	lrz_secure_wipe(block, sizeof(block));
	return true;
}

bool lrz_aead_kdf_setup(rzip_control *control)
{
	uchar master[HASH_LEN];
	const uchar *pass;
	size_t pass_len;

	if (!control || !control->salt_pass || control->salt_pass_len < SALT_LEN)
		return false;
	if (control->aead_iters < 1 || control->aead_iters > LRZ_PBKDF2_ITERS_MAX)
		return false;

	/* salt_pass layout for AEAD: [salt 16][password...] after we set it;
	 * for setup we use aead_salt + password after SALT_LEN legacy region.
	 * get_hash currently does salt_pass = salt8 || password.
	 * For AEAD we store password starting at salt_pass+SALT_LEN still,
	 * and full aead_salt separately. */
	pass = control->salt_pass + SALT_LEN;
	pass_len = (size_t)control->salt_pass_len - SALT_LEN;
	if (pass_len < 1)
		return false;

	print_maxverbose("PBKDF2-HMAC-SHA512 iterations %u\n", control->aead_iters);
	if (!pbkdf2_sha512(pass, pass_len, control->aead_salt, LRZ_AEAD_SALT_LEN,
			   control->aead_iters, master, HASH_LEN))
		return false;
	if (!hkdf_expand_sha512(master, HASH_LEN, "lrzip-v3-hdr", 12,
				control->aead_key_hdr, LRZ_AEAD_KEY_LEN))
		return false;
	if (!hkdf_expand_sha512(master, HASH_LEN, "lrzip-v3-data", 13,
				control->aead_key_data, LRZ_AEAD_KEY_LEN))
		return false;
	if (!get_rand(control, control->aead_nonce_prefix, 4)) {
		lrz_secure_wipe(master, sizeof(master));
		return false;
	}
	control->aead_hdr_seq = 0;
	control->aead_data_seq = 0;
	lrz_secure_wipe(master, sizeof(master));
	return true;
}

static void aead_next_nonce(rzip_control *control, int key_id,
			    uchar nonce[LRZ_AEAD_NONCE_LEN])
{
	uint64_t seq;
	int i;

	memcpy(nonce, control->aead_nonce_prefix, 4);
	if (key_id == LRZ_AEAD_KEY_HDR)
		seq = ++control->aead_hdr_seq;
	else
		seq = ++control->aead_data_seq;
	/* seq as little-endian in nonce[4..11] */
	for (i = 0; i < 8; i++)
		nonce[4 + i] = (uchar)(seq >> (8 * i));
}

static const uchar *aead_key(const rzip_control *control, int key_id)
{
	return key_id == LRZ_AEAD_KEY_HDR ? control->aead_key_hdr
					  : control->aead_key_data;
}

bool lrz_aead_seal(rzip_control *control, int key_id,
		   const uchar *aad, size_t aad_len,
		   const uchar *pt, size_t pt_len,
		   uchar *out, size_t *out_len)
{
	uchar nonce[LRZ_AEAD_NONCE_LEN], tag[LRZ_AEAD_TAG_LEN];
	size_t need = LRZ_AEAD_NONCE_LEN + pt_len + LRZ_AEAD_TAG_LEN;

	if (!control || !out || !out_len || *out_len < need || (pt_len && !pt))
		return false;
	if (key_id != LRZ_AEAD_KEY_HDR && key_id != LRZ_AEAD_KEY_DATA)
		return false;

	aead_next_nonce(control, key_id, nonce);
	if (gcm_aes_encrypt(aead_key(control, key_id), 256, nonce,
			    aad, aad_len, pt, pt_len,
			    out + LRZ_AEAD_NONCE_LEN, tag) != 0)
		return false;
	memcpy(out, nonce, LRZ_AEAD_NONCE_LEN);
	memcpy(out + LRZ_AEAD_NONCE_LEN + pt_len, tag, LRZ_AEAD_TAG_LEN);
	*out_len = need;
	lrz_secure_wipe(nonce, sizeof(nonce));
	lrz_secure_wipe(tag, sizeof(tag));
	return true;
}

bool lrz_aead_open(rzip_control *control, int key_id,
		   const uchar *aad, size_t aad_len,
		   const uchar *in, size_t in_len,
		   uchar *pt_out, size_t *pt_len)
{
	size_t clen;

	if (!control || !in || !pt_out || !pt_len)
		return false;
	if (in_len < LRZ_AEAD_NONCE_LEN + LRZ_AEAD_TAG_LEN)
		return false;
	if (key_id != LRZ_AEAD_KEY_HDR && key_id != LRZ_AEAD_KEY_DATA)
		return false;
	clen = in_len - LRZ_AEAD_NONCE_LEN - LRZ_AEAD_TAG_LEN;
	if (*pt_len < clen)
		return false;
	if (gcm_aes_decrypt(aead_key(control, key_id), 256, in,
			    aad, aad_len,
			    in + LRZ_AEAD_NONCE_LEN, clen,
			    in + LRZ_AEAD_NONCE_LEN + clen,
			    pt_out) != 0)
		return false;
	*pt_len = clen;
	return true;
}
