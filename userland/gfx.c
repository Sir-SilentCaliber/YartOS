/* Yart OS - ring-3 software rendering toolkit (modern font edition).
 * SIMD note: on x86-64 SSE2 is baseline and the kernel saves/restores
 * FXSAVE state on every context switch, so the hot blend/copy paths below
 * use SSE2.  This is the Skift-style "fast software rasterizer": Skift's
 * karm-gfx composites on the CPU too - its smoothness comes from a tight
 * blitter + damage tracking, not a GPU.  gfx_selftest() proves the SIMD
 * paths are BIT-EXACT vs the scalar reference. */
#include "gfx.h"
#include "kora.h"
#include "font_modern.h"
/* Freestanding SSE2.  <emmintrin.h> drags in <stdlib.h> via mm_malloc.h,
 * which does not exist for a freestanding x86_64-elf cross-compiler.  GCC
 * exposes the same instructions as __builtin_ia32_* builtins (no headers
 * needed), and the vector types below are the exact shapes gcc's own
 * emmintrin.h defines, so this compiles with zero libc/system headers. */
typedef char  __v16qi __attribute__((__vector_size__(16)));
typedef short __v8hi  __attribute__((__vector_size__(16)));

static int font_ready = 1;
static void font_build(void){ font_ready=1; }

/* ---- HiDPI UI scale (integer 1x/2x) ----
 * The compositor sets this to the user's scale factor.  Text is drawn with
 * crisp integer pixel-doubling (like 2x bitmap fonts on a Retina display);
 * the window surfaces of 1x apps are upscaled 2x when composited (the same
 * "legacy app on a HiDPI screen" model macOS uses). */
static int g_ui_scale = 1;
void sf_set_scale(int s){ g_ui_scale = (s >= 2) ? 2 : 1; }
int  sf_get_scale(void){ return g_ui_scale; }

/* ---- SSE2 constant-alpha blend (bit-exact vs the scalar reference) ----
 * dst = src*sa + dst*(255-sa), rounded, alpha forced opaque.  Fits in 16-bit
 * because src*sa + dst*(255-sa) <= 255*255 = 65025; divide by 255 exactly
 * with q = (y + 1 + (y>>8)) >> 8. */
static void blend_row_scalar(u32 *p, int n, u8 sr, u8 sg, u8 sb, u8 sa) {
    u8 ia = 255 - sa;
    for (int i = 0; i < n; i++) {
        u32 d = p[i];
        p[i] = 0xFF000000
             | (((sr*sa + ((d>>16)&0xFF)*ia + 127)/255) << 16)
             | (((sg*sa + ((d>>8)&0xFF)*ia + 127)/255) << 8)
             |  ((sb*sa + (d&0xFF)*ia + 127)/255);
    }
}
static void blend_row_sse2(u32 *p, int n, u8 sr, u8 sg, u8 sb, u8 sa) {
    if (sa == 255) {
        u32 c = 0xFF000000 | ((u32)sr<<16) | ((u32)sg<<8) | sb;
        for (int i = 0; i < n; i++) p[i] = c;
        return;
    }
    u8 ia = 255 - sa;
    /* lanes: [b,g,r,a, b,g,r,a] over 4 pixels (little-endian unpack) */
    __v8hi cvec  = (__v8hi){ sb, sg, sr, 0, sb, sg, sr, 0 };
    __v8hi savec = (__v8hi){ sa, sa, sa, 0, sa, sa, sa, 0 };
    __v8hi iavec = (__v8hi){ ia, ia, ia, 0, ia, ia, ia, 0 };
    __v8hi c127  = (__v8hi){ 127,127,127,127,127,127,127,127 };
    __v8hi c1    = (__v8hi){ 1,1,1,1,1,1,1,1 };
    __v8hi a255  = (__v8hi){ 0, 0, 0, 255, 0, 0, 0, 255 };
    __v16qi zero = (__v16qi){ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 };
    int i = 0;
    for (; i + 3 < n; i += 4) {
        __v16qi d   = __builtin_ia32_loaddqu((const char *)(p + i));
        __v8hi  lo  = (__v8hi)__builtin_ia32_punpcklbw128(d, zero);
        __v8hi  hi  = (__v8hi)__builtin_ia32_punpckhbw128(d, zero);
        __v8hi  ylo = __builtin_ia32_paddw128(
                        __builtin_ia32_paddw128(
                            __builtin_ia32_pmullw128(cvec, savec),
                            __builtin_ia32_pmullw128(lo, iavec)), c127);
        __v8hi  yhi = __builtin_ia32_paddw128(
                        __builtin_ia32_paddw128(
                            __builtin_ia32_pmullw128(cvec, savec),
                            __builtin_ia32_pmullw128(hi, iavec)), c127);
        __v8hi  qlo = __builtin_ia32_psrlwi128(
                        __builtin_ia32_paddw128(
                          __builtin_ia32_paddw128(ylo, __builtin_ia32_psrlwi128(ylo, 8)), c1), 8);
        __v8hi  qhi = __builtin_ia32_psrlwi128(
                        __builtin_ia32_paddw128(
                          __builtin_ia32_paddw128(yhi, __builtin_ia32_psrlwi128(yhi, 8)), c1), 8);
        qlo = qlo | a255;
        qhi = qhi | a255;
        __builtin_ia32_storedqu((char *)(p + i),
                                (__v16qi)__builtin_ia32_packuswb128(qlo, qhi));
    }
    for (; i < n; i++) {
        u32 d = p[i];
        p[i] = 0xFF000000
             | (((sr*sa + ((d>>16)&0xFF)*ia + 127)/255) << 16)
             | (((sg*sa + ((d>>8)&0xFF)*ia + 127)/255) << 8)
             |  ((sb*sa + (d&0xFF)*ia + 127)/255);
    }
}

