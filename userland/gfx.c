/* Yart OS - ring-3 software rendering toolkit (modern font edition) */
#include "gfx.h"
#include "kora.h"
#include "font_modern.h"

static int font_ready = 1;
static void font_build(void){ font_ready=1; }

/* ----- primitives ----- */
void sf_fill(surface_t *s, u32 c) { sf_fill_rect(s, 0, 0, s->w, s->h, c); }

void sf_hline(surface_t *s, int x, int y, int w, u32 c) {
    if (!s || !s->px) return;
    if (y < 0 || y >= s->h) return;
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
    if (x0<0) x0=0; if (y0<0) y0=0; if (x1>s->w) x1=s->w; if (y1>s->h) y1=s->h;
    for (int yy=y0; yy<y1; yy++) sf_hline(s, x0, yy, x1-x0, c);
}
void sf_fill_rect_blend(surface_t *s, int x, int y, int w, int h, u32 c) {
    if (w <= 0 || h <= 0) return;
    int x0=x,y0=y,x1=x+w,y1=y+h;
    if (x0<0) x0=0; if (y0<0) y0=0; if (x1>s->w) x1=s->w; if (y1>s->h) y1=s->h;
    if (A(c) == 255) { sf_fill_rect(s,x0,y0,x1-x0,y1-y0,c); return; }
    if (A(c) == 0) return;
    u32 sr=R(c), sg=G(c), sb=B(c), sa=A(c), ia=255-sa;
    for (int yy=y0; yy<y1; yy++) {
        u32 *p = s->px + (long)yy*s->pitch + x0;
        for (int xx=x0; xx<x1; xx++) {
            u32 d = *p;
            *p++ = 0xFF000000
                 | (((sr*sa + ((d>>16)&0xFF)*ia + 127)/255) << 16)
                 | (((sg*sa + ((d>>8)&0xFF)*ia + 127)/255) << 8)
                 |  ((sb*sa + (d&0xFF)*ia + 127)/255);
        }
    }
}
void sf_rect_outline(surface_t *s, int x, int y, int w, int h, u32 c) {
    sf_hline(s, x, y, w, c); sf_hline(s, x, y+h-1, w, c);
    sf_vline(s, x, y, h, c); sf_vline(s, x+w-1, y, h, c);
}
void sf_round_rect(surface_t *s, int x, int y, int w, int h, int r, u32 c) {
    if (w <= 0 || h <= 0) return;
    if (r <= 0) { sf_fill_rect(s, x, y, w, h, c); return; }
    if (r*2 > w) r = w/2; if (r*2 > h) r = h/2;
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
    if (w <= 0 || h <= 0) return; if (A(c) == 0) return;
    if (r <= 0) { sf_fill_rect_blend(s, x, y, w, h, c); return; }
    if (r*2 > w) r = w/2; if (r*2 > h) r = h/2;
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
    if (sx + w > src->w) w = src->w - sx; if (sy + h > src->h) h = src->h - sy;
    if (dx < 0) { sx -= dx; w += dx; dx = 0; } if (dy < 0) { sy -= dy; h += dy; dy = 0; }
    if (dx + w > dst->w) w = dst->w - dx; if (dy + h > dst->h) h = dst->h - dy;
    if (w <= 0 || h <= 0) return;
    for (int j=0;j<h;j++) {
        const u32 *sp = src->px + (long)(sy+j)*src->pitch + sx;
        u32 *dp = dst->px + (long)(dy+j)*dst->pitch + dx;
        int i = 0; if ((((unsigned long)sp)|((unsigned long)dp)) % 8 == 0) { for (; i+1 < w; i+=2) ((u64*)dp)[i/2] = ((const u64*)sp)[i/2]; }
        for (; i<w; i++) dp[i] = sp[i];
    }
}
void sf_blit_alpha(surface_t *dst, int dx, int dy, surface_t *src, int sx, int sy, int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (sx < 0) { dx -= sx; w += sx; sx = 0; } if (sy < 0) { dy -= sy; h += sy; sy = 0; }
    if (sx + w > src->w) w = src->w - sx; if (sy + h > src->h) h = src->h - sy;
    if (dx < 0) { sx -= dx; w += dx; dx = 0; } if (dy < 0) { sy -= dy; h += dy; dy = 0; }
    if (dx + w > dst->w) w = dst->w - dx; if (dy + h > dst->h) h = dst->h - dy;
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) {
        const u32 *sp = src->px + (long)(sy+j)*src->pitch + sx;
        for (int i = 0; i < w; i++) { u32 c = sp[i]; if (!A(c)) continue; sf_putpx_blend(dst, dx+i, dy+j, c); }
    }
}
void sf_blur_rect(surface_t *s, int x, int y, int w, int h, int radius, int passes) {
    if (radius < 1 || passes < 1 || w <= 0 || h <= 0) return;
    int x0=x,y0=y,x1=x+w,y1=y+h; if (x0<0) x0=0; if (y0<0) y0=0; if (x1>s->w) x1=s->w; if (y1>s->h) y1=s->h;
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
    for (int row=0; row<MODERN_FONT_H; row++) {
        for (int col=0; col<MODERN_FONT_W; col++) {
            u8 cov = modern_font_aa[idx][row][col];
            if (cov==0) continue;
            u32 out;
            if (cov==255) out = RGB(fr, fg_g, fb);
            else {
                u32 bg = sf_getpx(s, x+col, y+row);
                u8 br = R(bg), bg_g = G(bg), bb = B(bg);
                u8 nr = (u8)((fr*cov + br*(255-cov) +127)/255);
                u8 ng = (u8)((fg_g*cov + bg_g*(255-cov) +127)/255);
                u8 nb = (u8)((fb*cov + bb*(255-cov) +127)/255);
                out = RGB(nr, ng, nb);
            }
            sf_putpx(s, x+col, y+row, out);
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
    for (int row=0; row<MODERN_FONT_H; row++) {
        for (int col=0; col<MODERN_FONT_W; col++) {
            u8 cov = modern_font_aa[idx][row][col];
            if (cov==0) continue;
            u32 eff_a = (u32)cov * fa / 255;
            if (eff_a==0) continue;
            u32 c = ARGB(eff_a, fr, fg_g, fb);
            sf_putpx_blend(s, x+col, y+row, c);
        }
    }
}
void sf_text(surface_t *s, int x, int y, const char *t, u32 fg) {
    while (*t) { sf_putc(s, x, y, *t++, fg); x += FONT_W; }
}
void sf_text_blend(surface_t *s, int x, int y, const char *t, u32 fg) {
    while (*t) { sf_putc_blend(s, x, y, *t++, fg); x += FONT_W; }
}
int sf_text_width(const char *t) { int n=0; while (*t++) n++; return n*FONT_W; }

void itoa0(int v, char *b, int w) {
    char tmp[16]; int i = 0; int neg = 0;
    if (v < 0) { neg = 1; v = -v; } if (v == 0) tmp[i++] = '0';
    while (v) { tmp[i++] = '0' + v%10; v /= 10; }
    int k = 0; if (neg) b[k++] = '-';
    int digits = i; int pad = (w > digits + (neg?1:0)) ? w - digits - (neg?1:0) : 0;
    while (pad-- > 0) b[k++] = '0'; while (i) b[k++] = tmp[--i]; b[k] = 0;
}

/* ----- assets ----- */
extern const char _binary_kora_bin_start[];
extern const char _binary_kora_bin_end[];
extern const char _binary_wallpaper_bin_start[];
extern const char _binary_wallpaper_bin_end[];
typedef struct { u32 name_off; u16 name_len; u32 px_off; u16 w, h, pitch; } __attribute__((packed)) ic_ent_t;
static struct { int count; const ic_ent_t *ent; const u8 *blob; } G_icons;
static const u8  *G_wp_base; static const u32 *G_wp_pixels; static int G_wp_w, G_wp_h, G_wp_count, G_wp_index;
static const u8 *wallpaper_entry(int idx, int *w_out, int *h_out) {
    const u8 *b = G_wp_base; if (!b) return 0; if (b[0]!='Y'||b[1]!='W'||b[2]!='A'||b[3]!='L'||b[4]!='L') return 0;
    int version = b[5]; if (version == 1) { if (idx != 0) return 0; int w = b[6] | (b[7]<<8); int h = b[8] | (b[9]<<8); *w_out = w; *h_out = h; return b + 16; }
    if (version == 2) { int count = b[6] | (b[7]<<8); if (idx < 0 || idx >= count) return 0; const u32 *offs = (const u32*)(b + 16); const u8 *e = b + offs[idx]; int w = e[0] | (e[1]<<8); int h = e[2] | (e[3]<<8); *w_out = w; *h_out = h; return e + 8; } return 0;
}
void assets_init(void) {
    const u8 *b = (const u8*)_binary_kora_bin_start; if (b[0]!='Y'||b[1]!='I'||b[2]!='C'||b[3]!='O'||b[4]!='N') return;
    u16 count = *(const u16*)(b+6); G_icons.count = count; asm volatile("" ::: "memory"); G_icons.ent = (const ic_ent_t*)(b+16); asm volatile("" ::: "memory"); G_icons.blob = b; asm volatile("" ::: "memory"); font_build();
}
int wallpaper_count(void) {
    if (!G_wp_base) {
        G_wp_base = (const u8*)_binary_wallpaper_bin_start; const u8 *b = G_wp_base;
        if (b[0]!='Y'||b[1]!='W'||b[2]!='A'||b[3]!='L'||b[4]!='L') return 0;
        int ver = b[5]; if (ver == 1) G_wp_count = 1; else if (ver == 2) G_wp_count = b[6] | (b[7]<<8); else G_wp_count = 0;
    } return G_wp_count;
}
int wallpaper_load_index(int idx) {
    (void)wallpaper_count(); int w,h; const u8 *px = wallpaper_entry(idx, &w, &h); if (!px) return -1;
    G_wp_index = idx; G_wp_w = w; G_wp_h = h; G_wp_pixels = (const u32*)px; return 0;
}
int wallpaper_load(surface_t *out) {
    if (wallpaper_load_index(0) < 0) return -1; if (out) { out->px=(u32*)G_wp_pixels; out->w=G_wp_w; out->h=G_wp_h; out->pitch=G_wp_w; } return 0;
}
void wallpaper_bind(surface_t *s) { if (s) { s->px=(u32*)G_wp_pixels; s->w=G_wp_w; s->h=G_wp_h; s->pitch=G_wp_w; } }
int wallpaper_current_index(void) { return G_wp_index; }
int wallpaper_width(void) { return G_wp_w; } int wallpaper_height(void) { return G_wp_h; }
const u32 *wallpaper_pixels(void) { return G_wp_pixels; }
u32 wallpaper_px(int x, int y) { if ((u32)x>=(u32)G_wp_w || (u32)y>=(u32)G_wp_h) return 0xFF000000; const u8 *p = (const u8*)G_wp_pixels + ((long)y*G_wp_w + x)*4; return ARGB(p[3], p[0], p[1], p[2]); }
icon_t icon_get(int id) { icon_t z = {0}; if (id < 0 || id >= G_icons.count) return z; const ic_ent_t *e = &G_icons.ent[id]; z.px = (const u32*)(G_icons.blob + e->px_off); z.w = e->w; z.h = e->h; z.pitch = e->pitch / 4; return z; }
void sf_icon_tl(surface_t *s, int x, int y, icon_t ico, u32 tint) {
    if (!ico.px || !ico.w || !ico.h) return; u32 tR = R(tint), tG = G(tint), tB = B(tint); int has_tint = (tint != 0);
    for (int j = 0; j < ico.h; j++) { const u32 *src = ico.px + (long)j*ico.pitch; u32 *dp = s->px + (long)(y+j)*s->pitch + x; for (int i = 0; i < ico.w; i++) { u32 sp = src[i]; u8 sr = (u8)sp; u8 sg = (u8)(sp>>8); u8 sb = (u8)(sp>>16); u8 sa = (u8)(sp>>24); if (!sa) continue; u32 c; if (has_tint) c = ARGB(sa, tR, tG, tB); else c = ARGB(sa, sr, sg, sb); int dx = i; if (sa == 255) dp[dx] = c; else { u32 d = dp[dx]; u32 ia = 255-sa; dp[dx] = 0xFF000000 | ((((c>>16)&0xFF)*sa + ((d>>16)&0xFF)*ia + 127)/255 << 16) | ((((c>>8)&0xFF)*sa  + ((d>>8)&0xFF)*ia + 127)/255 << 8) | (((c&0xFF)*sa     + (d&0xFF)*ia + 127)/255); } } }
}
void sf_icon_tl_blend(surface_t *s, int x, int y, icon_t ico, u32 tint, u8 global_alpha) {
    if (!ico.px || !ico.w || !ico.h) return; u32 tR = R(tint), tG = G(tint), tB = B(tint); int has_tint = (tint != 0);
    for (int j = 0; j < ico.h; j++) { const u32 *src = ico.px + (long)j*ico.pitch; for (int i = 0; i < ico.w; i++) { u32 sp = src[i]; u8 sa = (u8)((((u8)(sp>>24))*(u32)global_alpha + 127)/255); if (!sa) continue; u32 c; if (has_tint) c = ARGB(sa, tR, tG, tB); else c = ARGB(sa, (u8)sp, (u8)(sp>>8), (u8)(sp>>16)); sf_putpx_blend(s, x+i, y+j, c); } }
}
void sf_icon(surface_t *s, int cx, int cy, icon_t ico, u32 tint) { sf_icon_tl(s, cx - ico.w/2, cy - ico.h/2, ico, tint); }
void sf_icon_scaled(surface_t *s, int cx, int cy, icon_t ico, u32 tint, int scale_num, int scale_den) {
    if (!ico.px || !ico.w || !ico.h) return; int dw = (ico.w*scale_num + scale_den/2)/scale_den; int dh = (ico.h*scale_num + scale_den/2)/scale_den; int x0 = cx - dw/2, y0 = cy - dh/2; u32 tR=R(tint), tG=G(tint), tB=B(tint); int has_tint = (tint!=0);
    for (int j=0;j<dh;j++) { int sy = (j*ico.h)/dh; if (sy >= ico.h) sy = ico.h-1; const u32 *src = ico.px + (long)sy*ico.pitch; for (int i=0;i<dw;i++) { int sx = (i*ico.w)/dw; if (sx >= ico.w) sx = ico.w-1; u32 sp = src[sx]; u8 sa = (u8)(sp>>24); if (!sa) continue; u32 c; if (has_tint) c = ARGB(sa, tR, tG, tB); else c = ARGB(sa, (u8)sp, (u8)(sp>>8), (u8)(sp>>16)); if (sa == 255) sf_putpx(s, x0+i, y0+j, c); else sf_putpx_blend(s, x0+i, y0+j, c); } }
}
surface_t sf_alloc(int w, int h) {
    surface_t s = {0}; size_t bytes = (size_t)w*h*4; size_t pagesz = 4096; size_t alloc = (bytes + pagesz-1) & ~(pagesz-1); void *mem = (void*)mmap((long)alloc); if (!mem) return s; s.px = (u32*)mem; s.w = w; s.h = h; s.pitch = w; return s;
}
void sf_free(surface_t *s) { if (s->px) munmap((long)s->px); s->px = 0; s->w = s->h = s->pitch = 0; }
