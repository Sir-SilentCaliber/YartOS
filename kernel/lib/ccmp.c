/* Yart OS - AES-CCMP (IEEE 802.11i) on top of the existing AES-128 block
 * cipher.  CCMP = CCM (RFC 3610) with M=8 and L=2, i.e. an 8-octet MIC and
 * a 13-octet nonce — the mode used to encrypt/authenticate every WPA2 data
 * frame.  Self-test pins RFC 3610 Packet Vectors #1/#3/#13.
 */
#include <yart/types.h>
#include <yart/string.h>
#include <yart/ccmp.h>

extern void aes128_keyexpand(const u8 key[16], u8 rk[176]);
extern void aes128_encrypt_block(const u8 rk[176], const u8 in[16], u8 out[16]);

#define CCM_M 8
#define CCM_L 2
#define NONCE_LEN (15 - CCM_L)   /* 13 */

static void xor16(u8 *d, const u8 *s) {
    for (int i = 0; i < 16; i++) d[i] ^= s[i];
}

/* B_0 = Flags || nonce || l(m).  Flags = 64*Adata + 8*M' + L'. */
static void ccm_b0(u8 b0[16], const u8 *nonce, size_t mlen, size_t alen) {
    b0[0] = (alen ? 0x40 : 0x00) | (u8)(((CCM_M - 2) / 2) << 3) | (u8)(CCM_L - 1);
    memcpy(b0 + 1, nonce, NONCE_LEN);
    for (int i = 0; i < CCM_L; i++)
        b0[15 - i] = (u8)(mlen >> (8 * i));
}

/* CTR_i = L' || nonce || counter(L bytes, big-endian, starts at 0). */
static void ccm_ctr0(u8 ctr[16], const u8 *nonce) {
    ctr[0] = (u8)(CCM_L - 1);
    memcpy(ctr + 1, nonce, NONCE_LEN);
    ctr[14] = 0;
    ctr[15] = 0;
}

static void ctr_inc(u8 ctr[16]) {
    u16 c = ((u16)ctr[14] << 8) | ctr[15];
    c++;
    ctr[14] = (u8)(c >> 8);
    ctr[15] = (u8)c;
}

/* Feed `len` bytes into the CBC-MAC chain (zero-pads a trailing partial
 * block; does NOT add a padding block when len is a multiple of 16). */
static void mac_feed(const u8 rk[176], u8 x[16], const u8 *data, size_t len) {
    u8 blk[16];
    while (len >= 16) {
        memcpy(blk, data, 16);
        xor16(x, blk);
        aes128_encrypt_block(rk, x, x);
        data += 16; len -= 16;
    }
    if (len) {
        memset(blk, 0, 16);
        memcpy(blk, data, len);
        xor16(x, blk);
        aes128_encrypt_block(rk, x, x);
    }
}

static void ccm_mac(const u8 rk[176], const u8 b0[16],
                    const u8 *aad, size_t alen,
                    const u8 *pt, size_t plen, u8 mac[16]) {
    u8 x[16], blk[16];
    aes128_encrypt_block(rk, b0, x);
    if (alen) {
        memset(blk, 0, 16);
        if (alen < 0xFF00) {                 /* 2-octet length field */
            blk[0] = (u8)(alen >> 8);
            blk[1] = (u8)alen;
            size_t first = alen < 14 ? alen : 14;
            memcpy(blk + 2, aad, first);
            xor16(x, blk);
            aes128_encrypt_block(rk, x, x);
            if (alen > first) mac_feed(rk, x, aad + first, alen - first);
        } else {                             /* 0xFF 0xFE || 32-bit length */
            blk[0] = 0xFF; blk[1] = 0xFE;
            blk[2] = (u8)(alen >> 24); blk[3] = (u8)(alen >> 16);
            blk[4] = (u8)(alen >> 8);  blk[5] = (u8)alen;
            size_t first = alen < 10 ? alen : 10;
            memcpy(blk + 6, aad, first);
            xor16(x, blk);
            aes128_encrypt_block(rk, x, x);
            if (alen > first) mac_feed(rk, x, aad + first, alen - first);
        }
    }
    mac_feed(rk, x, pt, plen);
    memcpy(mac, x, 16);
}