/* ----- drawing clip (Skift-style g.clip(r) model) ----- */
int g_clip_on = 0;
int g_clip_x0, g_clip_y0, g_clip_x1, g_clip_y1;

void sf_set_clip(int x, int y, int w, int h) {
    g_clip_on = 1;
    g_clip_x0 = x; g_clip_y0 = y;
    g_clip_x1 = x + w; g_clip_y1 = y + h;
}
void sf_clear_clip(void) { g_clip_on = 0; }

/* ----- primitives ----- */
void sf_fill(surface_t *s, u32 c) { sf_fill_rect(s, 0, 0, s->w, s->h, c); }

void sf_hline(surface_t *s, int x, int y, int w, u32 c) {
    if (!s || !s->px) return;
    if (y < 0 || y >= s->h) return;
    if (g_clip_on) {
        if (y < g_clip_y0 || y >= g_clip_y1) return;
        if (x < g_clip_x0) { w -= g_clip_x0 - x; x = g_clip_x0; }
        if (x + w > g_clip_x1) w = g_clip_x1 - x;
    }
    if (x < 0) { w += x; x = 0; }
    if (w <= 0) return;
    if (x + w > s->w) w = s->w - x;
    if (w <= 0) return;
    u32 *p = s->px + (long)y * s->pitch + x;
    u64 c64 = ((u64)c) | ((u64)c << 32);
    int i = 0;
    if (((unsigned long)p & 7) == 0) {
        for (; i+1 < w; i+=2) ((u64*)p)[i/2] = c64;
    }
    for (; i < w; i++) p[i] = c;
}
void sf_vline(surface_t *s, int x, int y, int h, u32 c) {
    if (!s || !s->px) return;
    if (x < 0 || x >= s->w) return;
    if (g_clip_on) {
        if (x < g_clip_x0 || x >= g_clip_x1) return;
        if (y < g_clip_y0) { h -= g_clip_y0 - y; y = g_clip_y0; }
        if (y + h > g_clip_y1) h = g_clip_y1 - y;
    }
    if (y < 0) { h += y; y = 0; }
    if (h <= 0) return;
    if (y + h > s->h) h = s->h - y;
    if (h <= 0) return;
    u32 *p = s->px + (long)y * s->pitch + x;
    for (int i = 0; i < h; i++) { *p = c; p += s->pitch; }
}
void sf_fill_rect(surface_t *s, int x, int y, int w, int h, u32 c) {
    if (w <= 0 || h <= 0) return;
    int x0=x,y0=y,x1=x+w,y1=y+h;
    if (x0<0) { x0=0; } if (y0<0) { y0=0; } if (x1>s->w) { x1=s->w; } if (y1>s->h) { y1=s->h; }
    for (int yy=y0; yy<y1; yy++) sf_hline(s, x0, yy, x1-x0, c);
}
void sf_fill_rect_blend(surface_t *s, int x, int y, int w, int h, u32 c) {
    if (w <= 0 || h <= 0) return;
    int x0=x,y0=y,x1=x+w,y1=y+h;
    if (g_clip_on) { if (x0<g_clip_x0) x0=g_clip_x0; if (y0<g_clip_y0) y0=g_clip_y0; if (x1>g_clip_x1) x1=g_clip_x1; if (y1>g_clip_y1) y1=g_clip_y1; }
    if (x0<0) { x0=0; } if (y0<0) { y0=0; } if (x1>s->w) { x1=s->w; } if (y1>s->h) { y1=s->h; }
    if (A(c) == 255) { sf_fill_rect(s,x0,y0,x1-x0,y1-y0,c); return; }
    if (A(c) == 0) return;
    u32 sr=R(c), sg=G(c), sb=B(c), sa=A(c);
    int wrow = x1 - x0;
    for (int yy=y0; yy<y1; yy++) {
        u32 *p = s->px + (long)yy*s->pitch + x0;
        blend_row_sse2(p, wrow, (u8)sr, (u8)sg, (u8)sb, (u8)sa);
    }
}
void sf_rect_outline(surface_t *s, int x, int y, int w, int h, u32 c) {
    sf_hline(s, x, y, w, c); sf_hline(s, x, y+h-1, w, c);
    sf_vline(s, x, y, h, c); sf_vline(s, x+w-1, y, h, c);
}
void sf_round_rect(surface_t *s, int x, int y, int w, int h, int r, u32 c) {
    if (w <= 0 || h <= 0) return;
    if (r <= 0) { sf_fill_rect(s, x, y, w, h, c); return; }
    if (r*2 > w) { r = w/2; } if (r*2 > h) { r = h/2; }
    sf_fill_rect(s, x+r, y, w-2*r, h, c);
    sf_fill_rect(s, x, y+r, r, h-2*r, c);
    sf_fill_rect(s, x+w-r, y+r, r, h-2*r, c);
    for (int dy = 0; dy < r; dy++) for (int dx = 0; dx < r; dx++) {
        int rr = dx*dx + dy*dy; int rc = (r-1)*(r-1);
        if (rr <= rc) {
            sf_putpx(s, x+r-1-dx, y+r-1-dy, c);
            sf_putpx(s, x+w-r+dx, y+r-1-dy, c);
            sf_putpx(s, x+r-1-dx, y+h-r+dy, c);
            sf_putpx(s, x+w-r+dx, y+h-r+dy, c);
        }
    }
}
void sf_round_rect_blend(surface_t *s, int x, int y, int w, int h, int r, u32 c) {
    if (w <= 0 || h <= 0) { return; } if (A(c) == 0) { return; }
    if (r <= 0) { sf_fill_rect_blend(s, x, y, w, h, c); return; }
    if (r*2 > w) { r = w/2; } if (r*2 > h) { r = h/2; }
    sf_fill_rect_blend(s, x+r, y, w-2*r, h, c);
    sf_fill_rect_blend(s, x, y+r, r, h-2*r, c);
    sf_fill_rect_blend(s, x+w-r, y+r, r, h-2*r, c);
    for (int dy = 0; dy < r; dy++) for (int dx = 0; dx < r; dx++) {
        int rr = dx*dx + dy*dy; int rc = (r-1)*(r-1);
        if (rr <= rc) {
            sf_putpx_blend(s, x+r-1-dx, y+r-1-dy, c);
            sf_putpx_blend(s, x+w-r+dx, y+r-1-dy, c);
            sf_putpx_blend(s, x+r-1-dx, y+h-r+dy, c);
            sf_putpx_blend(s, x+w-r+dx, y+h-r+dy, c);
        }
    }
}
void sf_gradient_v(surface_t *s, u32 top, u32 bot) {
    if (!s || !s->px || s->w <= 0 || s->h <= 0) return;
    int denom = s->h - 1; if (denom <= 0) { sf_fill_rect(s, 0, 0, s->w, s->h, top); return; }
    for (int y = 0; y < s->h; y++) {
        int t=y, nr=(int)R(top)+((int)R(bot)-(int)R(top))*t/denom;
        int ng=(int)G(top)+((int)G(bot)-(int)G(top))*t/denom;
        int nb=(int)B(top)+((int)B(bot)-(int)B(top))*t/denom;
        if(nr<0)nr=0;else if(nr>255)nr=255; if(ng<0)ng=0;else if(ng>255)ng=255; if(nb<0)nb=0;else if(nb>255)nb=255;
        u32 c = RGB(nr,ng,nb); sf_hline(s, 0, y, s->w, c);
    }
}
void sf_clip(int *x, int *y, int *w, int *h, int maxw, int maxh) {
    if (*x<0){*w+=*x;*x=0;} if (*y<0){*h+=*y;*y=0;} if (*x+*w>maxw)*w=maxw-*x; if (*y+*h>maxh)*h=maxh-*y; if (*w<0) *w=0; if (*h<0) *h=0;
}
void sf_blit(surface_t *dst, int dx, int dy, surface_t *src, int sx, int sy, int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (sx < 0) { dx -= sx; w += sx; sx = 0; } if (sy < 0) { dy -= sy; h += sy; sy = 0; }
    if (sx + w > src->w) { w = src->w - sx; } if (sy + h > src->h) { h = src->h - sy; }
    if (dx < 0) { sx -= dx; w += dx; dx = 0; } if (dy < 0) { sy -= dy; h += dy; dy = 0; }
    if (dx + w > dst->w) { w = dst->w - dx; } if (dy + h > dst->h) { h = dst->h - dy; }
    if (g_clip_on) {
        if (dx < g_clip_x0) { int d = g_clip_x0 - dx; dx += d; sx += d; w -= d; }
        if (dy < g_clip_y0) { int d = g_clip_y0 - dy; dy += d; sy += d; h -= d; }
        if (dx + w > g_clip_x1) w = g_clip_x1 - dx;
        if (dy + h > g_clip_y1) h = g_clip_y1 - dy;
    }
    if (w <= 0 || h <= 0) return;
    for (int j=0;j<h;j++) {
        const u32 *sp = src->px + (long)(sy+j)*src->pitch + sx;
        u32 *dp = dst->px + (long)(dy+j)*dst->pitch + dx;
        int i = 0;
        /* 128-bit SSE2 copy when both pointers are 16-byte aligned */
        if ((((unsigned long)sp)|((unsigned long)dp)) % 16 == 0) {
            for (; i+3 < w; i+=4)
                __builtin_ia32_storedqu((char *)(dp+i),
                    (__v16qi)__builtin_ia32_loaddqu((const char *)(sp+i)));
        }
        /* 64-bit copy when 8-byte aligned */
        if ((((unsigned long)sp)|((unsigned long)dp)) % 8 == 0) { for (; i+1 < w; i+=2) ((u64*)dp)[i/2] = ((const u64*)sp)[i/2]; }
        for (; i<w; i++) dp[i] = sp[i];
    }
}
void sf_blit_scaled(surface_t *dst, int dx, int dy, int dw, int dh,
                    surface_t *src, int sx, int sy, int sw, int sh) {
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0 || !src || !src->px) return;
    for (int j = 0; j < dh; j++) {
        int syy = sy + j * sh / dh; if (syy < 0) syy = 0; if (syy >= src->h) syy = src->h - 1;
        const u32 *sp = src->px + (long)syy * src->pitch;
        int dy2 = dy + j;
        if (dy2 < 0 || dy2 >= dst->h) continue;
        for (int i = 0; i < dw; i++) {
            int sxx = sx + i * sw / dw; if (sxx < 0) sxx = 0; if (sxx >= src->w) sxx = src->w - 1;
            sf_putpx(dst, dx + i, dy2, sp[sxx]);
        }
    }
}

