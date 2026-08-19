/* Minimal baseline JPEG encoder.  See jpeg_enc.h. */
#include "jpeg_enc.h"

typedef unsigned int  u32;
typedef unsigned char u8;

#define ENC_MAX_DIM 1280
#define MAXBLK ((ENC_MAX_DIM/8)*(ENC_MAX_DIM/8))

/* ---- bit writer ---- */
typedef struct {
    unsigned char *buf;
    unsigned int cap, len;
    unsigned int bitbuf;
    int nbits;
} bw_t;

static void bw_init(bw_t *b, unsigned char *buf, unsigned int cap) {
    b->buf = buf; b->cap = cap; b->len = 0; b->bitbuf = 0; b->nbits = 0;
}
static int bw_byte(bw_t *b, unsigned char v) {
    if (b->len >= b->cap) return -1;
    b->buf[b->len++] = v;
    return 0;
}
static int bw_bits(bw_t *b, unsigned int v, int n) {
    b->bitbuf = (b->bitbuf << n) | (v & ((1u << n) - 1));
    b->nbits += n;
    while (b->nbits >= 8) {
        unsigned char byte = (unsigned char)(b->bitbuf >> (b->nbits - 8));
        b->nbits -= 8;
        b->bitbuf &= (1u << b->nbits) - 1;
        if (byte == 0xFF) { if (bw_byte(b, 0xFF) || bw_byte(b, 0x00)) return -1; }
        else if (bw_byte(b, byte)) return -1;
    }
    return 0;
}
static int bw_flush(bw_t *b) {
    if (b->nbits > 0) {
        /* pad with 1-bits */
        unsigned char byte = (unsigned char)((b->bitbuf << (8 - b->nbits)) | ((1u << (8 - b->nbits)) - 1));
        b->nbits = 0;
        if (byte == 0xFF) { if (bw_byte(b, 0xFF) || bw_byte(b, 0x00)) return -1; }
        else if (bw_byte(b, byte)) return -1;
    }
    return 0;
}

/* ---- 8x8 cosine table ---- */
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

/* forward DCT (input value-128) */
static void fdct(const float *in, float *out) {
    float tmp[64];
    for (int v = 0; v < 8; v++)
        for (int u = 0; u < 8; u++) {
            float s = 0.0f;
            for (int x = 0; x < 8; x++) s += in[v * 8 + x] * g_cos[u][x];
            tmp[v * 8 + u] = s * ((u == 0) ? 0.35355339059f : 0.5f);   /* (1/2)*C(u) */
        }
    for (int u = 0; u < 8; u++)
        for (int v = 0; v < 8; v++) {
            float s = 0.0f;
            for (int y = 0; y < 8; y++) s += tmp[y * 8 + u] * g_cos[v][y];
            out[v * 8 + u] = s * ((v == 0) ? 0.35355339059f : 0.5f);   /* (1/2)*C(v) */
        }
}

/* standard quant tables (zigzag order) */
static const u8 std_lum_q[64] = {
    16,11,10,16,24,40,51,61, 12,12,14,19,26,58,60,55,
    14,13,16,24,40,57,69,56, 14,17,22,29,51,87,80,62,
    18,22,37,56,68,109,103,77, 24,35,55,64,81,104,113,92,
    49,64,78,87,103,121,120,101, 72,92,95,98,112,100,103,99
};
static const u8 std_chr_q[64] = {
    17,18,24,47,99,99,99,99, 18,21,26,66,99,99,99,99,
    24,26,56,99,99,99,99,99, 47,66,99,99,99,99,99,99,
    99,99,99,99,99,99,99,99, 99,99,99,99,99,99,99,99,
    99,99,99,99,99,99,99,99, 99,99,99,99,99,99,99,99
};

static const u8 zz[64] = {
     0, 1, 8,16, 9, 2, 3,10,17,24,32,25,18,11, 4, 5,
    12,19,26,33,40,48,41,34,27,20,13, 6, 7,14,21,28,35,
    42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,58,59,
    52,45,38,31,39,46,53,60,61,54,47,55,62,63
};

