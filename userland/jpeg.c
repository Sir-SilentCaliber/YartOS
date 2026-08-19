/* Minimal baseline JPEG decoder.  See jpeg.h.  Pure C, freestanding-safe:
 * no malloc, no libm.  Uses fixed-size static planes (JPEG_MAX_W/H) and a
 * cosine table computed once at first use. */
#include "jpeg.h"

#ifndef JPEG_MAX_W
#define JPEG_MAX_W 1280
#endif
#ifndef JPEG_MAX_H
#define JPEG_MAX_H 800
#endif

typedef unsigned int u32;

static void jmemset(void *d, int v, unsigned int n) {
    unsigned char *dd = (unsigned char *)d;
    while (n--) *dd++ = (unsigned char)v;
}

/* ---- byte stream ---- */
typedef struct {
    const unsigned char *d;
    unsigned int len, pos;
} bs_t;

static int bs_u8(bs_t *b)  { if (b->pos >= b->len) return -1; return b->d[b->pos++]; }
static int bs_u16(bs_t *b) { int hi = bs_u8(b); int lo = bs_u8(b); if (hi < 0 || lo < 0) return -1; return (hi << 8) | lo; }

/* skip to after the next 0xFF marker; returns marker byte or -1 */
static int skip_marker(bs_t *b) {
    for (;;) {
        int c = bs_u8(b);
        if (c < 0) return -1;
        if (c != 0xFF) continue;
        for (;;) {
            c = bs_u8(b);
            if (c < 0) return -1;
            if (c != 0xFF) break;
        }
        if (c == 0x00) continue;         /* stuffed byte */
        return c;
    }
}

/* ---- bit reader ---- */
typedef struct {
    const unsigned char *d;
    unsigned int len, pos;
    unsigned int bitbuf;
    int nbits;
} br_t;

static void br_init(br_t *b, const unsigned char *d, unsigned int len) {
    b->d = d; b->len = len; b->pos = 0; b->bitbuf = 0; b->nbits = 0;
}
static int br_bit(br_t *b) {
    if (b->nbits == 0) {
        if (b->pos >= b->len) return -1;
        unsigned int c = b->d[b->pos++];
        if (c == 0xFF) {
            if (b->pos < b->len && b->d[b->pos] == 0x00) b->pos++;
            else return -2;               /* marker */
        }
        b->bitbuf = (b->bitbuf << 8) | c;
        b->nbits = 8;
    }
    b->nbits--;
    return (int)((b->bitbuf >> b->nbits) & 1);
}
static int br_bits(br_t *b, int n) {
    int v = 0;
    while (n-- > 0) {
        int bit = br_bit(b);
        if (bit < 0) return -1;
        v = (v << 1) | bit;
    }
    return v;
}

/* ---- Huffman ---- */
#define MAX_HUFF 256
typedef struct {
    unsigned short codes[MAX_HUFF];
    unsigned char  sizes[MAX_HUFF];
    unsigned char  sym[MAX_HUFF];
    int n;
} huff_t;

static huff_t g_dc[2], g_ac[2];

static int build_huff(huff_t *t, const unsigned char *counts, const unsigned char *symbols) {
    unsigned short code = 0;
    int n = 0;
    for (int len = 1; len <= 16; len++) {
        int c = counts[len - 1];
        for (int i = 0; i < c; i++) {
            if (n >= MAX_HUFF) return -1;
            t->sizes[n] = (unsigned char)len;
            t->codes[n] = code;
            t->sym[n]  = symbols[n];
            n++;
            code++;
        }
        code <<= 1;
    }
    t->n = n;
    return n;
}
static int huff_decode(br_t *b, const huff_t *t) {
    unsigned int code = 0;
    for (int len = 1; len <= 16; len++) {
        int bit = br_bit(b);
        if (bit < 0) return -1;
        code = (code << 1) | (unsigned int)bit;
        for (int i = 0; i < t->n; i++)
            if (t->sizes[i] == len && t->codes[i] == (unsigned short)code)
                return t->sym[i];
    }
    return -1;
}

/* ---- quant tables + zigzag ---- */
static unsigned char g_qt[4][64];
static int g_qt_ok[4];

static const unsigned char zz[64] = {
     0, 1, 8,16, 9, 2, 3,10,17,24,32,25,18,11, 4, 5,
    12,19,26,33,40,48,41,34,27,20,13, 6, 7,14,21,28,35,
    42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,58,59,
    52,45,38,31,39,46,53,60,61,54,47,55,62,63
};
/* inverse zigzag: natural index -> zigzag index (quant tables are stored in
 * zigzag order, so dequant needs this inverse map) */