void sf_blit_alpha(surface_t *dst, int dx, int dy, surface_t *src, int sx, int sy, int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (sx < 0) { dx -= sx; w += sx; sx = 0; } if (sy < 0) { dy -= sy; h += sy; sy = 0; }
    if (sx + w > src->w) { w = src->w - sx; } if (sy + h > src->h) { h = src->h - sy; }
    if (dx < 0) { sx -= dx; w += dx; dx = 0; } if (dy < 0) { sy -= dy; h += dy; dy = 0; }
    if (dx + w > dst->w) { w = dst->w - dx; } if (dy + h > dst->h) { h = dst->h - dy; }
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) {
        const u32 *sp = src->px + (long)(sy+j)*src->pitch + sx;
        for (int i = 0; i < w; i++) { u32 c = sp[i]; if (!A(c)) continue; sf_putpx_blend(dst, dx+i, dy+j, c); }
    }
}
void sf_blur_rect(surface_t *s, int x, int y, int w, int h, int radius, int passes) {
    if (radius < 1 || passes < 1 || w <= 0 || h <= 0) return;
    int x0=x,y0=y,x1=x+w,y1=y+h; if (x0<0) { x0=0; } if (y0<0) { y0=0; } if (x1>s->w) { x1=s->w; } if (y1>s->h) { y1=s->h; }
    static u32 sbuf[2048]; int sw = x1-x0; if (sw > (int)(sizeof(sbuf)/sizeof(sbuf[0]))) sw = (int)(sizeof(sbuf)/sizeof(sbuf[0]));
    for (int p=0; p<passes; p++) {
        for (int yy=y0; yy<y1; yy++) {
            const u32 *row = s->px + (long)yy*s->pitch;
            for (int xx=x0; xx<x1; xx++) {
                int sr=0,sg=0,sb=0,cnt=0; for (int k=-radius; k<=radius; k++) { int sx = xx+k; if (sx<x0) sx=x0; else if (sx>=x1) sx=x1-1; u32 c = row[sx]; sr += (c>>16)&0xFF; sg += (c>>8)&0xFF; sb += c&0xFF; cnt++; }
                sbuf[xx-x0] = 0xFF000000 | ((sr/cnt)<<16) | ((sg/cnt)<<8) | (sb/cnt);
            }
            u32 *dst = s->px + (long)yy*s->pitch + x0; for (int i=0;i<sw;i++) dst[i] = sbuf[i];
        }
        for (int xx=x0; xx<x1; xx++) {
            for (int yy=y0; yy<y1; yy++) {
                int sr=0,sg=0,sb=0,cnt=0; for (int k=-radius; k<=radius; k++) { int sy = yy+k; if (sy<y0) sy=y0; else if (sy>=y1) sy=y1-1; u32 c = s->px[(long)sy*s->pitch + xx]; sr += (c>>16)&0xFF; sg += (c>>8)&0xFF; sb += c&0xFF; cnt++; }
                sbuf[yy-y0] = 0xFF000000 | ((sr/cnt)<<16) | ((sg/cnt)<<8) | (sb/cnt);
            }
            for (int yy=y0; yy<y1; yy++) s->px[(long)yy*s->pitch + xx] = sbuf[yy-y0];
        }
    }
}

