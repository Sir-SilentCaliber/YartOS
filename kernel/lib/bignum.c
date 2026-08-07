/* Yart OS - 2048-bit bignum + RSA public operation - kernel/lib/bignum.c
 *
 * Montgomery multiplication (CIOS, 32-bit limbs) for modular
 * exponentiation - enough for RSA verify/encrypt with 2048-bit keys
 * (the TLS RSA key exchange: client encrypts the premaster with the
 * server's public key).  e is usually 65537, but a general exponent is
 * supported.
 */
#include <yart/types.h>
#include <yart/string.h>

#define BN_LIMBS 64                 /* 2048 bits */

typedef struct { u32 d[BN_LIMBS]; } bn_t;

static void bn_zero(bn_t *r) { memset(r, 0, sizeof *r); }

static int bn_cmp(const bn_t *a, const bn_t *b) {
    for (int i = BN_LIMBS - 1; i >= 0; i--) {
        if (a->d[i] > b->d[i]) return 1;
        if (a->d[i] < b->d[i]) return -1;
    }
    return 0;
}

/* r = a - b (a >= b) */
static void bn_sub(bn_t *r, const bn_t *a, const bn_t *b) {
    u64 borrow = 0;
    for (int i = 0; i < BN_LIMBS; i++) {
        u64 x = (u64)a->d[i] - (u64)b->d[i] - borrow;
        borrow = (x >> 32) & 1;
        r->d[i] = (u32)x;
    }
}

/* r = (a * 2) mod n  (uses only subtract) */
static void bn_dbl_mod(bn_t *r, const bn_t *a, const bn_t *n) {
    u64 carry = 0;
    for (int i = 0; i < BN_LIMBS; i++) {
        u64 x = ((u64)a->d[i] << 1) | carry;
        r->d[i] = (u32)x;
        carry = x >> 32;
    }
    if (carry || bn_cmp(r, n) >= 0) bn_sub(r, r, n);
}

/* Montgomery multiplication (CIOS): r = a*b*R^-1 mod n.
 * n0inv = -n^{-1} mod 2^32.  tmp must hold 2*BN_LIMBS+2 u32s. */
static void bn_mont_mul(bn_t *r, const bn_t *a, const bn_t *b,
                        const bn_t *n, u32 n0inv, u32 *tmp) {
    u32 *t = tmp;
    memset(t, 0, (BN_LIMBS + 2) * sizeof(u32));
    for (int i = 0; i < BN_LIMBS; i++) {
        u64 carry = 0;
        u32 ai = a->d[i];
        for (int j = 0; j < BN_LIMBS; j++) {
            u64 x = (u64)t[j] + (u64)ai * b->d[j] + carry;
            t[j] = (u32)x;
            carry = x >> 32;
        }
        u64 x = (u64)t[BN_LIMBS] + carry;
        t[BN_LIMBS] = (u32)x;
        t[BN_LIMBS + 1] = (u32)(x >> 32);
        /* m = t0 * n0inv mod 2^32 */
        u32 m = t[0] * n0inv;
        u64 c2 = 0;
        for (int j = 0; j < BN_LIMBS; j++) {
            u64 y = (u64)m * n->d[j] + t[j] + c2;
            t[j] = (u32)y;
            c2 = y >> 32;
        }
        u64 z = (u64)t[BN_LIMBS] + c2;
        t[BN_LIMBS] = (u32)z;
        t[BN_LIMBS + 1] += (u32)(z >> 32);
        for (int j = 0; j < BN_LIMBS + 1; j++) t[j] = t[j + 1];
    }
    bn_t tmp2;
    for (int i = 0; i < BN_LIMBS; i++) tmp2.d[i] = t[i];
    /* CARRY-LIMB FIX: after the last iteration the accumulated high
     * carry sits in t[BN_LIMBS] (0, 1 or 2).  It was silently dropped,
     * losing a whole R = 2^(32*64) mod n from the result - the TLS
     * client's RSA-encrypted premaster came out wrong for ~85% of
     * random inputs (only ~1 in 6 handshakes succeeded by luck).  The
     * true CIOS value is V = t[BN_LIMBS]*2^(32*64) + tmp2; reduce it
     * by subtracting n (with borrow out of the carry limb) until it
     * fits. */
    u64 hv = t[BN_LIMBS];
    while (hv) {
        u64 borrow = 0;
        for (int i = 0; i < BN_LIMBS; i++) {
            u64 x = (u64)tmp2.d[i] - (u64)n->d[i] - borrow;
            tmp2.d[i] = (u32)x;
            borrow = (x >> 32) & 1;
        }
        hv -= borrow;
    }
    if (bn_cmp(&tmp2, n) >= 0) bn_sub(&tmp2, &tmp2, n);
    *r = tmp2;
}

