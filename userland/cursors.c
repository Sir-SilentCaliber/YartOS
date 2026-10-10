/* Yart userland - photo cursor themes (parsed from the embedded blob). */
#include "cursors.h"

extern char _binary_cursors_bin_start[];
extern char _binary_cursors_bin_end[];

static cursor_theme_t G_themes[CURSOR_THEME_COUNT];
static char G_names[CURSOR_THEME_COUNT][16];
static int G_count;

/* Blob pixels are R,G,B,A bytes; the renderer wants ARGB u32 words, so we
 * convert into static buffers at parse time. */
static u32 G_rgb[CURSOR_THEME_COUNT][CURSOR_KIND_COUNT][48 * 48];

int cursors_init(void) {
    const u8 *b = (const u8 *)_binary_cursors_bin_start;
    size_t size = (size_t)(_binary_cursors_bin_end - _binary_cursors_bin_start);
    if (size < 16) return 0;
    if (b[0] != 'Y' || b[1] != 'C' || b[2] != 'R' || b[3] != 'S') return 0;
    /* header: "YCRS" u16 version u16 n_themes u16 n_kinds u16 pad */
    unsigned n_themes = (unsigned)(b[6] | (b[7] << 8));
    unsigned n_kinds  = (unsigned)(b[8] | (b[9] << 8));
    if (n_themes == 0 || n_themes > CURSOR_THEME_COUNT) return 0;
    if (n_kinds != CURSOR_KIND_COUNT) return 0;

    G_count = (int)n_themes;
    for (unsigned t = 0; t < n_themes; t++) {
        for (unsigned k = 0; k < n_kinds; k++) {
            size_t e = 16 + ((size_t)t * n_kinds + k) * 28;
            if (e + 28 > size) return 0;
            char name[17];
            for (int i = 0; i < 16; i++) name[i] = (char)b[e + i];
            name[16] = 0;
            if (k == 0) {
                /* entry name is "<theme>-<kind>"; strip the kind suffix
                 * so the theme name matches the config value. Do this
                 * for EVERY theme (G_names would otherwise be NULL and
                 * crash cursors_theme_by_name). */
                int len = 0;
                while (name[len]) len++;
                while (len > 0 && name[len-1] != '-') len--;
                if (len > 0) { name[len-1] = 0; int k=0; while(name[k]){G_names[t][k]=name[k];k++;} G_names[t][k]=0; }
            }
            unsigned w  = (unsigned)(b[e+16] | (b[e+17] << 8));
            unsigned h  = (unsigned)(b[e+18] | (b[e+19] << 8));
            unsigned hx = (unsigned)(b[e+20] | (b[e+21] << 8));
            unsigned hy = (unsigned)(b[e+22] | (b[e+23] << 8));
            unsigned off = (unsigned)(b[e+24] | (b[e+25] << 8) |
                                      (b[e+26] << 16) | (b[e+27] << 24));
            cursor_img_t *im = &G_themes[t].img[k];
            im->present = false;
            if (w == 0 || h == 0 || w > 128 || h > 128) continue;
            size_t need = (size_t)w * h * 4;
            if (off + need > size) continue;
            if (w * h > 48 * 48) continue;         /* renderer buffer cap */
            im->w = (int)w; im->h = (int)h;
            im->hotx = (int)hx; im->hoty = (int)hy;
            /* convert RGBA bytes -> ARGB words */
            u32 *dst = G_rgb[t][k];
            const u8 *src = b + off;
            for (unsigned p = 0; p < (unsigned)(w * h); p++)
                dst[p] = ((u32)src[p*4+3] << 24) | ((u32)src[p*4] << 16) |
                         ((u32)src[p*4+1] << 8) |  (u32)src[p*4+2];
            im->px = dst;
            im->present = true;
        }
    }
    return G_count;
}

cursor_theme_t *cursors_theme(int t) {
    if (t < 0 || t >= G_count) return NULL;
    return &G_themes[t];
}
const char *cursors_theme_name(int t) {
    if (t < 0 || t >= G_count) return NULL;
    return G_names[t];
}
int cursors_theme_count(void) { return G_count; }

int cursors_theme_by_name(const char *name) {
    if (!name) return -1;
    for (int t = 0; t < G_count; t++) {
        const char *n = G_names[t];
        int i = 0;
        while (n[i] && name[i] && n[i] == name[i]) i++;
        if (n[i] == 0 && name[i] == 0) return t;
    }
    return -1;
}

/* ---- pre-scaled (draw-size) cursor cache ----
 * Scaled ONCE per (theme, kind), then blitted every frame with an integer
 * alpha blend.  Straight-ARGB output (A in 31:24, R 23:16, G 15:8, B 7:0). */