/* ----- modern AA font (DejaVu Sans Bold 10x18) ----- */
void sf_putc(surface_t *s, int x, int y, char ch, u32 fg) {
    unsigned char uch = (unsigned char)ch;
    if (uch < 0x20 || uch >= 0x7F) uch = '?';
    int idx = uch - 0x20;
    u8 fr = R(fg), fg_g = G(fg), fb = B(fg);
    int k = g_ui_scale;
    for (int row=0; row<MODERN_FONT_H; row++) {
        for (int col=0; col<MODERN_FONT_W; col++) {
            u8 cov = modern_font_aa[idx][row][col];
            if (cov==0) continue;
            u32 out;
            if (cov==255) out = RGB(fr, fg_g, fb);
            else {
                u32 bg = sf_getpx(s, x+col*k, y+row*k);
                u8 br = R(bg), bg_g = G(bg), bb = B(bg);
                u8 nr = (u8)((fr*cov + br*(255-cov) +127)/255);
                u8 ng = (u8)((fg_g*cov + bg_g*(255-cov) +127)/255);
                u8 nb = (u8)((fb*cov + bb*(255-cov) +127)/255);
                out = RGB(nr, ng, nb);
            }
            if (k == 1) {
                sf_putpx(s, x+col, y+row, out);
            } else {
                for (int dy=0; dy<k; dy++) for (int dx=0; dx<k; dx++)
                    sf_putpx(s, x + col*k + dx, y + row*k + dy, out);
            }
        }
    }
}
void sf_putc_blend(surface_t *s, int x, int y, char ch, u32 fg) {
    unsigned char uch = (unsigned char)ch;
    if (uch < 0x20 || uch >= 0x7F) uch = '?';
    int idx = uch - 0x20;
    u8 fa = A(fg);
    u8 fr = R(fg), fg_g = G(fg), fb = B(fg);
    if (fa==0) return;
    int k = g_ui_scale;
    for (int row=0; row<MODERN_FONT_H; row++) {
        for (int col=0; col<MODERN_FONT_W; col++) {
            u8 cov = modern_font_aa[idx][row][col];
            if (cov==0) continue;
            u32 eff_a = (u32)cov * fa / 255;
            if (eff_a==0) continue;
            u32 c = ARGB(eff_a, fr, fg_g, fb);
            if (k == 1) {
                sf_putpx_blend(s, x+col, y+row, c);
            } else {
                /* crisp 2x: fill a k x k block */
                for (int dy=0; dy<k; dy++) for (int dx=0; dx<k; dx++)
                    sf_putpx_blend(s, x + col*k + dx, y + row*k + dy, c);
            }
        }
    }
}
int sf_char_width(char ch) {
    unsigned char uch = (unsigned char)ch;
    if (uch < 0x20 || uch >= 0x7F) uch = '?';
    return modern_font_adv[uch - 0x20] * g_ui_scale;
}
void sf_text(surface_t *s, int x, int y, const char *t, u32 fg) {
    while (*t) { char c = *t++; sf_putc(s, x, y, c, fg); x += sf_char_width(c); }
}
void sf_text_blend(surface_t *s, int x, int y, const char *t, u32 fg) {
    while (*t) { char c = *t++; sf_putc_blend(s, x, y, c, fg); x += sf_char_width(c); }
}
int sf_text_width(const char *t) { int w=0; while (*t) w += sf_char_width(*t++); return w; }
int sf_text_width_n(const char *t, int n) { int w=0; while (n-- > 0 && *t) w += sf_char_width(*t++); return w; }

