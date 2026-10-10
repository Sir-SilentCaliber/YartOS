#pragma once
#include <yart/types.h>

/* SHA-1 (FIPS 180-4), HMAC-SHA1 (RFC 2104) and PBKDF2-HMAC-SHA1 (RFC 2898).
 * Added for WPA2-PSK: the Pairwise Master Key is
 *   PMK = PBKDF2-HMAC-SHA1(passphrase, ssid, 4096, 256)
 * so a byte-exact SHA-1/HMAC/PBKDF2 is the first hard prerequisite for any
 * real Wi-Fi handshake.  Self-test covers the FIPS/RFC vectors plus the
 * official IEEE 802.11 WPA2-PSK example vector. */
typedef struct {
    u32 h[5];
    u64 len;          /* total bytes processed */
    u8  buf[64];
    u32 buf_len;
} sha1_ctx_t;

void sha1_init(sha1_ctx_t *c);
void sha1_update(sha1_ctx_t *c, const void *data, size_t len);
void sha1_final(sha1_ctx_t *c, u8 out[20]);

void hmac_sha1(const u8 *key, size_t keylen,
               const u8 *data, size_t datalen, u8 out[20]);

/* PBKDF2-HMAC-SHA1: derive `outlen` bytes (WPA2 uses 32 = the PMK). */
void pbkdf2_hmac_sha1(const char *passphrase, size_t plen,
                      const u8 *salt, size_t slen,
                      u32 iterations, u8 *out, size_t outlen);

/* 0 = all vectors matched */
int sha1_selftest(void);