/* ---- STANDARD JPEG Huffman tables (Annex K.3) ----
 * Hardcoded counts+symbols (extracted from a canonical reference).  These are
 * the tables every JPEG decoder accepts; using them avoids the subtle
 * strictness issues of freshly-computed optimal tables.  Symbols are already
 * in canonical order (by code length, then symbol value). */

static const u8 std_counts[4][16] = {
    /* DC luma */    {0,1,5,1,1,1,1,1,1,0,0,0,0,0,0,0},
    /* DC chroma */  {0,3,1,1,1,1,1,1,1,1,1,0,0,0,0,0},
    /* AC luma */    {0,2,1,3,3,2,4,3,5,5,4,4,0,0,1,125},
    /* AC chroma */  {0,2,1,2,4,4,3,4,7,5,4,4,0,1,2,119},
};

static const u8 std_dc_lum_sym[12]  = {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b};
static const u8 std_dc_chr_sym[12]  = {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b};
static const u8 std_ac_lum_sym[162] = {0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,0x21,0x31,0x41,0x06,0x13,0x51,0x61,0x07,0x22,0x71,0x14,0x32,0x81,0x91,0xa1,0x08,0x23,0x42,0xb1,0xc1,0x15,0x52,0xd1,0xf0,0x24,0x33,0x62,0x72,0x82,0x09,0x0a,0x16,0x17,0x18,0x19,0x1a,0x25,0x26,0x27,0x28,0x29,0x2a,0x34,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x83,0x84,0x85,0x86,0x87,0x88,0x89,0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xe1,0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9,0xfa};
static const u8 std_ac_chr_sym[162] = {0x00,0x01,0x02,0x03,0x11,0x04,0x05,0x21,0x31,0x06,0x12,0x41,0x51,0x07,0x61,0x71,0x13,0x22,0x32,0x81,0x08,0x14,0x42,0x91,0xa1,0xb1,0xc1,0x09,0x23,0x33,0x52,0xf0,0x15,0x62,0x72,0xd1,0x0a,0x16,0x24,0x34,0xe1,0x25,0xf1,0x17,0x18,0x19,0x1a,0x26,0x27,0x28,0x29,0x2a,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x82,0x83,0x84,0x85,0x86,0x87,0x88,0x89,0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9,0xfa};

/* pointer/len accessors: table index 0=DC luma, 1=DC chroma, 2=AC luma, 3=AC chroma */
static const u8 *std_sym(int ti, int *nsym) {
    switch (ti) {
    case 0: *nsym = 12;  return std_dc_lum_sym;
    case 1: *nsym = 12;  return std_dc_chr_sym;
    case 2: *nsym = 162; return std_ac_lum_sym;
    default: *nsym = 162; return std_ac_chr_sym;
    }
}

/* Build canonical code lengths + codes from a standard table's counts+symbols.
 * symbols are already in canonical (length, value) order, so codes are just
 * assigned in order with the standard "increment within a length, shift
 * between lengths" rule. */
static void std_make_codes(int ti, u8 *lens, unsigned short *codes) {
    for (int i = 0; i < 256; i++) { lens[i] = 0; codes[i] = 0; }
    int nsym; const u8 *sym = std_sym(ti, &nsym);
    unsigned short code = 0;
    int si = 0;
    for (int len = 1; len <= 16; len++) {
        int c = std_counts[ti][len-1];
        for (int i = 0; i < c && si < nsym; i++) {
            u8 s = sym[si++];
            lens[s] = (u8)len;
            codes[s] = code;
            code++;
        }
        code <<= 1;
    }
}

/* Emit a DHT segment for a standard table. */
static int emit_dht(bw_t *b, int ti) {
    int nsym; std_sym(ti, &nsym);
    int seglen = 2 + 1 + 16 + nsym;   /* Ls includes the 2 length bytes */
    if (bw_byte(b, 0xFF) || bw_byte(b, 0xC4)) return -1;
    if (bw_byte(b, (u8)(seglen >> 8)) || bw_byte(b, (u8)(seglen & 0xFF))) return -1;
    if (bw_byte(b, (u8)((ti >> 1) << 4 | (ti & 1)))) return -1;  /* tc/th */
    for (int i = 0; i < 16; i++) if (bw_byte(b, std_counts[ti][i])) return -1;
    const u8 *sym = std_sym(ti, &nsym);
    for (int i = 0; i < nsym; i++) if (bw_byte(b, sym[i])) return -1;
    return 0;
}

