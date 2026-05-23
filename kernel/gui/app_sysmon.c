/* Yart OS - System Monitor.
 *
 * Live window with:
 *   - Memory bar + numerical readout
 *   - Scrolling memory-usage history graph (sample per second)
 *   - Uptime, ticks, IRQ counts
 *   - Process list (just init + kernel today)
 */
#include <yart/gui.h>
#include <yart/theme.h>
#include <yart/icons.h>
#include <yart/string.h>
#include <yart/mm.h>
#include <yart/hal.h>

#define HIST 120
typedef struct {
    u8   mem_pct[HIST];   /* 0..100 */
    int  head;
    u64  last_sample_ms;
} mon_t;

static void sample(mon_t *m, u64 now_ms) {
    if (now_ms - m->last_sample_ms < 250) return;
    m->last_sample_ms = now_ms;
    u64 used  = pmm_used_pages() * PAGE_SIZE;
    u64 total = pmm_total_pages() * PAGE_SIZE;
    if (total == 0) return;
    u8 pct = (u8)(used * 100 / total);
    m->mem_pct[m->head] = pct;
    m->head = (m->head + 1) % HIST;
}

static void mon_paint(window_t *w) {
    mon_t *m = w->ud;
    sample(m, pit_ticks() * 10);

    int x = w->x + 12, y = w->y + WIN_TITLE_H + 12;
    int W = w->w - 24, H = w->h - WIN_TITLE_H - 24;

    /* header info */
    char buf[120];
    size_t total = pmm_total_pages() * PAGE_SIZE / MB(1);
    size_t used  = pmm_used_pages()  * PAGE_SIZE / MB(1);
    snprintf(buf, sizeof buf, "Memory  %lu / %lu MiB", used, total);
    draw_text(x, y, buf, TH_TEXT, 0);

    /* memory bar */
    int barw = W - 12;
    draw_rounded_rect_outline(x, y + FONT_H + 4, barw, 12, 3, TH_TEXT_MUTED);
    int fill = total ? (int)((u64)(barw - 4) * used / total) : 0;
    color_t fc = used * 100 / total > 80 ? TH_ERR :
                 used * 100 / total > 60 ? TH_WARN : TH_OK;
    draw_rect(x + 2, y + FONT_H + 6, fill, 8, fc);

    /* graph */
    int gy = y + FONT_H + 26;
    int gh = H - (FONT_H + 26) - 80;
    if (gh < 30) gh = 30;
    draw_rect(x, gy, W, gh, TH_EDITOR_BG);
    draw_rect_outline(x, gy, W, gh, TH_WIN_BORDER);
    /* horizontal grid lines at 25/50/75 */
    for (int i = 1; i < 4; i++) {
        int ly = gy + (gh - 2) * i / 4;
        for (int xi = x + 2; xi < x + W - 2; xi += 4)
            draw_pixel(xi, ly, TH_TEXT_MUTED);
    }
    /* draw bars */
    int bw = (W - 4) / HIST;
    if (bw < 1) bw = 1;
    for (int i = 0; i < HIST; i++) {
        int idx = (m->head + i) % HIST;
        int v = m->mem_pct[idx];
        int bh = (gh - 4) * v / 100;
        int bx = x + 2 + i * bw;
        draw_rect(bx, gy + gh - 2 - bh, bw - 1, bh, TH_ACCENT_DIM);
        draw_rect(bx, gy + gh - 2 - bh, bw - 1, 1, TH_ACCENT);
    }
    draw_text(x + 6, gy + 4, "memory %", TH_TEXT_DIM, 0);

    /* footer info */
    int fy = gy + gh + 10;
    snprintf(buf, sizeof buf, "Uptime  %lu s   Ticks  %lu",
             (unsigned long)(pit_ticks() / 100),
             (unsigned long)pit_ticks());
    draw_text(x, fy, buf, TH_TEXT, 0); fy += FONT_H;

    snprintf(buf, sizeof buf, "Display %ux%u  Pages %lu / %lu",
             g_fb.width, g_fb.height,
             pmm_used_pages(), pmm_total_pages());
    draw_text(x, fy, buf, TH_TEXT_DIM, 0); fy += FONT_H;

    snprintf(buf, sizeof buf, "Tasks   1 (init exited 0)   Kernel: pid 0");
    draw_text(x, fy, buf, TH_TEXT_DIM, 0);
}

void open_sysmon(void) {
    mon_t *m = kzalloc(sizeof *m);
    int W = 540, H = 360;
    int x = 30;
    int y = 410;
    window_t *win = window_create("System Monitor", ICON_MONITOR, x, y, W, H, mon_paint);
    if (!win) { kfree(m); return; }
    win->ud = m;
    win->min_w = 380; win->min_h = 240;
    win->flags |= WIN_ANIM;
}
