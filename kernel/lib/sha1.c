/* Yart OS - SHA-1 (FIPS 180-4) + HMAC-SHA1 (RFC 2104) + PBKDF2 (RFC 2898).
 *
 * This exists for WPA2-PSK: PMK = PBKDF2-HMAC-SHA1(passphrase, ssid, 4096).
 * The self-test pins the FIPS 180-4 / RFC 3174 digests, an RFC 2202 HMAC
 * vector and the official IEEE 802.11 WPA2-PSK example PMK, so we know the
 * key derivation is byte-exact before a single radio frame depends on it.
 */
#include <yart/types.h>
#include <yart/string.h>
#include <yart/sha1.h>

static u32 rol32(u32 x, int n) { return (x << n) | (x >> (32 - n)); }

static void sha1_block(sha1_ctx_t *c, const u8 *p) {
    u32 w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((u32)p[4 * i] << 24) | ((u32)p[4 * i + 1] << 16) |
               ((u32)p[4 * i + 2] << 8) | (u32)p[4 * i + 3];
    for (int i = 16; i < 80; i++)
        w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    u32 a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4];
    for (int i = 0; i < 80; i++) {
        u32 f, k;
        if (i < 20)        { f = (b & cc) | (~b & d);          k = 0x5A827999; }
        else if (i < 40)   { f = b ^ cc ^ d;                    k = 0x6ED9EBA1; }
        else if (i < 60)   { f = (b & cc) | (b & d) | (cc & d); k = 0x8F1BBCDC; }
        else               { f = b ^ cc ^ d;                    k = 0xCA62C1D6; }
        u32 t = rol32(a, 5) + f + e + k + w[i];
        e = d; d = cc; cc = rol32(b, 30); b = a; a = t;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e;
}

void sha1_init(sha1_ctx_t *c) {
    c->h[0] = 0x67452301; c->h[1] = 0xEFCDAB89; c->h[2] = 0x98BADCFE;
    c->h[3] = 0x10325476; c->h[4] = 0xC3D2E1F0;
    c->len = 0;
    c->buf_len = 0;
}

void sha1_update(sha1_ctx_t *c, const void *data, size_t len) {
    const u8 *p = data;
    c->len += len;
    while (len) {
        size_t take = 64 - c->buf_len;
        if (take > len) take = len;
        memcpy(c->buf + c->buf_len, p, take);
        c->buf_len += take; p += take; len -= take;
        if (c->buf_len == 64) { sha1_block(c, c->buf); c->buf_len = 0; }
    }
}

void sha1_final(sha1_ctx_t *c, u8 out[20]) {
    u64 bitlen = c->len * 8;
    u8 pad = 0x80;
    sha1_update(c, &pad, 1);
    u8 zero = 0;
    while (c->buf_len != 56) sha1_update(c, &zero, 1);
    u8 lenb[8];
    for (int i = 0; i < 8; i++) lenb[i] = (u8)(bitlen >> (56 - 8 * i));
    sha1_update(c, lenb, 8);
    for (int i = 0; i < 5; i++) {
        out[4 * i]     = (u8)(c->h[i] >> 24);
        out[4 * i + 1] = (u8)(c->h[i] >> 16);
        out[4 * i + 2] = (u8)(c->h[i] >> 8);
        out[4 * i + 3] = (u8)(c->h[i]);
    }
}

void hmac_sha1(const u8 *key, size_t keylen,
               const u8 *data, size_t datalen, u8 out[20]) {
    u8 k[64];
    memset(k, 0, sizeof k);
    if (keylen > 64) {
        sha1_ctx_t c;
        sha1_init(&c); sha1_update(&c, key, keylen); sha1_final(&c, k);
    } else {
        memcpy(k, key, keylen);
    }
    u8 ipad[64], opad[64];
    for (int i = 0; i < 64; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5C; }
    sha1_ctx_t c;
    u8 inner[20];
    sha1_init(&c); sha1_update(&c, ipad, 64); sha1_update(&c, data, datalen); sha1_final(&c, inner);
    sha1_init(&c); sha1_update(&c, opad, 64); sha1_update(&c, inner, 20); sha1_final(&c, out);
}