/* n0inv = -n^{-1} mod 2^32 (Newton iteration) */
static u32 bn_n0inv(const bn_t *n) {
    u32 x = 1;
    u32 n0 = n->d[0];
    for (int i = 0; i < 5; i++)
        x = x * (2 - n0 * x);        /* mod 2^32 arithmetic */
    return 0u - x;
}

/* r = base^exp mod n  (Montgomery ladder) */
static void bn_modpow(const bn_t *base, const bn_t *exp, const bn_t *n, bn_t *r) {
    u32 n0inv = bn_n0inv(n);
    u32 tmp[BN_LIMBS + 2];

    /* R2 = R^2 mod n with R = 2^(32*BN_LIMBS) */
    bn_t R, R2, one;
    bn_zero(&R); bn_zero(&R2); bn_zero(&one);
    R.d[0] = 1;
    /* R mod n = 2^(32*64) mod n: double 32*64 times */
    bn_zero(&R);
    R.d[0] = 1;
    for (int i = 0; i < 32 * BN_LIMBS; i++) bn_dbl_mod(&R, &R, n);
    /* R2 = R*R mod n: double R, 32*64 times */
    bn_zero(&R2);
    R2 = R;
    for (int i = 0; i < 32 * BN_LIMBS; i++) bn_dbl_mod(&R2, &R2, n);
    /* convert base: base_m = base * R mod n */
    bn_t base_m;
    bn_mont_mul(&base_m, base, &R2, n, n0inv, tmp);
    /* result starts at R (Montgomery 1) */
    bn_t acc;
    bn_mont_mul(&acc, &R, &R, n, n0inv, tmp);   /* R*R*R^-1 = R mod n */
    /* square-and-multiply over exponent bits (MSB first) */
    bool started = false;
    for (int i = BN_LIMBS - 1; i >= 0; i--) {
        for (int b = 31; b >= 0; b--) {
            if (started) {
                bn_t sq;
                bn_mont_mul(&sq, &acc, &acc, n, n0inv, tmp);
                acc = sq;
                if ((exp->d[i] >> b) & 1) {
                    bn_t m2;
                    bn_mont_mul(&m2, &acc, &base_m, n, n0inv, tmp);
                    acc = m2;
                }
            } else if ((exp->d[i] >> b) & 1) {
                acc = base_m;
                started = true;
            }
        }
    }
    /* convert back: r = acc * 1 * R^-1 = acc (integer form) */
    bn_t one_m;
    bn_t one_i;
    bn_zero(&one_i);
    one_i.d[0] = 1;
    bn_mont_mul(&one_m, &acc, &one_i, n, n0inv, tmp);
    *r = one_m;
}

/* ---- public API ---- */

/* RSA public operation: out = in^e mod n.
 * in/out/n are big-endian byte arrays of nlen bytes (2048-bit keys:
 * nlen=256).  Returns 0 on success. */
int bn_rsa_public(const u8 *in, const u8 *exp, int explen,
                  const u8 *n, int nlen, u8 *out) {
    if (nlen > 256) return -1;
    bn_t bn_in, bn_e, bn_n, bn_r;
    bn_zero(&bn_in); bn_zero(&bn_e); bn_zero(&bn_n); bn_zero(&bn_r);
    for (int i = 0; i < nlen; i++) {
        /* SHIFT FIX: (len-1-i)%4*8 - the old (3-(i&3))*8 assumed the
         * length was a multiple of 4; a 3-byte exponent 01 00 01 landed
         * at bits 24.. instead of 16.. and became 0x01000100, not
         * 65537.  (Also fixed the old loop loading in[i] into BOTH the
         * modulus and the base - n was never read.) */
        bn_n.d[(nlen - 1 - i) / 4] |=
            (u32)n[i] << (((nlen - 1 - i) % 4) * 8);    /* modulus */
        bn_in.d[(nlen - 1 - i) / 4] |=
            (u32)in[i] << (((nlen - 1 - i) % 4) * 8);   /* base */
    }
    for (int i = 0; i < explen; i++)
        bn_e.d[(explen - 1 - i) / 4] |=
            (u32)exp[i] << (((explen - 1 - i) % 4) * 8);
    bn_modpow(&bn_in, &bn_e, &bn_n, &bn_r);
    for (int i = 0; i < nlen; i++)
        out[i] = (u8)(bn_r.d[(nlen - 1 - i) / 4] >> ((3 - (i & 3)) * 8));
    return 0;
}