void itoa0(int v, char *b, int w) {
    char tmp[16]; int i = 0; int neg = 0;
    if (v < 0) { neg = 1; v = -v; } if (v == 0) tmp[i++] = '0';
    while (v) { tmp[i++] = '0' + v%10; v /= 10; }
    int k = 0; if (neg) b[k++] = '-';
    int digits = i; int pad = (w > digits + (neg?1:0)) ? w - digits - (neg?1:0) : 0;
    while (pad-- > 0) { b[k++] = '0'; } while (i) { b[k++] = tmp[--i]; } b[k] = 0;
}

/* ----- assets (Kora icon atlas; linked into every binary) -----
 * The wallpaper pack is the OTHER large asset but is compositor-only, so it
 * lives in userland/wallpaper.c and is linked into /bin/init alone. */
extern const char _binary_kora_bin_start[];
extern const char _binary_kora_bin_end[];
typedef struct { u32 name_off; u16 name_len; u32 px_off; u16 w, h, pitch; } __attribute__((packed)) ic_ent_t;
static struct { int count; const ic_ent_t *ent; const u8 *blob; } G_icons;
void assets_init(void) {
    const u8 *b = (const u8*)_binary_kora_bin_start; if (b[0]!='Y'||b[1]!='I'||b[2]!='C'||b[3]!='O'||b[4]!='N') return;
    u16 count = *(const u16*)(b+6); G_icons.count = count; asm volatile("" ::: "memory"); G_icons.ent = (const ic_ent_t*)(b+16); asm volatile("" ::: "memory"); G_icons.blob = b; asm volatile("" ::: "memory"); font_build();
}
icon_t icon_get(int id) { icon_t z = {0}; if (id < 0 || id >= G_icons.count) return z; const ic_ent_t *e = &G_icons.ent[id]; z.px = (const u32*)(G_icons.blob + e->px_off); z.w = e->w; z.h = e->h; z.pitch = e->pitch / 4; return z; }
void sf_icon_tl(surface_t *s, int x, int y, icon_t ico, u32 tint) {
    if (!ico.px || !ico.w || !ico.h) { return; } u32 tR = R(tint), tG = G(tint), tB = B(tint); int has_tint = (tint != 0);
    for (int j = 0; j < ico.h; j++) { int yy = y + j; if (yy < 0 || yy >= s->h) continue; const u32 *src = ico.px + (long)j*ico.pitch; u32 *dp = s->px + (long)yy*s->pitch + x; for (int i = 0; i < ico.w; i++) { int xx = x + i; if (xx < 0 || xx >= s->w || !sf_clip_ok(xx, yy)) continue; u32 sp = src[i]; u8 sr = (u8)sp; u8 sg = (u8)(sp>>8); u8 sb = (u8)(sp>>16); u8 sa = (u8)(sp>>24); if (!sa) continue; u32 c; if (has_tint) c = ARGB(sa, tR, tG, tB); else c = ARGB(sa, sr, sg, sb); int dx = i; if (sa == 255) dp[dx] = c; else { u32 d = dp[dx]; u32 ia = 255-sa; dp[dx] = 0xFF000000 | ((((c>>16)&0xFF)*sa + ((d>>16)&0xFF)*ia + 127)/255 << 16) | ((((c>>8)&0xFF)*sa  + ((d>>8)&0xFF)*ia + 127)/255 << 8) | (((c&0xFF)*sa     + (d&0xFF)*ia + 127)/255); } } }
}
void sf_icon_tl_blend(surface_t *s, int x, int y, icon_t ico, u32 tint, u8 global_alpha) {
    if (!ico.px || !ico.w || !ico.h) { return; } u32 tR = R(tint), tG = G(tint), tB = B(tint); int has_tint = (tint != 0);
    for (int j = 0; j < ico.h; j++) { const u32 *src = ico.px + (long)j*ico.pitch; for (int i = 0; i < ico.w; i++) { u32 sp = src[i]; u8 sa = (u8)((((u8)(sp>>24))*(u32)global_alpha + 127)/255); if (!sa) continue; u32 c; if (has_tint) c = ARGB(sa, tR, tG, tB); else c = ARGB(sa, (u8)sp, (u8)(sp>>8), (u8)(sp>>16)); sf_putpx_blend(s, x+i, y+j, c); } }
}
void sf_icon(surface_t *s, int cx, int cy, icon_t ico, u32 tint) { sf_icon_tl(s, cx - ico.w/2, cy - ico.h/2, ico, tint); }
/* Bilinear-filtered icon sample (premultiplied-alpha correct).
 *
 * Icon pixels are stored as R,G,B,A bytes, i.e. as a u32 word:
 *   R = bits 0..7,  G = bits 8..15,  B = bits 16..23,  A = bits 24..31.
 * We return a word in the SAME layout so callers can reuse the original
 * extraction (sr=(u8)sp, sg=(u8)(sp>>8), sb=(u8)(sp>>16), sa=(u8)(sp>>24)). */
