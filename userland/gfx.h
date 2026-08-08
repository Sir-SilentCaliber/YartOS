/* Yart OS - ring-3 software rendering toolkit.
 * Clean, minimal, designed for a modern grey/white desktop.
 */
#pragma once
#include "sys.h"

/* ----- surface ----- */
typedef struct {
    u32 *px;
    int  w, h, pitch;
} surface_t;

static inline void sf_putpx(surface_t *s, int x, int y, u32 c) {
    if ((u32)x < (u32)s->w && (u32)y < (u32)s->h) s->px[y * s->pitch + x] = c;
}
static inline u32 sf_getpx(surface_t *s, int x, int y) {
    if ((u32)x < (u32)s->w && (u32)y < (u32)s->h) return s->px[y * s->pitch + x];
    return 0xFF000000;
}

/* Put a pixel with alpha blending (c is premultiplied ARGB) */
static inline void sf_putpx_blend(surface_t *s, int x, int y, u32 c) {
    if ((u32)x >= (u32)s->w || (u32)y >= (u32)s->h) return;
    u32 sa = (c >> 24) & 0xFF;
    if (sa == 0) return;
    u32 *d = &s->px[y * s->pitch + x];
    if (sa == 255) { *d = c; return; }
    u32 sr = (c >> 16) & 0xFF, sg = (c >> 8) & 0xFF, sb = c & 0xFF;
    u32 dst = *d;
    u32 dr = (dst >> 16) & 0xFF, dg = (dst >> 8) & 0xFF, db = dst & 0xFF;
    u32 ia = 255 - sa;
    *d = 0xFF000000
       | (((sr*sa + dr*ia + 127)/255) << 16)
       | (((sg*sa + dg*ia + 127)/255) << 8)
       |  ((sb*sa + db*ia + 127)/255);
}

/* ----- colors ----- */
#define ARGB(a,r,g,b) ((u32)(((u32)(a)<<24)|((u32)(r)<<16)|((u32)(g)<<8)|(u32)(b)))
#define RGB(r,g,b)    ARGB(0xFF,(r),(g),(b))
#define A(c) (((c)>>24)&0xFF)
#define R(c) (((c)>>16)&0xFF)
#define G(c) (((c)>>8)&0xFF)
#define B(c) ((c)&0xFF)

/* copy rect from src->dst */
void sf_blit(surface_t *dst, int dx, int dy, surface_t *src, int sx, int sy, int w, int h);
/* alpha-blend src over dst (skips transparent pixels) */
void sf_blit_alpha(surface_t *dst, int dx, int dy, surface_t *src,
                   int sx, int sy, int w, int h);
/* In-place box blur (for frosted-glass dock). Radius is in pixels. */
void sf_blur_rect(surface_t *s, int x, int y, int w, int h, int radius, int passes);
void sf_fill(surface_t *s, u32 c);
void sf_fill_rect(surface_t *s, int x, int y, int w, int h, u32 c);
void sf_fill_rect_blend(surface_t *s, int x, int y, int w, int h, u32 c);
void sf_hline(surface_t *s, int x, int y, int w, u32 c);
void sf_vline(surface_t *s, int x, int y, int h, u32 c);
void sf_rect_outline(surface_t *s, int x, int y, int w, int h, u32 c);
void sf_round_rect(surface_t *s, int x, int y, int w, int h, int r, u32 c);
void sf_round_rect_blend(surface_t *s, int x, int y, int w, int h, int r, u32 c);
void sf_gradient_v(surface_t *s, u32 top, u32 bot);
void sf_clip(int *x, int *y, int *w, int *h, int maxw, int maxh);

/* ----- font (modern bold, DejaVu Sans Bold 10x18, AA) ----- */
#define FONT_W 10
#define FONT_H 18
void sf_putc(surface_t *s, int x, int y, char ch, u32 fg);
void sf_text (surface_t *s, int x, int y, const char *txt, u32 fg);
int  sf_text_width(const char *txt);
/* Text with blended alpha (fg has alpha) */
void sf_text_blend(surface_t *s, int x, int y, const char *txt, u32 fg);
void sf_putc_blend(surface_t *s, int x, int y, char ch, u32 fg);

/* tiny integer -> string (no alloc). If w>0, pad with leading zeros to width w. */
void itoa0(int v, char *b, int w);

/* ----- icons (Kora KDE pack) ----- */
typedef struct { const u32 *px; int w, h, pitch; } icon_t;
void assets_init(void);
icon_t icon_get(int id);
/* Draw icon top-left at (x,y) with tint (0 = native colour). Tint replaces RGB of
 * non-transparent pixels. */
void sf_icon_tl(surface_t *s, int x, int y, icon_t ico, u32 tint);
/* Draw icon centred at (cx,cy) */
void sf_icon(surface_t *s, int cx, int cy, icon_t ico, u32 tint);
/* Draw icon top-left with blended alpha (not just 1-bit) */
void sf_icon_tl_blend(surface_t *s, int x, int y, icon_t ico, u32 tint, u8 global_alpha);
/* Nearest-neighbour scaled draw, centered on (cx,cy); scale = scale_num/scale_den */
void sf_icon_scaled(surface_t *s, int cx, int cy, icon_t ico, u32 tint, int scale_num, int scale_den);

/* Wallpaper pack: wallpaper_count() returns how many are built in.
 * wallpaper_load_index() selects which one is active; wallpaper_load() is a
 * backwards-compatible alias that selects index 0.  wallpaper_bind() attaches
 * the currently-selected wallpaper to a surface_t (points into the blob). */
int  wallpaper_count(void);
int  wallpaper_load(surface_t *out);
int  wallpaper_load_index(int idx);
void wallpaper_bind(surface_t *s);
int  wallpaper_current_index(void);
int  wallpaper_width(void);
int  wallpaper_height(void);
const u32 *wallpaper_pixels(void);
u32  wallpaper_px(int x, int y);

/* Cache of a rendered wallpaper / surface (allocated framebuffer) */
surface_t sf_alloc(int w, int h);
void sf_free(surface_t *s);
