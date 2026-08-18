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

/* ----- drawing clip (Skift-style g.clip(r) / g.clearClip()) -----
 * A single global clip rectangle lives in gfx.c.  When enabled, every
 * pixel write is confined to it.  The compositor sets it to the current
 * dirty rectangle before repainting, so draw calls can never bleed outside
 * the damaged region.  Apps never enable it, so their drawing is
 * unaffected (the flag is 0-initialised per process). */
extern int g_clip_on;
extern int g_clip_x0, g_clip_y0, g_clip_x1, g_clip_y1;
void sf_set_clip(int x, int y, int w, int h);
void sf_clear_clip(void);
static inline int sf_clip_ok(int x, int y) {
    return !g_clip_on || (x >= g_clip_x0 && x < g_clip_x1 && y >= g_clip_y0 && y < g_clip_y1);
}

static inline void sf_putpx(surface_t *s, int x, int y, u32 c) {
    if ((u32)x < (u32)s->w && (u32)y < (u32)s->h && sf_clip_ok(x, y)) s->px[y * s->pitch + x] = c;
}
static inline u32 sf_getpx(surface_t *s, int x, int y) {
    if ((u32)x < (u32)s->w && (u32)y < (u32)s->h) return s->px[y * s->pitch + x];
    return 0xFF000000;
}

/* Put a pixel with alpha blending (c is premultiplied ARGB) */
static inline void sf_putpx_blend(surface_t *s, int x, int y, u32 c) {
    if ((u32)x >= (u32)s->w || (u32)y >= (u32)s->h || !sf_clip_ok(x, y)) return;
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
static inline void sf_blit_rect(surface_t *dst,int dx,int dy,int w,int h,surface_t*src,int sx,int sy){sf_blit(dst,dx,dy,src,sx,sy,w,h);}
/* alpha-blend src over dst (skips transparent pixels) */
void sf_blit_alpha(surface_t *dst, int dx, int dy, surface_t *src,
                   int sx, int sy, int w, int h);
/* Nearest-neighbour scaled blit (clip-aware). Used for live window
 * previews in the Alt+Tab switcher and Overview. */
void sf_blit_scaled(surface_t *dst, int dx, int dy, int dw, int dh,
                    surface_t *src, int sx, int sy, int sw, int sh);
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

/* ----- font (Inter Medium 12x18, AA, proportional — Skift's UI font) ----- */
#define FONT_W 12          /* cell width (max glyph width) */
#define FONT_H 18          /* line height                */
void sf_putc(surface_t *s, int x, int y, char ch, u32 fg);
void sf_text (surface_t *s, int x, int y, const char *txt, u32 fg);
int  sf_text_width(const char *txt);
/* Width of the first n characters of txt (for caret placement). */
int  sf_text_width_n(const char *txt, int n);
/* Advance width of a single character (proportional). */
int  sf_char_width(char ch);
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
/* Draw an icon at EXACTLY `px` pixels wide/tall regardless of its native
 * size (icons in the pack are 22/32/48px; this makes them consistent). */
static inline void sf_icon_sz(surface_t *s, int cx, int cy, icon_t ico, u32 tint, int px){
    if(!ico.px || ico.w<=0 || px<=0) return;
    sf_icon_scaled(s, cx, cy, ico, tint, px, ico.w);
}

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

/* ---- Dirty-rectangle damage tracking (Skift-style) ---- */
typedef struct { int x,y,w,h; } GfxRect;
static inline int rect_empty(GfxRect r){ return r.w<=0||r.h<=0; }
static inline GfxRect rect_clip(GfxRect r, GfxRect c){
    int x0=r.x>c.x?r.x:c.x, y0=r.y>c.y?r.y:c.y;
    int x1=(r.x+r.w)<(c.x+c.w)?(r.x+r.w):(c.x+c.w);
    int y1=(r.y+r.h)<(c.y+c.h)?(r.y+r.h):(c.y+c.h);
    GfxRect o={x0,y0,x1-x0,y1-y0}; return o;
}
static inline int rect_colide(GfxRect a,GfxRect b){
    return a.x<b.x+b.w && b.x<a.x+a.w && a.y<b.y+b.h && b.y<a.y+a.h;
}
static inline GfxRect rect_merge(GfxRect a,GfxRect b){
    int x0=a.x<b.x?a.x:b.x, y0=a.y<b.y?a.y:b.y;
    int x1=(a.x+a.w)>(b.x+b.w)?(a.x+a.w):(b.x+b.w);
    int y1=(a.y+a.h)>(b.y+b.h)?(a.y+a.h):(b.y+b.h);
    GfxRect o={x0,y0,x1-x0,y1-y0}; return o;
}

/* SIMD bit-exactness self-test (0 = SSE2 blend == scalar reference). */
int gfx_selftest(void);

/* HiDPI integer UI scale (1 or 2).  The compositor sets it; all text is
 * drawn with crisp integer pixel-doubling at scale 2. */
void sf_set_scale(int s);
int  sf_get_scale(void);