static u32 icon_bilinear(const u32 *px, int pitch, int w, int h, float fx, float fy) {
    int x0 = (int)fx, y0 = (int)fy;
    float tx = fx - x0, ty = fy - y0;
    if (x0 < 0) { x0 = 0; tx = 0; }
    if (y0 < 0) { y0 = 0; ty = 0; }
    int x1 = x0 + 1; if (x1 >= w) x1 = w - 1;
    int y1 = y0 + 1; if (y1 >= h) y1 = h - 1;
    u32 c00 = px[y0*pitch + x0], c10 = px[y0*pitch + x1];
    u32 c01 = px[y1*pitch + x0], c11 = px[y1*pitch + x1];
    /* premultiply so colour fringes don't bleed out of transparent edges */
    float a00 = ((c00>>24)&255), r00 = (c00&255)*a00/255.f, g00 = ((c00>>8)&255)*a00/255.f, b00 = ((c00>>16)&255)*a00/255.f;
    float a10 = ((c10>>24)&255), r10 = (c10&255)*a10/255.f, g10 = ((c10>>8)&255)*a10/255.f, b10 = ((c10>>16)&255)*a10/255.f;
    float a01 = ((c01>>24)&255), r01 = (c01&255)*a01/255.f, g01 = ((c01>>8)&255)*a01/255.f, b01 = ((c01>>16)&255)*a01/255.f;
    float a11 = ((c11>>24)&255), r11 = (c11&255)*a11/255.f, g11 = ((c11>>8)&255)*a11/255.f, b11 = ((c11>>16)&255)*a11/255.f;
    float r0 = r00 + (r10-r00)*tx, r1 = r01 + (r11-r01)*tx;
    float g0 = g00 + (g10-g00)*tx, g1 = g01 + (g11-g01)*tx;
    float b0 = b00 + (b10-b00)*tx, b1 = b01 + (b11-b01)*tx;
    float a0 = a00 + (a10-a00)*tx, a1 = a01 + (a11-a01)*tx;
    float r = r0 + (r1-r0)*ty, g = g0 + (g1-g0)*ty, b = b0 + (b1-b0)*ty, a = a0 + (a1-a0)*ty;
    if (a <= 0.5f) return 0;
    /* unpremultiply: premultiplied channels were stored as c*a/255 */
    r = r * 255.0f / a;
    g = g * 255.0f / a;
    b = b * 255.0f / a;
    u8 ia = (u8)(a + 0.5f);
    u8 ir = (u8)(r + 0.5f), ig = (u8)(g + 0.5f), ib = (u8)(b + 0.5f);
    return ((u32)ia << 24) | ((u32)ir) | ((u32)ig << 8) | ((u32)ib << 16);
}

