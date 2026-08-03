/* Yart userland - photo cursor themes (parsed from the embedded blob). */
#include "cursors.h"

extern char _binary_cursors_bin_start[];
extern char _binary_cursors_bin_end[];

static cursor_theme_t G_themes[CURSOR_THEME_COUNT];
static const char *G_names[CURSOR_THEME_COUNT];
static int G_count;

/* Blob pixels are R,G,B,A bytes; the renderer wants ARGB u32 words, so we
 * convert into static buffers at parse time. */
static u32 G_rgb[CURSOR_THEME_COUNT][CURSOR_KIND_COUNT][32 * 32];

int cursors_init(void) {
    const u8 *b = (const u8 *)_binary_cursors_bin_start;
    size_t size = (size_t)(_binary_cursors_bin_end - _binary_cursors_bin_start);
    if (size < 16) return 0;
    if (b[0] != 'Y' || b[1] != 'C' || b[2] != 'R' || b[3] != 'S') return 0;
    unsigned n_themes = (unsigned)(b[4] | (b[5] << 8));
    unsigned n_kinds  = (unsigned)(b[6] | (b[7] << 8));
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
            if (t == 0 && k == 0) {
                /* entry name is "<theme>-<kind>"; strip the kind suffix
                 * so the theme name matches the config value */
                int len = 0;
                while (name[len]) len++;
                while (len > 0 && name[len-1] != '-') len--;
                if (len > 0) name[len-1] = 0;
                G_names[t] = name;
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
            if (w * h > 32 * 32) continue;         /* renderer buffer cap */
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