/* =====================================================================
 * RSA PRIVATE operation (CRT, PKCS#1 v2.0) - for the TLS server.
 * m = c^d mod n computed via:
 *   m1 = c^dp mod p, m2 = c^dq mod q
 *   h  = qinv * (m1 - m2) mod p        (qinv = q^-1 mod p, precomputed)
 *   m  = m2 + h*q  (then reduced mod n)
 * The modulus n is 2048 bits; p, q are 1024 bits.
 * ===================================================================== */

/* number of significant bits in a */
static int bn_bitlen(const bn_t *a) {
    for (int i = BN_LIMBS - 1; i >= 0; i--)
        if (a->d[i])
            for (int b = 31; b >= 0; b--)
                if (a->d[i] & (1u << b)) return i * 32 + b + 1;
    return 0;
}

/* left-shift by one bit (a -> r), no modulus */
static void bn_shl1(bn_t *r, const bn_t *a) {
    u32 carry = 0;
    for (int i = 0; i < BN_LIMBS; i++) {
        u32 nc = a->d[i] >> 31;
        r->d[i] = (a->d[i] << 1) | carry;
        carry = nc;
    }
}

/* r = a mod m  (binary long division; a may be up to 2048 bits,
 * m up to 1024 bits - both fit in bn_t). */
static void bn_mod(bn_t *r, const bn_t *a, const bn_t *m) {
    if (bn_cmp(a, m) < 0) { *r = *a; return; }
    int shift = bn_bitlen(a) - bn_bitlen(m);
    bn_t t = *m;
    for (int i = 0; i < shift; i++) bn_shl1(&t, &t);
    bn_t rem = *a;
    for (int i = 0; i <= shift; i++) {
        if (bn_cmp(&rem, &t) >= 0) bn_sub(&rem, &rem, &t);
        /* t >>= 1 */
        u32 carry = 0;
        for (int k = BN_LIMBS - 1; k >= 0; k--) {
            u32 nc = t.d[k] << 31;
            t.d[k] = (t.d[k] >> 1) | carry;
            carry = nc;
        }
    }
    *r = rem;
}

/* wide (128-limb) schoolbook multiply: w = a*b */
static void bn_mul_wide(u32 w[128], const bn_t *a, const bn_t *b) {
    memset(w, 0, 128 * sizeof(u32));
    for (int i = 0; i < BN_LIMBS; i++) {
        u32 ai = a->d[i];
        if (!ai) continue;
        u64 carry = 0;
        for (int j = 0; j < BN_LIMBS; j++) {
            u64 x = (u64)w[i + j] + (u64)ai * b->d[j] + carry;
            w[i + j] = (u32)x;
            carry = x >> 32;
        }
        u64 x = (u64)w[i + BN_LIMBS] + carry;
        w[i + BN_LIMBS] = (u32)x;
        w[i + BN_LIMBS + 1] += (u32)(x >> 32);
    }
}

/* w >= n ?  (n is BN_LIMBS limbs; w is 2*BN_LIMBS) */
static int bn_wide_cmp_n(const u32 w[128], const bn_t *n) {
    for (int i = 127; i >= BN_LIMBS; i--)
        if (w[i]) return 1;
    for (int i = BN_LIMBS - 1; i >= 0; i--) {
        if (w[i] > n->d[i]) return 1;
        if (w[i] < n->d[i]) return -1;
    }
    return 0;
}

static void bn_wide_sub_n(u32 w[128], const bn_t *n) {
    u64 borrow = 0;
    for (int i = 0; i < BN_LIMBS; i++) {
        u64 x = (u64)w[i] - n->d[i] - borrow;
        w[i] = (u32)x;
        borrow = (x >> 32) & 1;
    }
    for (int i = BN_LIMBS; i < 128 && borrow; i++) {
        u64 x = (u64)w[i] - borrow;
        w[i] = (u32)x;
        borrow = x >> 63;
    }
}

/* Compute R = 2^(32*BN_LIMBS) mod n and R2 = R*R mod n (Montgomery
 * conversion constants).  Both by repeated doubling - 2048 iterations
 * each; fine for one-shot server handshakes. */
static void bn_mont_consts(const bn_t *n, bn_t *R, bn_t *R2) {
    bn_zero(R); R->d[0] = 1;
    for (int i = 0; i < 32 * BN_LIMBS; i++) bn_dbl_mod(R, R, n);
    *R2 = *R;
    for (int i = 0; i < 32 * BN_LIMBS; i++) bn_dbl_mod(R2, R2, n);
}

/* Normal-form modular multiply: r = (a * b) mod n.
 * MONTGOMERY-FORM FIX: mont_mul computes a*b*R^-1 - usable as a plain
 * multiply only when both operands are in Montgomery form (x*R mod n).
 * This helper converts in, multiplies, converts back. */