static const unsigned char izz[64] = {
     0, 1, 5, 6,14,15,27,28, 2, 4, 7,13,16,26,29,42,
     3, 8,12,17,25,30,41,43, 9,11,18,24,31,40,44,53,
    10,19,23,32,39,45,52,54,20,22,33,38,46,51,55,60,
    21,34,37,47,50,56,59,61,35,36,48,49,57,58,62,63
};

/* ---- IDCT cosine table (hardcoded, double-precision source) ----
 * g_cos[u][x] = cos((2x+1)*u*pi/16).  A runtime Taylor series was WRONG for
 * arguments up to ~20 rad (it only converges near 0), which silently
 * corrupted high-frequency coefficients - visible only on small images
 * (whose 8x8 blocks span a large value range).  Hardcoded = exact. */
static const float g_cos[8][8] = {
  {1.000000000f, 1.000000000f, 1.000000000f, 1.000000000f, 1.000000000f, 1.000000000f, 1.000000000f, 1.000000000f},
  {0.980785280f, 0.831469612f, 0.555570233f, 0.195090322f, -0.195090322f, -0.555570233f, -0.831469612f, -0.980785280f},
  {0.923879533f, 0.382683432f, -0.382683432f, -0.923879533f, -0.923879533f, -0.382683432f, 0.382683432f, 0.923879533f},
  {0.831469612f, -0.195090322f, -0.980785280f, -0.555570233f, 0.555570233f, 0.980785280f, 0.195090322f, -0.831469612f},
  {0.707106781f, -0.707106781f, -0.707106781f, 0.707106781f, 0.707106781f, -0.707106781f, -0.707106781f, 0.707106781f},
  {0.555570233f, -0.980785280f, 0.195090322f, 0.831469612f, -0.831469612f, -0.195090322f, 0.980785280f, -0.555570233f},
  {0.382683432f, -0.923879533f, 0.923879533f, -0.382683432f, -0.382683432f, 0.923879533f, -0.923879533f, 0.382683432f},
  {0.195090322f, -0.555570233f, 0.831469612f, -0.980785280f, 0.980785280f, -0.831469612f, 0.555570233f, -0.195090322f}
};

/* separable IDCT: in[8x8] (natural order, dequantized) -> out[8x8] */
static void idct(const float *in, float *out) {
    float tmp[64];
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            float s = 0.0f;
            for (int u = 0; u < 8; u++) {
                float cu = (u == 0) ? 0.70710678118f : 1.0f;
                s += cu * in[y * 8 + u] * g_cos[u][x];
            }
            tmp[y * 8 + x] = s * 0.5f;
        }
    }
    for (int x = 0; x < 8; x++) {
        for (int y = 0; y < 8; y++) {
            float s = 0.0f;
            for (int v = 0; v < 8; v++) {
                float cv = (v == 0) ? 0.70710678118f : 1.0f;
                s += cv * tmp[v * 8 + x] * g_cos[v][y];
            }
            out[y * 8 + x] = s * 0.5f;
        }
    }
}

/* ---- per-frame state ---- */
static int g_ncomp;
static int g_id[3], g_h[3], g_v[3], g_tq[3];
static int g_fw, g_fh;
static int g_hmax, g_vmax;
static int g_pw[3], g_ph[3];
static float g_plane[3][JPEG_MAX_W * JPEG_MAX_H];

