/* Yart OS - framebuffer + drawing primitives + double buffering */
#include <yart/gui.h>
#include <yart/mm.h>
#include <yart/string.h>
#include <yart/console.h>

fb_ctx_t g_fb;

void fb_init(struct limine_framebuffer *lfb) {
    g_fb.fb       = (u32 *)lfb->address;
    g_fb.width    = lfb->width;
    g_fb.height   = lfb->height;
    g_fb.bpp      = lfb->bpp;
    g_fb.pitch_px = lfb->pitch / 4;
    g_fb.rgb      = (lfb->red_mask_shift > lfb->blue_mask_shift);

    size_t bytes = (size_t)g_fb.pitch_px * g_fb.height * 4;
    size_t pages = PAGE_ALIGN_UP(bytes) / PAGE_SIZE;
    paddr_t p = pmm_alloc_pages(pages);
    g_fb.pixels = (u32 *)phys_to_virt(p);
    fb_clear(0xFF11141A);
    kprintf("fb: %ux%u@%u  pitch=%u  rgb=%d\n",
            g_fb.width, g_fb.height, g_fb.bpp, g_fb.pitch_px, (int)g_fb.rgb);
}

static ALWAYS_INLINE u32 conv(color_t c) {
    if (g_fb.rgb) return c;
    return (c & 0xFF00FF00) | ((c & 0x00FF0000) >> 16) | ((c & 0x000000FF) << 16);
}

void fb_clear(color_t c) {
    u32 v = conv(c);
    for (u32 y = 0; y < g_fb.height; y++) {
        u32 *row = g_fb.pixels + y * g_fb.pitch_px;
        for (u32 x = 0; x < g_fb.width; x++) row[x] = v;
    }
}

void fb_present(void) {
    for (u32 y = 0; y < g_fb.height; y++) {
        u64 *src = (u64 *)(g_fb.pixels + y * g_fb.pitch_px);
        u64 *dst = (u64 *)(g_fb.fb     + y * g_fb.pitch_px);
        u32 n = g_fb.pitch_px / 2;
        for (u32 i = 0; i < n; i++) dst[i] = src[i];
        if (g_fb.pitch_px & 1) {
            ((u32 *)dst)[g_fb.pitch_px - 1] = ((u32 *)src)[g_fb.pitch_px - 1];
        }
    }
}

void draw_pixel(int x, int y, color_t c) {
    if ((u32)x >= g_fb.width || (u32)y >= g_fb.height) return;
    g_fb.pixels[y * g_fb.pitch_px + x] = conv(c);
}

void draw_hline(int x, int y, int w, color_t c) {
    if (y < 0 || (u32)y >= g_fb.height) return;
    if (x < 0) { w += x; x = 0; }
    if (x + w > (int)g_fb.width) w = g_fb.width - x;
    if (w <= 0) return;
    u32 v = conv(c);
    u32 *row = g_fb.pixels + y * g_fb.pitch_px + x;
    for (int i = 0; i < w; i++) row[i] = v;
}

void draw_vline(int x, int y, int h, color_t c) {
    if (x < 0 || (u32)x >= g_fb.width) return;
    if (y < 0) { h += y; y = 0; }
    if (y + h > (int)g_fb.height) h = g_fb.height - y;
    if (h <= 0) return;
    u32 v = conv(c);
    for (int i = 0; i < h; i++) g_fb.pixels[(y + i) * g_fb.pitch_px + x] = v;
}

void draw_rect(int x, int y, int w, int h, color_t c) {
    for (int j = 0; j < h; j++) draw_hline(x, y + j, w, c);
}

void draw_rect_outline(int x, int y, int w, int h, color_t c) {
    if (w <= 0 || h <= 0) return;
    draw_hline(x, y, w, c);
    draw_hline(x, y + h - 1, w, c);
    draw_vline(x, y, h, c);
    draw_vline(x + w - 1, y, h, c);
}

