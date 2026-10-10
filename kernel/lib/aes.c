/* Yart OS - AES-128 (FIPS 197) - kernel/lib/aes.c
 * Compact implementation: S-box computed at init from the GF(2^8)
 * inverse + affine transform (no 256-byte table to mistype), standard
 * key expansion, encrypt/decrypt blocks + CBC modes.
 */
#include <yart/types.h>
#include <yart/string.h>

static u8 g_sbox[256], g_isbox[256];
static bool g_aes_ready;

static u8 gf_mul(u8 a, u8 b) {
    u8 r = 0;
    for (int i = 0; i < 8; i++) {
        if (b & 1) r ^= a;
        u8 hi = a & 0x80;
        a <<= 1;
        if (hi) a ^= 0x1B;
        b >>= 1;
    }
    return r;
}

static u8 gf_inv(u8 x) {          /* x^254 in GF(2^8) */
    u8 r = 1;
    for (int i = 0; i < 254; i++) r = gf_mul(r, x);
    return r;
}

static void aes_build_tables(void) {
    for (int i = 0; i < 256; i++) {
        u8 inv = (i == 0) ? 0 : gf_inv((u8)i);
        /* affine transform with c=0x63 */
        u8 s = 0;
        for (int b = 0; b < 8; b++) {
            u8 bit = ((inv >> b) & 1) ^ ((inv >> ((b + 4) & 7)) & 1) ^
                     ((inv >> ((b + 5) & 7)) & 1) ^ ((inv >> ((b + 6) & 7)) & 1) ^
                     ((inv >> ((b + 7) & 7)) & 1) ^ ((0x63 >> b) & 1);
            s |= (u8)(bit << b);
        }
        g_sbox[i] = s;
        g_isbox[s] = (u8)i;
    }
    g_aes_ready = true;
}

/* expand a 16-byte key into 11 round keys (176 bytes) */
void aes128_keyexpand(const u8 key[16], u8 rk[176]) {
    if (!g_aes_ready) aes_build_tables();
    memcpy(rk, key, 16);
    u8 rcon = 1;
    for (int i = 1; i <= 10; i++) {
        int base = i * 16;
        /* w[i] = w[i-4] ^ g(w[i-1]); g = RotWord+SubWord+Rcon */
        u8 t[4];
        t[0] = g_sbox[rk[base - 3]];
        t[1] = g_sbox[rk[base - 2]];
        t[2] = g_sbox[rk[base - 1]];
        t[3] = g_sbox[rk[base - 4]];
        t[0] ^= rcon;
        rcon = gf_mul(rcon, 2);
        for (int j = 0; j < 4; j++) {
            rk[base + j] = rk[base - 16 + j] ^ t[j];
            rk[base + 4 + j] = rk[base - 12 + j] ^ rk[base + j];
            rk[base + 8 + j] = rk[base - 8 + j] ^ rk[base + 4 + j];
            rk[base + 12 + j] = rk[base - 4 + j] ^ rk[base + 8 + j];
        }
    }
}

