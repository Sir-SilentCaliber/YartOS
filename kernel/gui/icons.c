/* Yart OS - icon API backed by real raster assets generated at build time.
 * The actual pixel data lives in asset_icons.c. */
#include <yart/icons.h>
#include <yart/gui.h>

typedef struct { const u32 *px; int w; int h; } icon_asset_t;
extern const icon_asset_t yart_icons[];
extern const int yart_icons_count;

const u32 *icon_pixels(icon_id_t id) {
    if ((int)id < 0 || (int)id >= yart_icons_count) return 0;
    return yart_icons[id].px;
}

/* Blend a single ARGB source pixel onto the backbuffer at (x,y). */
static inline void put_argb(int x, int y, u32 src) {
    u32 a = (src >> 24) & 0xFF;
    if (a == 0) return;
    if (a == 0xFF) { draw_pixel(x, y, src); return; }
    /* simple alpha blend over current backbuffer pixel */
    extern fb_ctx_t g_fb;
    if ((u32)x >= g_fb.width || (u32)y >= g_fb.height) return;
    u32 *dst = &g_fb.pixels[y * g_fb.pitch_px + x];
    u32 d = *dst;
    u32 sr = (src >> 16) & 0xFF, sg = (src >> 8) & 0xFF, sb = src & 0xFF;
    u32 dr = (d   >> 16) & 0xFF, dg = (d   >> 8) & 0xFF, db = d   & 0xFF;
    u32 ia = 255 - a;
    u32 nr = (sr * a + dr * ia) >> 8;
    u32 ng = (sg * a + dg * ia) >> 8;
    u32 nb = (sb * a + db * ia) >> 8;
    *dst = 0xFF000000U | (nr << 16) | (ng << 8) | nb;
}

void draw_icon(int x, int y, icon_id_t id) {
    if ((int)id < 0 || (int)id >= yart_icons_count) return;
    const icon_asset_t *a = &yart_icons[id];
    for (int j = 0; j < a->h; j++)
        for (int i = 0; i < a->w; i++)
            put_argb(x + i, y + j, a->px[j * a->w + i]);
}

void draw_icon_scaled(int x, int y, icon_id_t id, int scale) {
    if (scale <= 1) { draw_icon(x, y, id); return; }
    if ((int)id < 0 || (int)id >= yart_icons_count) return;
    const icon_asset_t *a = &yart_icons[id];
    for (int j = 0; j < a->h; j++)
        for (int i = 0; i < a->w; i++) {
            u32 c = a->px[j * a->w + i];
            if (!(c & 0xFF000000)) continue;
            for (int sy = 0; sy < scale; sy++)
                for (int sx = 0; sx < scale; sx++)
                    put_argb(x + i * scale + sx, y + j * scale + sy, c);
        }
}

/* Draw icon scaled to (size x size) using nearest-neighbor sampling. */
void draw_icon_sized(int x, int y, icon_id_t id, int size) {
    if ((int)id < 0 || (int)id >= yart_icons_count) return;
    const icon_asset_t *a = &yart_icons[id];
    if (size <= 0) return;
    for (int j = 0; j < size; j++) {
        int sy = j * a->h / size;
        for (int i = 0; i < size; i++) {
            int sx = i * a->w / size;
            put_argb(x + i, y + j, a->px[sy * a->w + sx]);
        }
    }
}