void pbkdf2_hmac_sha1(const char *passphrase, size_t plen,
                      const u8 *salt, size_t slen,
                      u32 iterations, u8 *out, size_t outlen) {
    u32 block = 1;
    while (outlen) {
        u8 msg[64];                       /* salt (SSID <= 32) + 4-byte counter */
        memcpy(msg, salt, slen);
        msg[slen + 0] = (u8)(block >> 24);
        msg[slen + 1] = (u8)(block >> 16);
        msg[slen + 2] = (u8)(block >> 8);
        msg[slen + 3] = (u8)block;
        u8 u[20], t[20];
        hmac_sha1((const u8 *)passphrase, plen, msg, slen + 4, u);
        memcpy(t, u, 20);
        for (u32 i = 1; i < iterations; i++) {
            hmac_sha1((const u8 *)passphrase, plen, u, 20, u);
            for (int j = 0; j < 20; j++) t[j] ^= u[j];
        }
        size_t take = outlen > 20 ? 20 : outlen;
        memcpy(out, t, take);
        out += take; outlen -= take; block++;
    }
}

int sha1_selftest(void) {
    u8 d[20];
    sha1_ctx_t c;

    /* FIPS 180-4 / RFC 3174: SHA1("abc") */
    sha1_init(&c); sha1_update(&c, "abc", 3); sha1_final(&c, d);
    static const u8 v_abc[20] = {
        0xa9,0x99,0x3e,0x36,0x47,0x06,0x81,0x6a,0xba,0x3e,
        0x25,0x71,0x78,0x50,0xc2,0x6c,0x9c,0xd0,0xd8,0x9d };
    if (memcmp(d, v_abc, 20)) return 1;

    /* FIPS 180-4: SHA1("") */
    sha1_init(&c); sha1_update(&c, "", 0); sha1_final(&c, d);
    static const u8 v_empty[20] = {
        0xda,0x39,0xa3,0xee,0x5e,0x6b,0x4b,0x0d,0x32,0x55,
        0xbf,0xef,0x95,0x60,0x18,0x90,0xaf,0xd8,0x07,0x09 };
    if (memcmp(d, v_empty, 20)) return 2;

    /* RFC 2202 test case 2: HMAC-SHA1(key="Jefe", data="what do ya want for nothing?") */
    hmac_sha1((const u8 *)"Jefe", 4,
              (const u8 *)"what do ya want for nothing?", 28, d);
    static const u8 v_hmac[20] = {
        0xef,0xfc,0xdf,0x6a,0xe5,0xeb,0x2f,0xa2,0xd2,0x74,
        0x16,0xd5,0xf1,0x84,0xdf,0x9c,0x25,0x9a,0x7c,0x79 };
    if (memcmp(d, v_hmac, 20)) return 3;

    /* IEEE 802.11 WPA2-PSK example: passphrase "password", SSID "IEEE", 4096
     * iterations -> 256-bit PMK. */
    u8 pmk[32];
    pbkdf2_hmac_sha1("password", 8, (const u8 *)"IEEE", 4, 4096, pmk, 32);
    static const u8 v_pmk[32] = {
        0xf4,0x2c,0x6f,0xc5,0x2d,0xf0,0xeb,0xef,0x9e,0xbb,0x4b,0x90,0xb3,0x8a,0x5f,0x90,
        0x2e,0x83,0xfe,0x1b,0x13,0x5a,0x70,0xe2,0x3a,0xed,0x76,0x2e,0x97,0x10,0xa1,0x2e };
    if (memcmp(pmk, v_pmk, 32)) return 4;

    return 0;
}