static void add_round_key(u8 s[16], const u8 *rk) {
    for (int i = 0; i < 16; i++) s[i] ^= rk[i];
}
static void sub_bytes(u8 s[16]) { for (int i = 0; i < 16; i++) s[i] = g_sbox[s[i]]; }
static void inv_sub_bytes(u8 s[16]) { for (int i = 0; i < 16; i++) s[i] = g_isbox[s[i]]; }
static void shift_rows(u8 s[16]) {
    u8 t;
    t = s[1]; s[1] = s[5]; s[5] = s[9]; s[9] = s[13]; s[13] = t;
    t = s[2]; s[2] = s[10]; s[10] = t; t = s[6]; s[6] = s[14]; s[14] = t;
    t = s[15]; s[15] = s[11]; s[11] = s[7]; s[7] = s[3]; s[3] = t;
}
static void inv_shift_rows(u8 s[16]) {
    u8 t;
    t = s[13]; s[13] = s[9]; s[9] = s[5]; s[5] = s[1]; s[1] = t;
    t = s[2]; s[2] = s[10]; s[10] = t; t = s[6]; s[6] = s[14]; s[14] = t;
    t = s[3]; s[3] = s[7]; s[7] = s[11]; s[11] = s[15]; s[15] = t;
}
static void mix_columns(u8 s[16]) {
    for (int c = 0; c < 4; c++) {
        u8 *p = s + c * 4;
        u8 a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
        p[0] = (u8)(gf_mul(a0, 2) ^ gf_mul(a1, 3) ^ a2 ^ a3);
        p[1] = (u8)(a0 ^ gf_mul(a1, 2) ^ gf_mul(a2, 3) ^ a3);
        p[2] = (u8)(a0 ^ a1 ^ gf_mul(a2, 2) ^ gf_mul(a3, 3));
        p[3] = (u8)(gf_mul(a0, 3) ^ a1 ^ a2 ^ gf_mul(a3, 2));
    }
}
static void inv_mix_columns(u8 s[16]) {
    for (int c = 0; c < 4; c++) {
        u8 *p = s + c * 4;
        u8 a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
        p[0] = (u8)(gf_mul(a0, 14) ^ gf_mul(a1, 11) ^ gf_mul(a2, 13) ^ gf_mul(a3, 9));
        p[1] = (u8)(gf_mul(a0, 9) ^ gf_mul(a1, 14) ^ gf_mul(a2, 11) ^ gf_mul(a3, 13));
        p[2] = (u8)(gf_mul(a0, 13) ^ gf_mul(a1, 9) ^ gf_mul(a2, 14) ^ gf_mul(a3, 11));
        p[3] = (u8)(gf_mul(a0, 11) ^ gf_mul(a1, 13) ^ gf_mul(a2, 9) ^ gf_mul(a3, 14));
    }
}

void aes128_encrypt_block(const u8 rk[176], const u8 in[16], u8 out[16]) {
    u8 s[16];
    memcpy(s, in, 16);
    add_round_key(s, rk);
    for (int r = 1; r <= 9; r++) {
        sub_bytes(s);
        shift_rows(s);
        mix_columns(s);
        add_round_key(s, rk + r * 16);
    }
    sub_bytes(s);
    shift_rows(s);
    add_round_key(s, rk + 160);
    memcpy(out, s, 16);
}

void aes128_decrypt_block(const u8 rk[176], const u8 in[16], u8 out[16]) {
    u8 s[16];
    memcpy(s, in, 16);
    add_round_key(s, rk + 160);
    for (int r = 9; r >= 1; r--) {
        inv_shift_rows(s);
        inv_sub_bytes(s);
        add_round_key(s, rk + r * 16);
        inv_mix_columns(s);
    }
    inv_shift_rows(s);
    inv_sub_bytes(s);
    add_round_key(s, rk);
    memcpy(out, s, 16);
}

/* CBC encrypt: iv in/out (last ciphertext block returned).  len % 16 == 0. */
void aes128_cbc_encrypt(const u8 rk[176], u8 iv[16], u8 *data, size_t len) {
    u8 block[16];
    for (size_t off = 0; off < len; off += 16) {
        for (int i = 0; i < 16; i++) block[i] = data[off + i] ^ iv[i];
        aes128_encrypt_block(rk, block, data + off);
        memcpy(iv, data + off, 16);
    }
}

void aes128_cbc_decrypt(const u8 rk[176], const u8 iv[16], u8 *data, size_t len) {
    u8 block[16], prev[16];
    memcpy(prev, iv, 16);
    for (size_t off = 0; off < len; off += 16) {
        memcpy(block, data + off, 16);
        aes128_decrypt_block(rk, block, data + off);
        for (int i = 0; i < 16; i++) data[off + i] ^= prev[i];
        memcpy(prev, block, 16);
    }
}
