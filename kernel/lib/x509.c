/* Yart OS - minimal X.509 / DER parser - kernel/lib/x509.c
 *
 * Extracts the RSA public key (modulus + exponent) from a DER-encoded
 * certificate - just enough for the TLS client to encrypt the premaster.
 * No chain validation, no expiry checks (the test client accepts any
 * certificate, like curl -k).
 */
#include <yart/types.h>
#include <yart/string.h>

typedef struct {
    u8  tag;
    const u8 *v;
    u32 len;
} der_t;

/* Parse a TLV at p (len bytes available).  Returns 0 + fills out. */
static int der_read(const u8 *p, u32 len, der_t *out) {
    if (len < 2) return -1;
    out->tag = p[0];
    u32 l = p[1];
    u32 hdr = 2;
    if (l & 0x80) {
        u32 nbytes = l & 0x7F;
        if (nbytes == 0 || nbytes > 4 || len < 2 + nbytes) return -1;
        l = 0;
        for (u32 i = 0; i < nbytes; i++) l = (l << 8) | p[2 + i];
        hdr = 2 + nbytes;
    }
    if (hdr + l > len) return -1;
    out->v = p + hdr;
    out->len = l;
    return 0;
}

/* Find the SubjectPublicKeyInfo BIT STRING inside a DER certificate:
 * Certificate ::= SEQUENCE { tbs SEQUENCE, sigalg, sig }
 * tbs ::= SEQUENCE { [0] version?, serial, sig, issuer, validity,
 *                    subject, spki SEQUENCE { alg, BIT STRING }, ... }
 * Walk: outer SEQUENCE -> tbs SEQUENCE -> scan its children for the
 * SEQUENCE whose first element is an OID (0x06) and second is a BIT
 * STRING (0x03) - that is the SPKI. */
int x509_get_spki_bitstring(const u8 *cert, u32 certlen,
                            const u8 **bits, u32 *bitslen) {
    der_t top;
    if (der_read(cert, certlen, &top) != 0) return -1;
    if (top.tag != 0x30) return -1;            /* SEQUENCE */

    der_t tbs;
    if (der_read(top.v, top.len, &tbs) != 0) return -1;
    if (tbs.tag != 0x30) return -1;

    const u8 *p = tbs.v;
    u32 left = tbs.len;
    while (left >= 2) {
        der_t e;
        if (der_read(p, left, &e) != 0) return -1;
        if (e.tag == 0x30) {
            /* candidate SPKI: SEQUENCE { algorithm SEQUENCE { OID .. },
             *                  subjectPublicKey BIT STRING }
             * The first child is the ALGORITHM SEQUENCE (whose first
             * child is the OID, 0x06), the second child is the BIT
             * STRING (0x03). */
            der_t a, b;
            if (der_read(e.v, e.len, &a) == 0 && a.tag == 0x30) {
                der_t oid;
                if (der_read(a.v, a.len, &oid) == 0 && oid.tag == 0x06) {
                    u32 ql = e.len - (u32)(a.v + a.len - e.v);
                    if (der_read(a.v + a.len, ql, &b) == 0 && b.tag == 0x03) {
                        *bits = b.v;
                        *bitslen = b.len;
                        return 0;
                    }
                }
            }
        }
        u32 consumed = (u32)(e.v + e.len - p);
        p += consumed;
        left -= consumed;
    }
    return -1;
}

/* Parse the RSA key out of the SPKI BIT STRING content:
 * subjectPublicKey ::= BIT STRING { unused-bits(1) RSAPublicKey }
 * RSAPublicKey ::= SEQUENCE { modulus INTEGER, publicExponent INTEGER } */
int x509_rsa_key(const u8 *spki_bitstr, u32 bitstrlen,
                 u8 *modulus, u32 *modlen, u8 *exponent, u32 *explen) {
    if (bitstrlen < 1) return -1;
    const u8 *k = spki_bitstr + 1;              /* skip unused-bits */
    u32 klen = bitstrlen - 1;
    der_t seq;
    if (der_read(k, klen, &seq) != 0 || seq.tag != 0x30) return -1;
    der_t m;
    if (der_read(seq.v, seq.len, &m) != 0 || m.tag != 0x02) return -1;
    /* 2048-bit moduli are 257 bytes with a leading 0x00 (positive
     * INTEGER); strip it below.  Allow up to 257. */
    if (m.len == 0 || m.len > 257) return -1;
    u32 mlen = m.len;
    const u8 *mv = m.v;
    if (mlen > 1 && mv[0] == 0) { mv++; mlen--; }
    memcpy(modulus, mv, mlen);
    *modlen = mlen;
    der_t e;
    u32 ql = seq.len - (u32)(m.v + m.len - seq.v);
    if (der_read(m.v + m.len, ql, &e) != 0 || e.tag != 0x02) return -1;
    u32 elen = e.len;
    const u8 *ev = e.v;
    if (elen > 1 && ev[0] == 0) { ev++; elen--; }
    if (elen > 8) elen = 8;
    memcpy(exponent, ev, elen);
    *explen = elen;
    return 0;
}