static void bn_mod_mul(bn_t *r, const bn_t *a, const bn_t *b, const bn_t *n) {
    bn_t R, R2, a_m, b_m, t, one;
    bn_mont_consts(n, &R, &R2);
    u32 n0inv = bn_n0inv(n);
    u32 tmp[BN_LIMBS + 2];
    bn_mont_mul(&a_m, a, &R2, n, n0inv, tmp);   /* a*R mod n   */
    bn_mont_mul(&b_m, b, &R2, n, n0inv, tmp);   /* b*R mod n   */
    bn_mont_mul(&t, &a_m, &b_m, n, n0inv, tmp); /* a*b*R mod n */
    bn_zero(&one); one.d[0] = 1;
    bn_mont_mul(r, &t, &one, n, n0inv, tmp);    /* a*b mod n   */
}

/* RSA-CRT decrypt: out = in^d mod n using the embedded private halves. */
int bn_rsa_private_crt(const u8 *in, int nlen,
                       const u8 *p, int plen, const u8 *q, int qlen,
                       const u8 *dp, const u8 *dq, const u8 *qinv,
                       const u8 *n, u8 *out) {
    if (nlen > 256 || plen > 128 || qlen > 128) return -1;
    bn_t bn_c, bn_p, bn_q, bn_dp, bn_dq, bn_qinv, bn_n;
    bn_zero(&bn_c); bn_zero(&bn_p); bn_zero(&bn_q);
    bn_zero(&bn_dp); bn_zero(&bn_dq); bn_zero(&bn_qinv); bn_zero(&bn_n);
    for (int i = 0; i < nlen; i++)
        bn_c.d[(nlen - 1 - i) / 4] |= (u32)in[i] << (((nlen - 1 - i) % 4) * 8);
    for (int i = 0; i < plen; i++)
        bn_p.d[(plen - 1 - i) / 4] |= (u32)p[i] << (((plen - 1 - i) % 4) * 8);
    for (int i = 0; i < qlen; i++)
        bn_q.d[(qlen - 1 - i) / 4] |= (u32)q[i] << (((qlen - 1 - i) % 4) * 8);
    for (int i = 0; i < plen; i++)
        bn_dp.d[(plen - 1 - i) / 4] |= (u32)dp[i] << (((plen - 1 - i) % 4) * 8);
    for (int i = 0; i < qlen; i++)
        bn_dq.d[(qlen - 1 - i) / 4] |= (u32)dq[i] << (((qlen - 1 - i) % 4) * 8);
    for (int i = 0; i < plen; i++)
        bn_qinv.d[(plen - 1 - i) / 4] |= (u32)qinv[i] << (((plen - 1 - i) % 4) * 8);
    for (int i = 0; i < nlen; i++)
        bn_n.d[(nlen - 1 - i) / 4] |= (u32)n[i] << (((nlen - 1 - i) % 4) * 8);

    /* reduce c mod p and mod q */
    bn_t c_mod_p, c_mod_q;
    bn_mod(&c_mod_p, &bn_c, &bn_p);
    bn_mod(&c_mod_q, &bn_c, &bn_q);

    bn_t m1, m2;
    bn_modpow(&c_mod_p, &bn_dp, &bn_p, &m1);
    bn_modpow(&c_mod_q, &bn_dq, &bn_q, &m2);

    /* h = qinv * (m1 - m2) mod p ; add p once to keep it positive */
    bn_t diff;
    if (bn_cmp(&m1, &m2) >= 0) bn_sub(&diff, &m1, &m2);
    else { bn_sub(&diff, &m2, &m1); bn_sub(&diff, &bn_p, &diff); }
    bn_t h;
    bn_mod_mul(&h, &bn_qinv, &diff, &bn_p);

    /* m = m2 + h*q, then reduce mod n once (h*q < n, m2 < q < n) */
    u32 w[128];
    bn_mul_wide(w, &h, &bn_q);
    for (int i = 0; i < BN_LIMBS; i++) {
        u64 x = (u64)w[i] + m2.d[i];
        w[i] = (u32)x;
        u64 carry = x >> 32;
        for (int k = i + 1; carry && k < 128; k++) {
            u64 y = (u64)w[k] + carry;
            w[k] = (u32)y;
            carry = y >> 32;
        }
    }
    if (bn_wide_cmp_n(w, &bn_n) >= 0)
        bn_wide_sub_n(w, &bn_n);

    for (int i = 0; i < nlen; i++)
        out[i] = (u8)(w[(nlen - 1 - i) / 4] >> (((nlen - 1 - i) % 4) * 8));
    return 0;
}
