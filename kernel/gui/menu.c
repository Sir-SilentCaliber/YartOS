/* Yart OS - context menus + toast notifications */
#include <yart/menu.h>
#include <yart/gui.h>
#include <yart/theme.h>
#include <yart/string.h>
#include <yart/hal.h>
#include <stdarg.h>

/* ---------- menu ---------- */
#define MENU_PAD_X     14
#define MENU_PAD_Y     6
#define MENU_ROW_H     22
#define MENU_SEP_H     6

static struct {
    bool         open;
    int          x, y, w, h;
    menu_item_t  items[MENU_MAX_ITEMS];
    int          n;
    int          hover;
} g_menu;

void menu_open(int x, int y, menu_item_t *items, int n) {
    if (n <= 0 || n > MENU_MAX_ITEMS) return;
    g_menu.open = true;
    g_menu.n = n;
    int max_w = 80;
    int total_h = MENU_PAD_Y * 2;
    for (int i = 0; i < n; i++) {
        g_menu.items[i] = items[i];
        if (items[i].separator) total_h += MENU_SEP_H;
        else {
            total_h += MENU_ROW_H;
            int w = (int)strlen(items[i].label) * FONT_W + MENU_PAD_X * 2;
            if (w > max_w) max_w = w;
        }
    }
    g_menu.w = max_w;
    g_menu.h = total_h;
    g_menu.x = x;
    g_menu.y = y;
    /* clamp to screen */
    if (g_menu.x + g_menu.w > (int)g_fb.width)  g_menu.x = g_fb.width  - g_menu.w - 4;
    if (g_menu.y + g_menu.h > (int)g_fb.height) g_menu.y = g_fb.height - g_menu.h - 4;
    if (g_menu.x < 0) g_menu.x = 0;
    if (g_menu.y < 0) g_menu.y = 0;
}

void menu_close(void)    { g_menu.open = false; g_menu.hover = -1; }
bool menu_is_open(void)  { return g_menu.open; }

static int row_y(int i) {
    int y = g_menu.y + MENU_PAD_Y;
    for (int k = 0; k < i; k++)
        y += g_menu.items[k].separator ? MENU_SEP_H : MENU_ROW_H;
    return y;
}

void menu_render(void) {
    if (!g_menu.open) return;
    /* shadow */
    for (int s = 1; s <= 5; s++) {
        u32 a = 70 - s * 12;
        color_t sc = (a << 24);
        draw_hline(g_menu.x + s, g_menu.y + g_menu.h, g_menu.w, sc);
        draw_vline(g_menu.x + g_menu.w, g_menu.y + s, g_menu.h, sc);
    }
    draw_rounded_rect(g_menu.x, g_menu.y, g_menu.w, g_menu.h, 5, TH_PANEL_HI);
    draw_rounded_rect_outline(g_menu.x, g_menu.y, g_menu.w, g_menu.h, 5, TH_WIN_BORDER);
    int cx, cy; cursor_get_pos(&cx, &cy);
    g_menu.hover = -1;
    for (int i = 0; i < g_menu.n; i++) {
        int y = row_y(i);
        if (g_menu.items[i].separator) {
            draw_hline(g_menu.x + 6, y + MENU_SEP_H/2,
                       g_menu.w - 12, TH_TEXT_MUTED);
            continue;
        }
        bool hover = (cx >= g_menu.x && cx < g_menu.x + g_menu.w &&
                      cy >= y && cy < y + MENU_ROW_H);
        if (hover && !g_menu.items[i].disabled) {
            g_menu.hover = i;
            draw_rect(g_menu.x + 2, y, g_menu.w - 4, MENU_ROW_H, TH_ACCENT_BG);
        }
        color_t c = g_menu.items[i].disabled ? TH_TEXT_MUTED :
                    (hover ? TH_ACCENT : TH_TEXT);
        draw_text(g_menu.x + MENU_PAD_X, y + 3, g_menu.items[i].label, c, 0);
    }
}

bool menu_handle_click(int x, int y, bool down) {
    if (!g_menu.open) return false;
    if (!down) return true;        /* swallow up while open */
    /* click outside? close */
    if (x < g_menu.x || x >= g_menu.x + g_menu.w ||
        y < g_menu.y || y >= g_menu.y + g_menu.h) {
        menu_close();
        return true;
    }
    if (g_menu.hover >= 0) {
        menu_item_t *it = &g_menu.items[g_menu.hover];
        if (!it->disabled && it->on_click) {
            void (*cb)(void *) = it->on_click;
            void *ud = it->ud;
            menu_close();
            cb(ud);
            return true;
        }
    }
    menu_close();
    return true;
}

/* ---------- toast ---------- */
#define TOAST_MAX  6
#define TOAST_LIFE 2500   /* ms */
typedef struct {
    char  text[80];
    u64   born_ms;
    bool  in_use;
} toast_t;

static toast_t g_toasts[TOAST_MAX];

void toast(const char *fmt, ...) {
    char buf[80];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    /* shift */
    for (int i = TOAST_MAX - 1; i > 0; i--)
        g_toasts[i] = g_toasts[i-1];
    g_toasts[0].in_use = true;
    g_toasts[0].born_ms = pit_ticks() * 10;
    strncpy(g_toasts[0].text, buf, sizeof g_toasts[0].text - 1);
    g_toasts[0].text[sizeof g_toasts[0].text - 1] = 0;
}

void toasts_render(u64 now_ms) {
    int y = (int)g_fb.height - 80;     /* above dock */
    for (int i = 0; i < TOAST_MAX; i++) {
        toast_t *t = &g_toasts[i];
        if (!t->in_use) continue;
        u64 age = now_ms - t->born_ms;
        if (age > TOAST_LIFE) { t->in_use = false; continue; }
        int tw = text_width(t->text);
        int W = tw + 28, H = 30;
        if (W > 360) W = 360;
        int x = g_fb.width - W - 30;
        if (x < 8) x = 8;
        /* simple slide: first 200ms ease in from right */
        int slide = 0;
        if (age < 200) slide = (int)((200 - age) * 40 / 200);
        x += slide;
        /* fade out last 400ms */
        u32 alpha = 255;
        if (age > TOAST_LIFE - 400) {
            alpha = (u32)((TOAST_LIFE - age) * 255 / 400);
        }
        if (alpha < 60) alpha = 60;
        UNUSED(alpha);    /* simple opaque draw is fine on this look */
        draw_rounded_rect(x, y, W, H, 6, TH_PANEL_HI);
        draw_rounded_rect_outline(x, y, W, H, 6, TH_ACCENT_DIM);
        draw_text(x + 14, y + 7, t->text, TH_TEXT, 0);
        y -= H + 8;
    }
}

bool toasts_active(void) {
    for (int i = 0; i < TOAST_MAX; i++) if (g_toasts[i].in_use) return true;
    return false;
}
