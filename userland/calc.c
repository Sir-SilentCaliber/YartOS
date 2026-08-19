/* /bin/calc — a simple calculator.
 *
 * Ships ONLY as an installable package (`apk add calc`) to demonstrate the
 * package manager + launcher integration: install it, press Super, and it's
 * in the app grid — like an Ubuntu install.
 */
#include "sys.h"
#include "gfx.h"
#include "fsutil.h"

static int          s_wid;
static wm_surf_info_t s_info;
static surface_t    s_surf;
static char s_expr[64];
static int  s_len;

static void s_put(const char *t) {
    while (*t && s_len < (int)sizeof(s_expr) - 1) s_expr[s_len++] = *t++;
    s_expr[s_len] = 0;
}

/* tiny integer recursive-descent evaluator */
static long eval_expr(const char **p);
static long eval_term(const char **p);
static long eval_fact(const char **p);

static long eval_fact(const char **p) {
    while (**p == ' ') (*p)++;
    if (**p == '(') { (*p)++; long v = eval_expr(p); if (**p == ')') (*p)++; return v; }
    long v = 0;
    while (**p >= '0' && **p <= '9') { v = v * 10 + (**p - '0'); (*p)++; }
    return v;
}
static long eval_term(const char **p) {
    long v = eval_fact(p);
    for (;;) {
        while (**p == ' ') (*p)++;
        if (**p == '*') { (*p)++; v *= eval_fact(p); }
        else if (**p == '/') { (*p)++; long d = eval_fact(p); if (d) v /= d; }
        else break;
    }
    return v;
}
static long eval_expr(const char **p) {
    long v = eval_term(p);
    for (;;) {
        while (**p == ' ') (*p)++;
        if (**p == '+') { (*p)++; v += eval_term(p); }
        else if (**p == '-') { (*p)++; v -= eval_term(p); }
        else break;
    }
    return v;
}

#define BTN_COLS 4
#define BTN_ROWS 5
static const char *s_keys[BTN_ROWS][BTN_COLS] = {
    { "7", "8", "9", "/" },
    { "4", "5", "6", "*" },
    { "1", "2", "3", "-" },
    { "0", "(", ")", "+" },
    { "C", "BS", "=", "=" },
};

static void btn_rect(int r, int c, int *x, int *y, int *w, int *h) {
    int pad = 10, gap = 8;
    int bw = (s_surf.w - pad * 2 - gap * (BTN_COLS - 1)) / BTN_COLS;
    int bh = 52;
    *w = bw; *h = bh;
    *x = pad + c * (bw + gap);
    *y = 150 + r * (bh + gap);
}

static void draw_calc(void) {
    sf_fill(&s_surf, RGB(0x14, 0x14, 0x18));
    sf_round_rect(&s_surf, 10, 14, s_surf.w - 20, 110, 12, RGB(0x0E, 0x0E, 0x12));
    sf_text(&s_surf, 26, 34, s_expr[0] ? s_expr : "0", RGB(0xF0, 0xF4, 0xFA));
    if (s_len > 0) {
        long v = 0; const char *p = s_expr; v = eval_expr(&p);
        char res[64]; fs_itoa(v, res);
        sf_text(&s_surf, 26, 96, res, RGB(0x60, 0xC0, 0x60));
    }
    for (int r = 0; r < BTN_ROWS; r++) {
        for (int c = 0; c < BTN_COLS; c++) {
            const char *kk = s_keys[r][c];
            int x, y, w, h; btn_rect(r, c, &x, &y, &w, &h);
            u32 bg = (c == BTN_COLS - 1) ? RGB(0x35, 0x40, 0x52)
                   : (r == BTN_ROWS - 1) ? RGB(0x2A, 0x2A, 0x34)
                   : RGB(0x24, 0x24, 0x2E);
            sf_round_rect(&s_surf, x, y, w, h, 10, bg);
            int tw = sf_text_width(kk);
            sf_text(&s_surf, x + (w - tw) / 2, y + (h - 18) / 2, kk, RGB(0xE8, 0xEC, 0xF2));
        }
    }
}

static void handle_keypress(char c) {
    if (c == 'C') { s_len = 0; s_expr[0] = 0; return; }
    if (c == 'B' && s_len > 0) { s_len--; s_expr[s_len] = 0; return; }
    if (c == '=' || c == '\n' || c == '\r') {
        const char *p = s_expr; long v = eval_expr(&p);
        fs_itoa(v, s_expr);
        s_len = (int)strlen(s_expr);
        return;
    }
    char tmp[2] = { c, 0 };
    s_put(tmp);
}

static void click_at(int x, int y) {
    for (int r = 0; r < BTN_ROWS; r++) {
        for (int c = 0; c < BTN_COLS; c++) {
            const char *kk = s_keys[r][c];
            int bx, by, bw, bh; btn_rect(r, c, &bx, &by, &bw, &bh);
            if (x >= bx && x < bx + bw && y >= by && y < by + bh) {
                if (kk[0] == 'C') handle_keypress('C');
                else if (kk[0] == 'B') handle_keypress('B');
                else if (kk[0] == '=') handle_keypress('=');
                else handle_keypress(kk[0]);
                return;
            }
        }
    }
}

static int calc_main(void) {
    s_wid = (int)wm_create(320, 480, &s_info);
    if (s_wid < 0) return 1;
    s_surf.px = (u32 *)(unsigned long)s_info.app_va;
    s_surf.w = (int)s_info.w; s_surf.h = (int)s_info.h; s_surf.pitch = (int)s_info.w;
    wm_title(s_wid, "Calculator");

    for (;;) {
        int ev;
        while ((ev = poll_key()) != 0) {
            int ascii = ev & 255, make = !(ev & (1 << 16));
            if (!make) continue;
            if (ascii == 27) return 0;
            if (ascii >= 32 && ascii < 127) handle_keypress((char)ascii);
            else if (ascii == 8 || ascii == 127) handle_keypress('B');
            else if (ascii == 13 || ascii == 10) handle_keypress('=');
        }
        mouse_ev_t m;
        while (poll_mouse(&m)) {
            if (m.buttons & 1) {
                int pos[2]; mouse_pos(pos);
                click_at(pos[0] - (int)s_info.win_x, pos[1] - (int)s_info.win_y);
            }
        }
        draw_calc();
        wm_flip(s_wid);
        sleep(20);
    }
}

int main_entry(int argc, char **argv, char **envp) {
    (void)argc; (void)argv; (void)envp;
    return calc_main();
}