void sf_icon_scaled(surface_t *s, int cx, int cy, icon_t ico, u32 tint, int scale_num, int scale_den) {
    if (!ico.px || !ico.w || !ico.h) return;
    int dw = (ico.w*scale_num + scale_den/2)/scale_den;
    int dh = (ico.h*scale_num + scale_den/2)/scale_den;
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    int x0 = cx - dw/2, y0 = cy - dh/2;
    u32 tR=R(tint), tG=G(tint), tB=B(tint); int has_tint = (tint!=0);
    for (int j=0;j<dh;j++) {
        float fy = (j + 0.5f) * ico.h / dh - 0.5f;
        for (int i=0;i<dw;i++) {
            float fx = (i + 0.5f) * ico.w / dw - 0.5f;
            u32 sp = icon_bilinear(ico.px, ico.pitch, ico.w, ico.h, fx, fy);
            u8 sa = (u8)(sp>>24);
            if (!sa) continue;
            u32 c;
            if (has_tint) c = ARGB(sa, tR, tG, tB);
            else c = ARGB(sa, (u8)sp, (u8)(sp>>8), (u8)(sp>>16));
            if (sa >= 250) sf_putpx(s, x0+i, y0+j, c);
            else sf_putpx_blend(s, x0+i, y0+j, c);
        }
    }
}
surface_t sf_alloc(int w, int h) {
    surface_t s = {0}; size_t bytes = (size_t)w*h*4; size_t pagesz = 4096; size_t alloc = (bytes + pagesz-1) & ~(pagesz-1); void *mem = (void*)mmap((long)alloc); if (!mem) return s; s.px = (u32*)mem; s.w = w; s.h = h; s.pitch = w; return s;
}
void sf_free(surface_t *s) { if (s->px) munmap((long)s->px); s->px = 0; s->w = s->h = s->pitch = 0; }

/* ---- SIMD bit-exactness self-test ----
 * Runs the SSE2 blend path and the scalar reference on the same randomized
 * buffers and returns the number of differing pixels (0 = bit-exact). */
static u32 gfx_rng_state = 0x12345678;
static u32 gfx_rng(void) {
    gfx_rng_state ^= gfx_rng_state << 13;
    gfx_rng_state ^= gfx_rng_state >> 17;
    gfx_rng_state ^= gfx_rng_state << 5;
    return gfx_rng_state;
}
int gfx_selftest(void) {
    static u32 a[64], b[64];
    int bad = 0;
    for (int t = 0; t < 1000; t++) {
        for (int i = 0; i < 64; i++) { a[i] = b[i] = gfx_rng(); }
        u8 sr = (u8)(t*7+1), sg = (u8)(t*13+2), sb = (u8)(t*29+3), sa = (u8)((t*37+5)&0xFF);
        blend_row_sse2(a, 64, sr, sg, sb, sa);
        blend_row_scalar(b, 64, sr, sg, sb, sa);
        for (int i = 0; i < 64; i++) if (a[i] != b[i]) bad++;
    }
    return bad;
}