int jpeg_decode(const unsigned char *jpeg, unsigned int len,
                unsigned int *out, unsigned int pitch,
                int max_w, int max_h,
                int *w_out, int *h_out) {
    bs_t bs; bs.d = jpeg; bs.len = len; bs.pos = 0;

    for (int i = 0; i < 4; i++) g_qt_ok[i] = 0;
    g_ncomp = 0; g_fw = g_fh = 0;

    if (bs_u8(&bs) != 0xFF || bs_u8(&bs) != 0xD8) return -1;

    int decoded = 0;

    for (;;) {
        int marker = skip_marker(&bs);
        if (marker < 0) return -1;
        if (marker == 0xD9) break;                    /* EOI */

        if (marker == 0xDB) {                          /* DQT */
            int l = bs_u16(&bs); if (l < 2) return -1;
            int end = (int)bs.pos + l - 2;
            while (bs.pos < (unsigned int)end && bs.pos < bs.len) {
                int pq = bs_u8(&bs); if (pq < 0) return -1;
                int tq = pq & 0x0F, prec = pq >> 4;
                if (tq > 3) return -1;
                for (int i = 0; i < 64; i++) {
                    int v;
                    if (prec) { int hi = bs_u8(&bs); if (hi < 0) return -1; v = (hi << 8) | bs_u8(&bs); }
                    else v = bs_u8(&bs);
                    if (v < 0) return -1;
                    g_qt[tq][i] = (unsigned char)v;
                }
                g_qt_ok[tq] = 1;
            }
        } else if (marker == 0xC4) {                   /* DHT */
            int l = bs_u16(&bs); if (l < 2) return -1;
            int end = (int)bs.pos + l - 2;
            while (bs.pos < (unsigned int)end && bs.pos < bs.len) {
                int tc = bs_u8(&bs); if (tc < 0) return -1;
                int th = tc & 0x0F, tclass = tc >> 4;
                if (th > 1 || tclass > 1) return -1;
                unsigned char counts[16];
                int total = 0;
                for (int i = 0; i < 16; i++) { int c = bs_u8(&bs); if (c < 0) return -1; counts[i] = (unsigned char)c; total += c; }
                if (total < 1 || total > MAX_HUFF) return -1;
                unsigned char symbols[MAX_HUFF];
                for (int i = 0; i < total; i++) { int s = bs_u8(&bs); if (s < 0) return -1; symbols[i] = (unsigned char)s; }
                if (tclass == 0) build_huff(&g_dc[th], counts, symbols);
                else             build_huff(&g_ac[th], counts, symbols);
            }
        } else if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2) {  /* SOF */
            int l = bs_u16(&bs); if (l < 2) return -1;
            int prec = bs_u8(&bs);
            g_fh = bs_u16(&bs); g_fw = bs_u16(&bs);
            int nf = bs_u8(&bs);
            if (prec != 8 || g_fh <= 0 || g_fw <= 0 || nf < 1 || nf > 3) return -1;
            if (g_fw > JPEG_MAX_W || g_fh > JPEG_MAX_H) return -1;
            if (max_w > 0 && g_fw > max_w) return -1;
            if (max_h > 0 && g_fh > max_h) return -1;
            g_ncomp = nf;
            g_hmax = g_vmax = 1;
            for (int i = 0; i < nf; i++) {
                int cid = bs_u8(&bs), sf = bs_u8(&bs), tq = bs_u8(&bs);
                if (cid < 0 || sf < 0 || tq < 0) return -1;
                g_id[i] = cid;
                g_h[i] = sf >> 4; g_v[i] = sf & 0x0F;
                g_tq[i] = tq;
                if (g_h[i] < 1 || g_h[i] > 4 || g_v[i] < 1 || g_v[i] > 4) return -1;
                if (g_h[i] > g_hmax) g_hmax = g_h[i];
                if (g_v[i] > g_vmax) g_vmax = g_v[i];
            }
        } else if (marker == 0xDA) {                   /* SOS */
            if (g_ncomp == 0) return -1;
            int l = bs_u16(&bs); if (l < 2) return -1;
            int ns = bs_u8(&bs);
            if (ns != g_ncomp) return -1;
            for (int i = 0; i < ns; i++) {
                bs_u8(&bs); bs_u8(&bs);   /* selector + table ids */
            }
            if (bs_u8(&bs) != 0 || bs_u8(&bs) != 63 || bs_u8(&bs) != 0) return -1;

            /* probe mode: caller only wants dimensions */
            if (out == 0) { *w_out = g_fw; *h_out = g_fh; return 0; }

            for (int c = 0; c < g_ncomp; c++) {
                g_pw[c] = (g_fw * g_h[c] + g_hmax - 1) / g_hmax;
                g_ph[c] = (g_fh * g_v[c] + g_vmax - 1) / g_vmax;
                jmemset(g_plane[c], 0, (unsigned int)g_pw[c] * g_ph[c] * sizeof(float));
            }

            /* entropy data begins right here (after the SOS header) */
            br_t br; br_init(&br, bs.d + bs.pos, bs.len - bs.pos);
            int ldc[3] = {0,0,0};
            int mcus_x = (g_fw + g_hmax * 8 - 1) / (g_hmax * 8);
            int mcus_y = (g_fh + g_vmax * 8 - 1) / (g_vmax * 8);

            for (int my = 0; my < mcus_y; my++) {
                for (int mx = 0; mx < mcus_x; mx++) {
                    for (int c = 0; c < g_ncomp; c++) {
                        for (int vy = 0; vy < g_v[c]; vy++) {
                            for (int hx = 0; hx < g_h[c]; hx++) {
                                int tabidx = (g_id[c] == 1) ? 0 : 1;
                                /* DC */
                                int dcsym = huff_decode(&br, &g_dc[tabidx]);
                                if (dcsym < 0) return -1;
                                int diff;
                                if (dcsym == 0) diff = 0;
                                else {
                                    int bits = br_bits(&br, dcsym);
                                    if (bits < 0) return -1;
                                    if (bits < (1 << (dcsym - 1))) bits -= (1 << dcsym) - 1;
                                    diff = bits;
                                }
                                ldc[c] += diff;
                                int coeff[64]; jmemset(coeff, 0, sizeof(coeff));
                                coeff[0] = ldc[c];
                                /* AC */
                                int k = 1;
                                while (k < 64) {
                                    int rs = huff_decode(&br, &g_ac[tabidx]);
                                    if (rs < 0) return -1;
                                    int r = rs >> 4, s = rs & 0x0F;
                                    if (s == 0) { if (r == 15) { k += 16; continue; } break; }
                                    if (k + r > 63) return -1;
                                    k += r;
                                    int bits = br_bits(&br, s);
                                    if (bits < 0) return -1;
                                    if (bits < (1 << (s - 1))) bits -= (1 << s) - 1;
                                    coeff[zz[k]] = bits;
                                    k++;
                                }
                                /* dequant + IDCT (quant table is zigzag order) */
                                float dq[64];
                                for (int i = 0; i < 64; i++) dq[i] = (float)(coeff[i] * (int)g_qt[g_tq[c]][izz[i]]);
                                float blk[64];
                                idct(dq, blk);
                                int bx = mx * g_h[c] + hx, by = my * g_v[c] + vy;
                                for (int y = 0; y < 8; y++) {
                                    int py = by * 8 + y;
                                    if (py >= g_ph[c]) continue;
                                    for (int x = 0; x < 8; x++) {
                                        int px = bx * 8 + x;
                                        if (px >= g_pw[c]) continue;
                                        g_plane[c][py * g_pw[c] + px] = blk[y * 8 + x];
                                    }
                                }
                            }
                        }
                    }
                }
            }
            decoded = 1;
        } else if (marker >= 0xE0 && marker <= 0xEF) {  /* APPn */
            int l = bs_u16(&bs); if (l < 2) return -1;
            bs.pos += (unsigned int)(l - 2);
            if (bs.pos > bs.len) return -1;
        } else if (marker == 0xFE) {                     /* COM */
            int l = bs_u16(&bs); if (l < 2) return -1;
            bs.pos += (unsigned int)(l - 2);
            if (bs.pos > bs.len) return -1;
        } else if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            continue;                                    /* restart markers */
        } else {
            return -1;
        }
    }

    if (!decoded) return -1;

    /* render to ARGB */
    for (int y = 0; y < g_fh; y++) {
        for (int x = 0; x < g_fw; x++) {
            unsigned char R, G, B;
            if (g_ncomp == 1) {
                int v = (int)g_plane[0][y * g_pw[0] + x] + 128;
                if (v < 0) v = 0;
                if (v > 255) v = 255;
                R = G = B = (unsigned char)v;
            } else {
                int c1x = x * g_h[1] / g_hmax, c1y = y * g_v[1] / g_vmax;
                int c2x = x * g_h[2] / g_hmax, c2y = y * g_v[2] / g_vmax;
                /* planes hold (value-128): Y-128, Cb-128, Cr-128 */
                float Y  = g_plane[0][y * g_pw[0] + x] + 128.0f;
                float Cb = g_plane[1][c1y * g_pw[1] + c1x];
                float Cr = g_plane[2][c2y * g_pw[2] + c2x];
                float r = Y + 1.402f * Cr;
                float gg = Y - 0.344136f * Cb - 0.714136f * Cr;
                float b = Y + 1.772f * Cb;
                int ri = (int)r, gi = (int)gg, bi = (int)b;
                if (ri < 0) ri = 0;
                if (ri > 255) ri = 255;
                if (gi < 0) gi = 0;
                if (gi > 255) gi = 255;
                if (bi < 0) bi = 0;
                if (bi > 255) bi = 255;
                R = (unsigned char)ri; G = (unsigned char)gi; B = (unsigned char)bi;
            }
            out[y * pitch + x] = 0xFF000000u | ((u32)R << 16) | ((u32)G << 8) | (u32)B;
        }
    }

    *w_out = g_fw; *h_out = g_fh;
    return 0;
}