static u32 lerp_chan(u32 a, u32 b, int num, int den) {
    int A = a, B = b;
    return (u32)(A + (B - A) * num / den);
}
static color_t lerp_color(color_t a, color_t b, int num, int den) {
    u32 ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF, aa = (a >> 24) & 0xFF;
    u32 br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF, ba = (b >> 24) & 0xFF;
    if (den < 1) den = 1;
    return (lerp_chan(aa,ba,num,den) << 24) |
           (lerp_chan(ar,br,num,den) << 16) |
           (lerp_chan(ag,bg,num,den) << 8)  |
            lerp_chan(ab,bb,num,den);
}

void draw_rect_gradient_v(int x, int y, int w, int h, color_t top, color_t bot) {
    if (h <= 0) return;
    for (int j = 0; j < h; j++) {
        color_t c = lerp_color(top, bot, j, h - 1);
        draw_hline(x, y + j, w, c);
    }
}

/* Filled rectangle with rounded corners.  r <= 0 == draw_rect.        */
void draw_rounded_rect(int x, int y, int w, int h, int r, color_t c) {
    if (r <= 0) { draw_rect(x, y, w, h, c); return; }
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    /* middle band */
    for (int j = r; j < h - r; j++) draw_hline(x, y + j, w, c);
    /* top + bottom bands w/ corner mask */
    for (int j = 0; j < r; j++) {
        int dy = r - j;
        int dx = 0;
        /* circle equation - integer */
        while (dx * dx + dy * dy < r * r) dx++;
        int inset = r - dx;
        draw_hline(x + inset, y + j, w - 2 * inset, c);
        draw_hline(x + inset, y + h - 1 - j, w - 2 * inset, c);
    }
}

void draw_rounded_rect_outline(int x, int y, int w, int h, int r, color_t c) {
    if (r <= 0) { draw_rect_outline(x, y, w, h, c); return; }
    /* straight edges */
    draw_hline(x + r, y,         w - 2 * r, c);
    draw_hline(x + r, y + h - 1, w - 2 * r, c);
    draw_vline(x,         y + r, h - 2 * r, c);
    draw_vline(x + w - 1, y + r, h - 2 * r, c);
    /* corners */
    for (int j = 0; j < r; j++) {
        int dy = r - j;
        int dx = 0;
        while (dx * dx + dy * dy < r * r) dx++;
        int inset = r - dx;
        draw_pixel(x + inset,         y + j,         c);
        draw_pixel(x + w - 1 - inset, y + j,         c);
        draw_pixel(x + inset,         y + h - 1 - j, c);
        draw_pixel(x + w - 1 - inset, y + h - 1 - j, c);
    }
}

void draw_char(int x, int y, char ch, color_t fg, color_t bg) {
    const u8 *g = yart_font8x16[(u8)ch];
    u32 fgv = conv(fg), bgv = conv(bg);
    bool transp_bg = ((bg >> 24) == 0);
    for (int j = 0; j < FONT_H; j++) {
        u8 row = g[j];
        if (y + j < 0 || (u32)(y + j) >= g_fb.height) continue;
        u32 *p = g_fb.pixels + (y + j) * g_fb.pitch_px;
        for (int i = 0; i < FONT_W; i++) {
            int xi = x + i;
            if (xi < 0 || (u32)xi >= g_fb.width) continue;
            if (row & (0x80 >> i)) p[xi] = fgv;
            else if (!transp_bg)   p[xi] = bgv;
        }
    }
}

void draw_text(int x, int y, const char *s, color_t fg, color_t bg) {
    int cx = x;
    while (*s) {
        if (*s == '\n') { y += FONT_H; cx = x; }
        else { draw_char(cx, y, *s, fg, bg); cx += FONT_W; }
        s++;
    }
}

void draw_text_n(int x, int y, const char *s, int n, color_t fg, color_t bg) {
    int cx = x;
    for (int i = 0; i < n && s[i]; i++) {
        if (s[i] == '\n') { y += FONT_H; cx = x; }
        else { draw_char(cx, y, s[i], fg, bg); cx += FONT_W; }
    }
}

int text_width(const char *s) {
    int w = 0; while (*s++) w += FONT_W; return w;
}

bool fb_set_font(const char *name) {
    if (!name) return false;
    for (int i = 0; i < yart_fonts_count; i++) {
        if (strcmp(yart_fonts[i].name, name) == 0) {
            yart_font8x16 = yart_fonts[i].data;
            return true;
        }
    }
    return false;
}