/* Encode one 8x8 block (q[] in zigzag order) as entropy-coded bits.
 * comp: 0=luma (Huffman table 0), 1/2=chroma (table 1).  prevdc[3] is the
 * per-component DC predictor (updated in place). */
static int enc_block(bw_t *bw, const int *q, int comp,
                     unsigned short c_dc[2][256], u8 l_dc[2][256],
                     unsigned short c_ac[2][256], u8 l_ac[2][256],
                     int prevdc[3]) {
    int ti = (comp == 0) ? 0 : 1;
    int pd = (comp == 0) ? 0 : comp;
    int diff = q[0] - prevdc[pd]; prevdc[pd] = q[0];
    int ad = diff < 0 ? -diff : diff, cat = 0; while (ad) { cat++; ad >>= 1; }
    if (bw_bits(bw, c_dc[ti][cat], l_dc[ti][cat])) return -1;
    if (cat) {
        unsigned int bits = (diff < 0) ? (unsigned int)(diff + ((1 << cat) - 1)) : (unsigned int)diff;
        if (bw_bits(bw, bits, cat)) return -1;
    }
    int run = 0;
    for (int i = 1; i < 64; i++) {
        if (q[i] == 0) { run++; continue; }
        while (run > 15) { if (bw_bits(bw, c_ac[ti][0xF0], l_ac[ti][0xF0])) return -1; run -= 16; }
        ad = q[i] < 0 ? -q[i] : q[i]; cat = 0; while (ad) { cat++; ad >>= 1; }
        if (bw_bits(bw, c_ac[ti][(run << 4) | cat], l_ac[ti][(run << 4) | cat])) return -1;
        unsigned int bits = (q[i] < 0) ? (unsigned int)(q[i] + ((1 << cat) - 1)) : (unsigned int)q[i];
        if (bw_bits(bw, bits, cat)) return -1;
        run = 0;
    }
    if (run > 0) if (bw_bits(bw, c_ac[ti][0x00], l_ac[ti][0x00])) return -1;
    return 0;
}

