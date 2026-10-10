#pragma once
#include <yart/types.h>

/* SHA-256 (FIPS 180-4).  Used to hash passwords with a per-user salt so
 * the stored value is a real cryptographic digest, not a crackable hash. */
typedef struct {
    u32 state[8];
    u64 bitlen;
    u8  buf[64];
    u32 buflen;
} sha256_ctx_t;

void sha256_init(sha256_ctx_t *c);
void sha256_update(sha256_ctx_t *c, const void *data, size_t len);
void sha256_final(sha256_ctx_t *c, u8 out[32]);

/* one-shot: digest of (parts...) ; each part NULL-terminated string or NULL */
void sha256_strings(u8 out[32], const char *s1, const char *s2, const char *s3);
void sha256_to_hex(const u8 digest[32], char *hex64);
