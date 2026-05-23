/* Yart OS - small built-in apps: Clock, System Info, Settings */
#include <yart/gui.h>
#include <yart/theme.h>
#include <yart/string.h>
#include <yart/mm.h>
#include <yart/hal.h>
#include <yart/icons.h>

/* ---- shared helpers ---- */
static void panel(window_t *w, color_t c) {
    int x = w->x + 8, y = w->y + WIN_TITLE_H + 8;
    int W = w->w - 16, H = w->h - WIN_TITLE_H - 16;
    draw_rounded_rect(x, y, W, H, 4, c);
    draw_rounded_rect_outline(x, y, W, H, 4, TH_WIN_BORDER);
}

/* ---- analog clock ---- */
static const int sin_lut[16] = {
    0, 391, 707, 923, 1000, 923, 707, 391,
    0, -391, -707, -923, -1000, -923, -707, -391
};
static int isin_q(int idx) { return sin_lut[idx & 15]; }
static int icos_q(int idx) { return sin_lut[(idx + 4) & 15]; }
static void line(int x0, int y0, int x1, int y1, color_t c) {
    int dx =  ((x1 > x0) ? (x1 - x0) : (x0 - x1));
    int dy = -((y1 > y0) ? (y1 - y0) : (y0 - y1));
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        draw_pixel(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}
static void thick(int x0, int y0, int x1, int y1, color_t c) {
    line(x0, y0, x1, y1, c);
    line(x0+1, y0, x1+1, y1, c);
    line(x0, y0+1, x1, y1+1, c);
}

static void clock_paint(window_t *w) {
    panel(w, TH_WIN_BG);
    int cx = w->x + w->w / 2;
    int cy = w->y + WIN_TITLE_H + (w->h - WIN_TITLE_H) / 2;
    int r  = (w->h - WIN_TITLE_H) / 2 - 24;
    if (r < 30) r = 30;

    /* face */
    for (int t = 0; t < 60; t++) {
        int idx = (t * 16) / 60;
        int x = cx + isin_q(idx) * r / 1000;
        int y = cy - icos_q(idx) * r / 1000;
        color_t c = (t % 5 == 0) ? TH_TEXT : TH_TEXT_MUTED;
        draw_rect(x - 1, y - 1, 3, 3, c);
    }

    rtc_time_t t; rtc_read(&t);
    int hi = ((t.hour % 12) * 16) / 12;
    int mi = (t.minute * 16) / 60;
    int subsec = (int)(pit_ticks() % 100);
    int si = (t.second * 16 * 100 + subsec * 16) / (60 * 100);

    int hx = cx + isin_q(hi) * (r * 5/8) / 1000;
    int hy = cy - icos_q(hi) * (r * 5/8) / 1000;
    thick(cx, cy, hx, hy, TH_TEXT);

    int mx_ = cx + isin_q(mi) * (r * 7/8) / 1000;
    int my_ = cy - icos_q(mi) * (r * 7/8) / 1000;
    thick(cx, cy, mx_, my_, TH_TEXT);

    int sx_ = cx + isin_q(si) * (r * 9/10) / 1000;
    int sy_ = cy - icos_q(si) * (r * 9/10) / 1000;
    line(cx, cy, sx_, sy_, TH_ACCENT);

    draw_rect(cx - 2, cy - 2, 5, 5, TH_ACCENT);

    char ts[32];
    snprintf(ts, sizeof ts, "%02u:%02u:%02u", t.hour, t.minute, t.second);
    int tw = text_width(ts);
    draw_text(cx - tw / 2, w->y + w->h - 30, ts, TH_TEXT, 0);
}

void open_clock(void) {
    int s = 320;
    window_t *win = window_create("Clock", ICON_CLOCK,
        100, 100, s, s, clock_paint);
    if (!win) return;
    win->min_w = 240; win->min_h = 240;
    win->flags |= WIN_ANIM;
}

/* ---- sysinfo ---- */
static void info_paint(window_t *w) {
    panel(w, TH_WIN_BG);
    int x = w->x + 32, y = w->y + WIN_TITLE_H + 32;
    char buf[120];

    draw_text(x, y, "System", TH_ACCENT, 0); y += FONT_H + 8;

    snprintf(buf, sizeof buf, "OS         : Yart OS %s", YART_VERSION);
    draw_text(x, y, buf, TH_TEXT, 0); y += FONT_H;

    snprintf(buf, sizeof buf, "Arch       : x86_64 long mode");
    draw_text(x, y, buf, TH_TEXT, 0); y += FONT_H;

    snprintf(buf, sizeof buf, "Bootloader : Limine (UEFI/BIOS hybrid)");
    draw_text(x, y, buf, TH_TEXT, 0); y += FONT_H;

    size_t total = pmm_total_pages() * PAGE_SIZE / MB(1);
    size_t used  = pmm_used_pages()  * PAGE_SIZE / MB(1);
    snprintf(buf, sizeof buf, "Memory     : %lu / %lu MiB", used, total);
    draw_text(x, y, buf, TH_TEXT, 0); y += FONT_H;

    snprintf(buf, sizeof buf, "Display    : %ux%u@%u", g_fb.width, g_fb.height, g_fb.bpp);
    draw_text(x, y, buf, TH_TEXT, 0); y += FONT_H;

    snprintf(buf, sizeof buf, "Uptime     : %lu s", (unsigned long)(pit_ticks() / 100));
    draw_text(x, y, buf, TH_TEXT, 0); y += FONT_H;

    rtc_time_t t; rtc_read(&t);
    snprintf(buf, sizeof buf, "Time       : %04u-%02u-%02u %02u:%02u:%02u",
             t.year, t.month, t.day, t.hour, t.minute, t.second);
    draw_text(x, y, buf, TH_TEXT, 0); y += FONT_H;
    y += 8;

    /* memory bar */
    int bw = w->w - 80;
    draw_rounded_rect_outline(x, y, bw, 14, 3, TH_TEXT_MUTED);
    int fill = (int)((u64)(bw - 4) * used / total);
    draw_rect(x + 2, y + 2, fill, 10, TH_ACCENT);
    y += 22;

    draw_text(x, y, "Press F1 anywhere to open the application drawer.",
              TH_TEXT_DIM, 0);
}

void open_sysinfo(void) {
    int w = 520, h = 320;
    window_t *win = window_create("System", ICON_INFO,
        140, 140, w, h, info_paint);
    if (!win) return;
    win->min_w = 380; win->min_h = 260;
    win->flags |= WIN_ANIM;
}

