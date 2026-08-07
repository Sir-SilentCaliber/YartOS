/* Yart OS - HMAC-SHA256 (RFC 2104) - kernel/lib/hmac.c */
#include <yart/sha256.h>
#include <yart/string.h>

void hmac_sha256(const u8 *key, size_t keylen,
                 const u8 *data, size_t datalen, u8 out[32]) {
    u8 k[64];
    memset(k, 0, sizeof k);
    if (keylen > 64) {
        sha256_ctx_t c;
        sha256_init(&c);
        sha256_update(&c, key, keylen);
        sha256_final(&c, k);          /* 32 bytes, rest zero */
    } else {
        memcpy(k, key, keylen);
    }
    u8 ipad[64], opad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5C;
    }
    sha256_ctx_t c;
    sha256_init(&c);
    sha256_update(&c, ipad, 64);
    sha256_update(&c, data, datalen);
    u8 inner[32];
    sha256_final(&c, inner);
    sha256_init(&c);
    sha256_update(&c, opad, 64);
    sha256_update(&c, inner, 32);
    sha256_final(&c, out);
}
