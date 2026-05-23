/* Yart OS - Calculator.
 *
 * Tiny stack-based RPN calculator with a 4-row keypad.  Click numbers
 * and ops with the mouse, or press digits/+ - * / Enter Backspace on
 * the keyboard.
 */
#include <yart/gui.h>
#include <yart/theme.h>
#include <yart/icons.h>
#include <yart/string.h>
#include <yart/mm.h>
#include <yart/menu.h>

extern int  g_editor_click_x, g_editor_click_y;
extern bool g_editor_click_pending;

typedef struct {
    char  display[32];
    long  acc;       /* accumulator */
    long  cur;       /* current entry */
    bool  entering;
    char  pending_op;
    bool  prev_left;
} calc_t;

static const char *labels[] = {
    "C", "<", "/", "*",
    "7", "8", "9", "-",
    "4", "5", "6", "+",
    "1", "2", "3", "=",
    "0", ".",
};

static void calc_format(calc_t *c) {
    if (c->entering) snprintf(c->display, sizeof c->display, "%ld", c->cur);
    else             snprintf(c->display, sizeof c->display, "%ld", c->acc);
}

static void calc_apply(calc_t *c) {
    if (c->pending_op == 0) { c->acc = c->cur; return; }
    switch (c->pending_op) {
    case '+': c->acc += c->cur; break;
    case '-': c->acc -= c->cur; break;
    case '*': c->acc *= c->cur; break;
    case '/': c->acc = c->cur ? c->acc / c->cur : 0; break;
    }
    c->pending_op = 0;
    c->cur = 0;
    c->entering = false;
}

static void calc_press(calc_t *c, char k) {
    if (k >= '0' && k <= '9') {
        if (!c->entering) { c->cur = 0; c->entering = true; }
        c->cur = c->cur * 10 + (k - '0');
    } else if (k == 'C') {
        c->cur = c->acc = 0;
        c->entering = false;
        c->pending_op = 0;
    } else if (k == '<') {
        if (c->entering) c->cur /= 10;
    } else if (k == '+' || k == '-' || k == '*' || k == '/') {
        if (c->entering) {
            if (c->pending_op == 0) c->acc = c->cur;
            else calc_apply(c);
            c->entering = false;
        }
        c->pending_op = k;
    } else if (k == '=' || k == '\n') {
        if (c->entering) calc_apply(c);
        c->entering = false;
    }
    calc_format(c);
}

static void calc_paint(window_t *w) {
    calc_t *c = w->ud;
    if (!c->display[0]) calc_format(c);

    /* display */
    int dx = w->x + 12, dy = w->y + WIN_TITLE_H + 12;
    int dw = w->w - 24, dh = 44;
    draw_rect(dx, dy, dw, dh, TH_EDITOR_BG);
    draw_rect_outline(dx, dy, dw, dh, TH_WIN_BORDER);
    int tw = text_width(c->display);
    draw_text(dx + dw - tw - 12, dy + (dh - FONT_H)/2,
              c->display, TH_ACCENT, 0);

    /* keypad */
    int kx0 = w->x + 12, ky0 = w->y + WIN_TITLE_H + 12 + dh + 12;
    int kw = (w->w - 24 - 9) / 4;       /* 3 gaps of 3 px */
    int kh = 36;
    int cxm, cym; cursor_get_pos(&cxm, &cym);

    int idx = 0;
    for (int row = 0; row < 5; row++) {
        for (int col = 0; col < 4; col++) {
            if (idx >= (int)(sizeof labels / sizeof labels[0])) break;
            int kx = kx0 + col * (kw + 3);
            int ky = ky0 + row * (kh + 3);
            /* last row: 0 spans 2, '.' single */
            int this_kw = kw;
            if (row == 4 && col == 0) this_kw = kw * 2 + 3;
            if (row == 4 && col == 1) { idx++; continue; }
            const char *lbl = labels[idx++];
            bool hover = (cxm >= kx && cxm < kx + this_kw &&
                          cym >= ky && cym < ky + kh);
            color_t bg = TH_PANEL_HI;
            color_t fg = TH_TEXT;
            bool isop = (lbl[0] == '+' || lbl[0] == '-' || lbl[0] == '*' || lbl[0] == '/' || lbl[0] == '=');
            bool isspecial = (lbl[0] == 'C' || lbl[0] == '<');
            if (isop)      { bg = TH_ACCENT_BG; fg = TH_ACCENT; }
            if (isspecial) { bg = TH_PANEL;     fg = TH_WARN; }
            if (hover) { bg = TH_ACCENT_BG; fg = TH_ACCENT; }
            draw_rounded_rect(kx, ky, this_kw, kh, 5, bg);
            draw_rounded_rect_outline(kx, ky, this_kw, kh, 5, TH_WIN_BORDER);
            int lw = text_width(lbl);
            draw_text(kx + (this_kw - lw)/2, ky + (kh - FONT_H)/2, lbl, fg, 0);
            if (col == 3) break;       /* only one column on last row's row[1..3] handled */
        }
    }

    /* click handling: reuse the editor pending channel for now */
    if (g_editor_click_pending) {
        int ccx = g_editor_click_x, ccy = g_editor_click_y;
        g_editor_click_pending = false;
        int idx2 = 0;
        for (int row = 0; row < 5; row++) {
            for (int col = 0; col < 4; col++) {
                if (idx2 >= (int)(sizeof labels / sizeof labels[0])) break;
                int kx = kx0 + col * (kw + 3);
                int ky = ky0 + row * (kh + 3);
                int this_kw = kw;
                if (row == 4 && col == 0) this_kw = kw * 2 + 3;
                if (row == 4 && col == 1) { idx2++; continue; }
                const char *lbl = labels[idx2++];
                if (ccx >= kx && ccx < kx + this_kw &&
                    ccy >= ky && ccy < ky + kh) {
                    calc_press(c, lbl[0]);
                    return;
                }
                if (col == 3) break;
            }
        }
    }
}

static void calc_on_key(window_t *w, int sc, char ch, u32 mods) {
    (void)mods;
    UNUSED(sc);
    calc_t *c = w->ud;
    if ((ch >= '0' && ch <= '9') ||
        ch == '+' || ch == '-' || ch == '*' || ch == '/' ||
        ch == '=' || ch == '\n')      { calc_press(c, ch); return; }
    if (ch == '\b' || ch == 8)        { calc_press(c, '<'); return; }
    if (ch == 'c' || ch == 'C' ||
        ch == 27 /* esc */)           { calc_press(c, 'C'); return; }
}

void open_calc(void) {
    calc_t *c = kzalloc(sizeof *c);
    int W = 250, H = 320;
    int x = 30;
    int y = 50;
    window_t *win = window_create("Calculator", ICON_CALC, x, y, W, H, calc_paint);
    if (!win) { kfree(c); return; }
    win->ud = c;
    win->on_key = calc_on_key;
    win->min_w = 220; win->min_h = 290;
}