int jpeg_encode(const unsigned int *pixels, int w, int h, int pitch,
                int quality, unsigned char *out, unsigned int cap) {
    if (w <= 0 || h <= 0 || w > ENC_MAX_DIM || h > ENC_MAX_DIM) return -1;
    /* 4:2:0 chroma subsampling needs both dims to be multiples of 16 (the
     * chroma planes are w/2 x h/2 and must stay 8x8-block-aligned).  Real
     * screenshots/video are 1280x800 / 640x480 etc., all multiples of 16. */
    if ((w & 15) || (h & 15)) return -1;

    int scale = (quality >= 50) ? (200 - quality * 2) : (5000 / (quality ? quality : 1));
    if (scale < 1) scale = 1;
    u8 qlum[64], qchr[64];
    for (int i = 0; i < 64; i++) {
        int v = ((int)std_lum_q[i] * scale + 50) / 100; if (v < 1) v = 1; if (v > 255) v = 255; qlum[i] = (u8)v;
        v = ((int)std_chr_q[i] * scale + 50) / 100; if (v < 1) v = 1; if (v > 255) v = 255; qchr[i] = (u8)v;
    }

    /* static planes + quantized coefficients */
    static float Y[ENC_MAX_DIM * ENC_MAX_DIM];
    static float Cb[(ENC_MAX_DIM/2)*(ENC_MAX_DIM/2)];
    static float Cr[(ENC_MAX_DIM/2)*(ENC_MAX_DIM/2)];
    static int cY[MAXBLK * 64], cCb[(MAXBLK/4)*64], cCr[(MAXBLK/4)*64];

    /* RGB -> YCbCr (4:2:0) */
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            u32 p = pixels[y * pitch + x];
            int R = (int)((p >> 16) & 0xFF), G = (int)((p >> 8) & 0xFF), B = (int)(p & 0xFF);
            Y[y * w + x] = 0.299f*R + 0.587f*G + 0.114f*B;
            if ((y & 1) == 0 && (x & 1) == 0) {
                Cb[(y/2)*(w/2) + (x/2)] = -0.168736f*R - 0.331264f*G + 0.5f*B + 128.0f;
                Cr[(y/2)*(w/2) + (x/2)] = 0.5f*R - 0.418688f*G - 0.081312f*B + 128.0f;
            }
        }
    }
    int cw = w / 2, ch = h / 2;

    /* quantize + count frequencies (zigzag order storage) */
    int prevdc[3] = {0,0,0};
    /* quantize all blocks into zigzag-ordered coefficient arrays.
     * cY/cCb/cCr use a CONTIGUOUS block index (row-major over the block grid),
     * the SAME index the MCU-interleaved entropy loop reads them back with. */
    int yw = w / 8;                       /* luma block grid width   */
    int cbw = cw / 8;                     /* chroma block grid width */
    int blkidx = 0;
    for (int by = 0; by < h; by += 8) {
        for (int bx = 0; bx < w; bx += 8) {
            float in[64], dct[64];
            for (int j = 0; j < 8; j++)
                for (int i = 0; i < 8; i++)
                    in[j*8+i] = Y[(by+j)*w + (bx+i)] - 128.0f;
            fdct(in, dct);
            for (int i = 0; i < 64; i++) {
                int z = zz[i];
                float dv = dct[z] / (float)qlum[z];
                cY[blkidx*64 + i] = (int)(dv + (dv >= 0 ? 0.5f : -0.5f));
            }
            blkidx++;
        }
    }
    int cblk = 0;
    for (int by = 0; by < ch; by += 8) {
        for (int bx = 0; bx < cw; bx += 8) {
            for (int comp = 0; comp < 2; comp++) {
                float in[64], dct[64];
                float *plane = comp ? Cr : Cb;
                for (int j = 0; j < 8; j++)
                    for (int i = 0; i < 8; i++)
                        in[j*8+i] = plane[(by+j)*cw + (bx+i)] - 128.0f;
                fdct(in, dct);
                int *dst = comp ? cCr : cCb;
                for (int i = 0; i < 64; i++) {
                    int z = zz[i];
                    float dv = dct[z] / (float)qchr[z];
                    dst[cblk*64 + i] = (int)(dv + (dv >= 0 ? 0.5f : -0.5f));
                }
            }
            cblk++;   /* one per (Cb,Cr) pair — contiguous, matching entropy */
        }
    }

    /* DEBUG */

    bw_t bw; bw_init(&bw, out, cap);

    /* SOI */
    if (bw_byte(&bw, 0xFF) || bw_byte(&bw, 0xD8)) return -1;
    /* DQT */
    {
        int len = 67;   /* 2 (len) + Pq/Tq(1) + 64 */
        if (bw_byte(&bw, 0xFF) || bw_byte(&bw, 0xDB)) return -1;
        if (bw_byte(&bw, (u8)(len>>8)) || bw_byte(&bw, (u8)(len&0xFF))) return -1;
        if (bw_byte(&bw, 0x00)) return -1;
        for (int i = 0; i < 64; i++) if (bw_byte(&bw, qlum[zz[i]])) return -1;   /* write in zigzag order */
        if (bw_byte(&bw, 0xFF) || bw_byte(&bw, 0xDB)) return -1;
        if (bw_byte(&bw, (u8)(len>>8)) || bw_byte(&bw, (u8)(len&0xFF))) return -1;
        if (bw_byte(&bw, 0x01)) return -1;
        for (int i = 0; i < 64; i++) if (bw_byte(&bw, qchr[zz[i]])) return -1;   /* write in zigzag order */
    }
    /* SOF0 */
    {
        int len = 17;   /* 2 (len) + prec+h+w+Nf+3*3 */
        if (bw_byte(&bw, 0xFF) || bw_byte(&bw, 0xC0)) return -1;
        if (bw_byte(&bw, (u8)(len>>8)) || bw_byte(&bw, (u8)(len&0xFF))) return -1;
        if (bw_byte(&bw, 8)) return -1;
        if (bw_byte(&bw, (u8)(h>>8)) || bw_byte(&bw, (u8)(h&0xFF))) return -1;
        if (bw_byte(&bw, (u8)(w>>8)) || bw_byte(&bw, (u8)(w&0xFF))) return -1;
        if (bw_byte(&bw, 3)) return -1;
        if (bw_byte(&bw, 1) || bw_byte(&bw, 0x22) || bw_byte(&bw, 0)) return -1;
        if (bw_byte(&bw, 2) || bw_byte(&bw, 0x11) || bw_byte(&bw, 1)) return -1;
        if (bw_byte(&bw, 3) || bw_byte(&bw, 0x11) || bw_byte(&bw, 1)) return -1;
    }
    /* DHT */
    if (emit_dht(&bw, 0)) return -1;   /* DC luma   */
    if (emit_dht(&bw, 2)) return -1;   /* AC luma   */
    if (emit_dht(&bw, 1)) return -1;   /* DC chroma */
    if (emit_dht(&bw, 3)) return -1;   /* AC chroma */

    /* codes */
    u8 l_dc[2][256], l_ac[2][256];
    unsigned short c_dc[2][256], c_ac[2][256];
    std_make_codes(0, l_dc[0], c_dc[0]);
    std_make_codes(2, l_ac[0], c_ac[0]);
    std_make_codes(1, l_dc[1], c_dc[1]);
    std_make_codes(3, l_ac[1], c_ac[1]);

    /* SOS */
    if (bw_byte(&bw, 0xFF) || bw_byte(&bw, 0xDA)) return -1;
    if (bw_byte(&bw, 0) || bw_byte(&bw, 12)) return -1;   /* 2 (len) + Ns+3*2+3 = 12 */
    if (bw_byte(&bw, 3)) return -1;
    if (bw_byte(&bw, 1) || bw_byte(&bw, 0x00)) return -1;
    if (bw_byte(&bw, 2) || bw_byte(&bw, 0x11)) return -1;
    if (bw_byte(&bw, 3) || bw_byte(&bw, 0x11)) return -1;
    if (bw_byte(&bw, 0) || bw_byte(&bw, 63) || bw_byte(&bw, 0)) return -1;

    /* entropy — MCU-INTERLEAVED order (the JPEG scan order for 4:2:0):
     * each 16x16 MCU emits its 2x2 luma blocks, then its Cb, then its Cr.
     * Emitting all-luma-then-all-chroma (raster order) produces a bitstream
     * the decoder's MCU loop desynchronises on. */
    {
        prevdc[0] = prevdc[1] = prevdc[2] = 0;
        for (int my = 0; my < h / 16; my++) {
            for (int mx = 0; mx < w / 16; mx++) {
                /* 2x2 luma blocks of this MCU */
                for (int dy = 0; dy < 2; dy++)
                    for (int dx = 0; dx < 2; dx++) {
                        int blk = (2 * my + dy) * yw + (2 * mx + dx);
                        if (enc_block(&bw, &cY[blk * 64], 0, c_dc, l_dc, c_ac, l_ac, prevdc)) return -1;
                    }
                /* Cb then Cr */
                int cblk2 = my * cbw + mx;
                if (enc_block(&bw, &cCb[cblk2 * 64], 1, c_dc, l_dc, c_ac, l_ac, prevdc)) return -1;
                if (enc_block(&bw, &cCr[cblk2 * 64], 2, c_dc, l_dc, c_ac, l_ac, prevdc)) return -1;
            }
        }
    }
    if (bw_flush(&bw)) return -1;
    if (bw_byte(&bw, 0xFF) || bw_byte(&bw, 0xD9)) return -1;

    return (int)bw.len;
}