static void ccm_ctr(const u8 rk[176], const u8 ctr0[16],
                    const u8 *in, u8 *out, size_t len) {
    u8 ctr[16], ks[16];
    memcpy(ctr, ctr0, 16);
    size_t i = 0;
    while (len >= 16) {
        ctr_inc(ctr);
        aes128_encrypt_block(rk, ctr, ks);
        for (int j = 0; j < 16; j++) out[i + j] = in[i + j] ^ ks[j];
        i += 16; len -= 16;
    }
    if (len) {
        ctr_inc(ctr);
        aes128_encrypt_block(rk, ctr, ks);
        for (size_t j = 0; j < len; j++) out[i + j] = in[i + j] ^ ks[j];
    }
}

void ccmp_encrypt(const u8 key[16], const u8 nonce[13],
                  const u8 *aad, size_t aad_len,
                  const u8 *pt, size_t pt_len,
                  u8 *ct_out, u8 mic[8]) {
    u8 rk[176];
    aes128_keyexpand(key, rk);
    u8 b0[16], ctr0[16], mac[16], s0[16];
    ccm_b0(b0, nonce, pt_len, aad_len);
    ccm_ctr0(ctr0, nonce);
    ccm_mac(rk, b0, aad, aad_len, pt, pt_len, mac);
    aes128_encrypt_block(rk, ctr0, s0);
    for (int i = 0; i < 8; i++) mic[i] = mac[i] ^ s0[i];
    ccm_ctr(rk, ctr0, pt, ct_out, pt_len);
}

int ccmp_decrypt(const u8 key[16], const u8 nonce[13],
                 const u8 *aad, size_t aad_len,
                 const u8 *ct, size_t ct_len, const u8 mic[8],
                 u8 *pt_out) {
    u8 rk[176];
    aes128_keyexpand(key, rk);
    u8 b0[16], ctr0[16], mac[16], s0[16], expect[8];
    ccm_ctr0(ctr0, nonce);
    ccm_ctr(rk, ctr0, ct, pt_out, ct_len);
    ccm_b0(b0, nonce, ct_len, aad_len);
    ccm_mac(rk, b0, aad, aad_len, pt_out, ct_len, mac);
    aes128_encrypt_block(rk, ctr0, s0);
    for (int i = 0; i < 8; i++) expect[i] = mac[i] ^ s0[i];
    u8 d = 0;                                /* constant-time compare */
    for (int i = 0; i < 8; i++) d |= expect[i] ^ mic[i];
    return d ? -1 : 0;
}