static u32  G_scaled[CURSOR_THEME_COUNT][CURSOR_KIND_COUNT][48 * 48];
static cursor_img_t G_scaled_img[CURSOR_THEME_COUNT][CURSOR_KIND_COUNT];
static bool G_scaled_ok[CURSOR_THEME_COUNT][CURSOR_KIND_COUNT];

/* Bilinear downscale of one cursor (premultiplied-alpha correct), producing a
 * straight-ARGB bitmap.  Runs once; the per-frame path is a plain blit. */
static void cursor_prescale(const cursor_img_t *im, u32 *dst, int dw, int dh) {
    const int SN = CURSOR_SCALE_NUM, SD = CURSOR_SCALE_DEN;
    for (int y = 0; y < dh; y++) {
        int sy = (y * SD * 256) / SN;
        int iy = sy >> 8, fy = sy & 255;
        int iy1 = iy + 1; if (iy1 >= im->h) iy1 = im->h - 1;
        for (int x = 0; x < dw; x++) {
            int sx = (x * SD * 256) / SN;
            int ix = sx >> 8, fx = sx & 255;
            int ix1 = ix + 1; if (ix1 >= im->w) ix1 = im->w - 1;
            u32 c00 = im->px[iy  * im->w + ix];
            u32 c10 = im->px[iy  * im->w + ix1];
            u32 c01 = im->px[iy1 * im->w + ix];
            u32 c11 = im->px[iy1 * im->w + ix1];
            int a00 = (int)(c00 >> 24), r00 = (int)(u8)(c00 >> 16) * a00, g00 = (int)(u8)(c00 >> 8) * a00, b00 = (int)(u8)c00 * a00;
            int a10 = (int)(c10 >> 24), r10 = (int)(u8)(c10 >> 16) * a10, g10 = (int)(u8)(c10 >> 8) * a10, b10 = (int)(u8)c10 * a10;
            int a01 = (int)(c01 >> 24), r01 = (int)(u8)(c01 >> 16) * a01, g01 = (int)(u8)(c01 >> 8) * a01, b01 = (int)(u8)c01 * a01;
            int a11 = (int)(c11 >> 24), r11 = (int)(u8)(c11 >> 16) * a11, g11 = (int)(u8)(c11 >> 8) * a11, b11 = (int)(u8)c11 * a11;
            int r0 = r00 + (((r10 - r00) * fx) >> 8);
            int g0 = g00 + (((g10 - g00) * fx) >> 8);
            int b0 = b00 + (((b10 - b00) * fx) >> 8);
            int al0 = a00 + (((a10 - a00) * fx) >> 8);
            int r1 = r01 + (((r11 - r01) * fx) >> 8);
            int g1 = g01 + (((g11 - g01) * fx) >> 8);
            int b1 = b01 + (((b11 - b01) * fx) >> 8);
            int al1 = a01 + (((a11 - a01) * fx) >> 8);
            int pr = r0 + (((r1 - r0) * fy) >> 8);
            int pg = g0 + (((g1 - g0) * fy) >> 8);
            int pb = b0 + (((b1 - b0) * fy) >> 8);
            int pa = al0 + (((al1 - al0) * fy) >> 8);
            u32 out = 0;
            if (pa > 0) {
                if (pa > 255) pa = 255;
                int R = (pr * 255 + pa / 2) / pa;
                int G = (pg * 255 + pa / 2) / pa;
                int B = (pb * 255 + pa / 2) / pa;
                if (R > 255) R = 255;
                if (G > 255) G = 255;
                if (B > 255) B = 255;
                out = ((u32)pa << 24) | ((u32)R << 16) | ((u32)G << 8) | (u32)B;
            }
            dst[y * dw + x] = out;
        }
    }
}

cursor_img_t *cursors_draw_img(int theme, int kind) {
    if (theme < 0 || theme >= G_count || kind < 0 || kind >= CURSOR_KIND_COUNT)
        return NULL;
    if (G_scaled_ok[theme][kind])
        return &G_scaled_img[theme][kind];
    cursor_img_t *im = &G_themes[theme].img[kind];
    if (!im->present) return NULL;
    int dw = (im->w * CURSOR_SCALE_NUM) / CURSOR_SCALE_DEN; if (dw < 1) dw = 1;
    int dh = (im->h * CURSOR_SCALE_NUM) / CURSOR_SCALE_DEN; if (dh < 1) dh = 1;
    if (dw * dh > 48 * 48) return NULL;
    cursor_prescale(im, G_scaled[theme][kind], dw, dh);
    cursor_img_t *out = &G_scaled_img[theme][kind];
    out->px = G_scaled[theme][kind];
    out->w = dw;
    out->h = dh;
    out->hotx = (im->hotx * CURSOR_SCALE_NUM) / CURSOR_SCALE_DEN;
    out->hoty = (im->hoty * CURSOR_SCALE_NUM) / CURSOR_SCALE_DEN;
    out->present = true;
    G_scaled_ok[theme][kind] = true;
    return out;
}
