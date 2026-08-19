/* Wallpaper pack loader (linked ONLY into the compositor /bin/init).
 *
 * The wallpaper pack (build/wallpaper.bin) is a 16 MB blob of raw BGRA
 * 1280x800 frames.  It used to live in gfx.c, which every app binary links,
 * so the same 16 MB was duplicated into all six userland ELFs (~100 MB of
 * ELF, ~109 MB ISO).  Only the compositor composites the wallpaper, so the
 * pixel accessors live here and this object + the blob are linked into
 * init.elf alone.  Apps that just need the wallpaper COUNT use the
 * WALLPAPER_COUNT constant in gfx.h (it matches the generator's output).
 */
#include "gfx.h"

extern const char _binary_wallpaper_bin_start[];
extern const char _binary_wallpaper_bin_end[];

static const u8  *G_wp_base;
static const u32 *G_wp_pixels;
static int G_wp_w, G_wp_h, G_wp_count, G_wp_index;

static void wp_init(void) {
    if (G_wp_base) return;
    G_wp_base = (const u8 *)_binary_wallpaper_bin_start;
    const u8 *b = G_wp_base;
    if (b[0] != 'Y' || b[1] != 'W' || b[2] != 'A' || b[3] != 'L' || b[4] != 'L') return;
    int ver = b[5];
    if (ver == 1) G_wp_count = 1;
    else if (ver == 2) G_wp_count = b[6] | (b[7] << 8);
    else G_wp_count = 0;
}

static const u8 *wallpaper_entry(int idx, int *w_out, int *h_out) {
    const u8 *b = G_wp_base;
    if (!b) return 0;
    int version = b[5];
    if (version == 1) {
        if (idx != 0) return 0;
        int w = b[6] | (b[7] << 8);
        int h = b[8] | (b[9] << 8);
        *w_out = w; *h_out = h;
        return b + 16;
    }
    if (version == 2) {
        int count = b[6] | (b[7] << 8);
        if (idx < 0 || idx >= count) return 0;
        const u32 *offs = (const u32 *)(b + 16);
        const u8 *e = b + offs[idx];
        int w = e[0] | (e[1] << 8);
        int h = e[2] | (e[3] << 8);
        *w_out = w; *h_out = h;
        return e + 8;
    }
    return 0;
}

int wallpaper_count(void) {
    wp_init();
    return G_wp_count;
}

int wallpaper_load_index(int idx) {
    wp_init();
    int w, h;
    const u8 *px = wallpaper_entry(idx, &w, &h);
    if (!px) return -1;
    G_wp_index = idx; G_wp_w = w; G_wp_h = h; G_wp_pixels = (const u32 *)px;
    return 0;
}

int wallpaper_load(surface_t *out) {
    if (wallpaper_load_index(0) < 0) return -1;
    if (out) { out->px = (u32 *)G_wp_pixels; out->w = G_wp_w; out->h = G_wp_h; out->pitch = G_wp_w; }
    return 0;
}

void wallpaper_bind(surface_t *s) {
    if (s) { s->px = (u32 *)G_wp_pixels; s->w = G_wp_w; s->h = G_wp_h; s->pitch = G_wp_w; }
}

int wallpaper_current_index(void) { return G_wp_index; }
int wallpaper_width(void)  { return G_wp_w; }
int wallpaper_height(void) { return G_wp_h; }
const u32 *wallpaper_pixels(void) { return G_wp_pixels; }

u32 wallpaper_px(int x, int y) {
    if ((u32)x >= (u32)G_wp_w || (u32)y >= (u32)G_wp_h) return 0xFF000000;
    const u8 *p = (const u8 *)G_wp_pixels + ((long)y * G_wp_w + x) * 4;
    return ARGB(p[3], p[0], p[1], p[2]);
}