int ccmp_selftest(void) {
    /* ---- RFC 3610 Packet Vector #1 / NIST SP 800-38C C.1 (M=8, L=2, 8-byte AAD) ---- */
    {
        static const u8 key[16] = {0xC0,0xC1,0xC2,0xC3,0xC4,0xC5,0xC6,0xC7,
                                   0xC8,0xC9,0xCA,0xCB,0xCC,0xCD,0xCE,0xCF};
        static const u8 nonce[13] = {0x00,0x00,0x00,0x03,0x02,0x01,0x00,
                                     0xA0,0xA1,0xA2,0xA3,0xA4,0xA5};
        static const u8 aad[8] = {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07};
        static const u8 pt[23] = {0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,
                                  0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,
                                  0x18,0x19,0x1A,0x1B,0x1C,0x1D,0x1E};
        static const u8 ct_want[23] = {0x58,0x8C,0x97,0x9A,0x61,0xC6,0x63,0xD2,
                                       0xF0,0x66,0xD0,0xC2,0xC0,0xF9,0x89,0x80,
                                       0x6D,0x5F,0x6B,0x61,0xDA,0xC3,0x84};
        static const u8 mic_want[8] = {0x17,0xE8,0xD1,0x2C,0xFD,0xF9,0x26,0xE0};
        u8 ct[23], mic[8];
        ccmp_encrypt(key, nonce, aad, 8, pt, 23, ct, mic);
        if (memcmp(ct, ct_want, 23) || memcmp(mic, mic_want, 8)) return 1;
        u8 pt2[23];
        if (ccmp_decrypt(key, nonce, aad, 8, ct, 23, mic, pt2) || memcmp(pt2, pt, 23)) return 2;
    }

    /* ---- RFC 3610 Packet Vector #3 (M=8, L=2, 8-byte AAD, 25-byte msg) ---- */
    {
        static const u8 key[16] = {0xC0,0xC1,0xC2,0xC3,0xC4,0xC5,0xC6,0xC7,
                                   0xC8,0xC9,0xCA,0xCB,0xCC,0xCD,0xCE,0xCF};
        static const u8 nonce[13] = {0x00,0x00,0x00,0x05,0x04,0x03,0x02,
                                     0xA0,0xA1,0xA2,0xA3,0xA4,0xA5};
        static const u8 aad[8] = {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07};
        static const u8 pt[25] = {0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,
                                  0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,
                                  0x18,0x19,0x1A,0x1B,0x1C,0x1D,0x1E,0x1F,0x20};
        static const u8 ct_want[25] = {0x51,0xB1,0xE5,0xF4,0x4A,0x19,0x7D,0x1D,
                                       0xA4,0x6B,0x0F,0x8E,0x2D,0x28,0x2A,0xE8,
                                       0x71,0xE8,0x38,0xBB,0x64,0xDA,0x85,0x96,0x57};
        static const u8 mic_want[8] = {0x4A,0xDA,0xA7,0x6F,0xBD,0x9F,0xB0,0xC5};
        u8 ct[25], mic[8];
        ccmp_encrypt(key, nonce, aad, 8, pt, 25, ct, mic);
        if (memcmp(ct, ct_want, 25) || memcmp(mic, mic_want, 8)) return 3;
        u8 pt2[25];
        if (ccmp_decrypt(key, nonce, aad, 8, ct, 25, mic, pt2) || memcmp(pt2, pt, 25)) return 4;
    }

    /* ---- RFC 3610 Packet Vector #13 (M=8, L=2, 8-byte AAD, 23-byte msg) ---- */
    {
        static const u8 key[16] = {0xD7,0x82,0x8D,0x13,0xB2,0xB0,0xBD,0xC3,
                                   0x25,0xA7,0x62,0x36,0xDF,0x93,0xCC,0x6B};
        static const u8 nonce[13] = {0x00,0x41,0x2B,0x4E,0xA9,0xCD,0xBE,0x3C,
                                     0x96,0x96,0x76,0x6C,0xFA};
        static const u8 aad[8] = {0x0B,0xE1,0xA8,0x8B,0xAC,0xE0,0x18,0xB1};
        static const u8 pt[23] = {0x08,0xE8,0xCF,0x97,0xD8,0x20,0xEA,0x25,
                                  0x84,0x60,0xE9,0x6A,0xD9,0xCF,0x52,0x89,
                                  0x05,0x4D,0x89,0x5C,0xEA,0xC4,0x7C};
        static const u8 ct_want[23] = {0x4C,0xB9,0x7F,0x86,0xA2,0xA4,0x68,0x9A,
                                       0x87,0x79,0x47,0xAB,0x80,0x91,0xEF,0x53,
                                       0x86,0xA6,0xFF,0xBD,0xD0,0x80,0xF8};
        static const u8 mic_want[8] = {0xE7,0x8C,0xF7,0xCB,0x0C,0xDD,0xD7,0xB3};
        u8 ct[23], mic[8];
        ccmp_encrypt(key, nonce, aad, 8, pt, 23, ct, mic);
        if (memcmp(ct, ct_want, 23) || memcmp(mic, mic_want, 8)) return 5;
        u8 pt2[23];
        if (ccmp_decrypt(key, nonce, aad, 8, ct, 23, mic, pt2) || memcmp(pt2, pt, 23)) return 6;
    }

    return 0;
}
