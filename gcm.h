/**
 * AES-GCM (self-contained) for lrzip suite-3 encryption.
 * Uses PolarSSL AES (aes.h) for the block cipher.
 *
 * Copyright (C) 2026 Con Kolivas
 */
#ifndef LRZIP_GCM_H
#define LRZIP_GCM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GCM_BLOCK_SIZE	16
#define GCM_NONCE_LEN	12
#define GCM_TAG_LEN	16

/**
 * AES-GCM seal (encrypt + tag).
 * @param key       16/24/32-byte AES key
 * @param keybits   128, 192, or 256
 * @param nonce     12-byte nonce (must be unique under key)
 * @param aad       additional authenticated data (may be NULL if aad_len 0)
 * @param pt        plaintext (may be NULL if pt_len 0)
 * @param ct        ciphertext out (may alias pt); length == pt_len
 * @param tag       16-byte authentication tag out
 * @return 0 on success, non-zero on failure
 */
int gcm_aes_encrypt(const unsigned char *key, int keybits,
		    const unsigned char nonce[GCM_NONCE_LEN],
		    const unsigned char *aad, size_t aad_len,
		    const unsigned char *pt, size_t pt_len,
		    unsigned char *ct,
		    unsigned char tag[GCM_TAG_LEN]);

/**
 * AES-GCM open (verify tag + decrypt). Constant-time tag compare.
 * On tag failure, pt is wiped and non-zero is returned.
 */
int gcm_aes_decrypt(const unsigned char *key, int keybits,
		    const unsigned char nonce[GCM_NONCE_LEN],
		    const unsigned char *aad, size_t aad_len,
		    const unsigned char *ct, size_t ct_len,
		    const unsigned char tag[GCM_TAG_LEN],
		    unsigned char *pt);

#ifdef __cplusplus
}
#endif

#endif /* LRZIP_GCM_H */
