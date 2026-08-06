/* Yart OS - /bin/settings: the FIRST REAL APP.
 *
 * A real ring-3 process with its own window surface (SYS_WM_CREATE): it
 * draws its UI into a kernel-managed canvas that the compositor blits on
 * screen, receives its own copies of mouse/keyboard events while focused,
 * and writes /home/yart/cursor.conf - the compositor picks the change up
 * within a second and swaps the cursor theme (classic procedural vs. the
 * real photo cursors rendered from web art).
 */
#include "sys.h"
#include "gfx.h"
#include "cursors.h"

#define WIN_W 430
#define WIN_H 300
#define TITLE_H 24

typedef struct { const char *name; const char *cfg; int kind; } theme_row_t;

static surface_t G_surf;              /* wraps the mapped canvas */
static wm_surf_info_t G_info;
static int G_sel;                     /* selected theme row */

static theme_row_t G_rows[CURSOR_THEME_COUNT + 1];
static int G_row_n;

static void put_dec(long v) {
    char tmp[24]; int i = 0;
    if (v == 0) tmp[i++] = '0';
    while (v > 0) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
    char out[24]; int j = 0;
    while (i) out[j++] = tmp[--i];
    out[j] = 0;
    klog(out);
}

/* simple procedural arrow for the "classic" preview */
static void draw_classic_arrow(surface_t *s, int cx, int cy, u32 c) {
    for (int i = 0; i < 12; i++) {
        int w = 3 + i / 3;
        for (int j = 0; j < w; j++)
            sf_putpx(s, cx + i, cy + j, c);
    }
    for (int i = 0; i < 7; i++)
        sf_putpx(s, cx + 6 + i, cy + 12 + i, c);
}

static void draw_ui(void) {
    surface_t *s = &G_surf;
    /* body.  NOTE: no title bar here - the COMPOSITOR draws the window
     * chrome (title + close X) above this surface; drawing a second one
     * inside the client area looked like a duplicated window frame. */
    sf_fill(s, RGB(0x14, 0x16, 0x1B));

    sf_text(s, 16, 14, "Cursor theme", RGB(0xEC, 0xEE, 0xF1));
    sf_hline(s, 16, 34, WIN_W - 32, RGB(0x2A, 0x2E, 0x36));

    /* theme rows */
    int y = 48;
    for (int i = 0; i < G_row_n; i++) {
        int ry = y + i * 52;
        u32 bg = (i == G_sel) ? RGB(0x24, 0x2E, 0x3E) : RGB(0x1A, 0x1D, 0x24);
        u32 bd = (i == G_sel) ? RGB(0x5B, 0xA7, 0xDF) : RGB(0x2E, 0x32, 0x3A);
        sf_round_rect(s, 16, ry, WIN_W - 32, 46, 8, bg);
        sf_round_rect(s, 16, ry, WIN_W - 32, 46, 8, bd);
        /* preview box 40x40 */
        sf_fill_rect(s, 26, ry + 3, 40, 40, RGB(0x0E, 0x10, 0x14));
        sf_rect_outline(s, 26, ry + 3, 40, 40, RGB(0x2E, 0x32, 0x3A));
        if (G_rows[i].kind == 0) {
            draw_classic_arrow(s, 30, ry + 7, RGB(0xEC, 0xEE, 0xF1));
        } else {
            int t = G_rows[i].kind - 1;
            cursor_theme_t *th = cursors_theme(t);
            if (th && th->img[CURSOR_KIND_ARROW].present) {
                cursor_img_t *im = &th->img[CURSOR_KIND_ARROW];
                surface_t spr;
                spr.px = (u32 *)im->px; spr.w = im->w; spr.h = im->h;
                spr.pitch = im->w;
                sf_blit(s, 30, ry + 7, &spr, 0, 0, im->w, im->h);
            }
        }
        sf_text(s, 78, ry + 15, G_rows[i].name, RGB(0xEC, 0xEE, 0xF1));
        if (i == G_sel) {
            sf_text(s, WIN_W - 46, ry + 15, "*", RGB(0x5B, 0xA7, 0xDF));
        }
    }
    sf_text(s, 16, WIN_H - 24, "Click a theme to apply. Esc or X closes.",
            RGB(0x88, 0x8D, 0x96));
}

static void write_config(void) {
    /* O_WRONLY(1) | O_CREAT(0x40) | O_TRUNC(0x200) */
    int fd = open("/home/yart/cursor.conf", 0x241);
    if (fd < 0) { klog("settings: cannot open cursor.conf\n"); return; }
    const char *cfg = G_rows[G_sel].cfg;
    write(fd, "theme=", 6);
    write(fd, cfg, strlen(cfg));
    write(fd, "\n", 1);
    close(fd);
    klog("settings: cursor theme -> ");
    klog(cfg);
    klog("\n");
}

static int hit_row(int y) {
    int ry = 48;                     /* content starts at y=48 now (no
                                        client-area title bar) */
    for (int i = 0; i < G_row_n; i++) {
        if (y >= ry + i * 52 && y < ry + i * 52 + 46) return i;
    }
    return -1;
}

int main_entry(int argc, char **argv, char **envp) {
    (void)argc; (void)argv;
    /* Test mode: when launched with YART_TEST_EXIT=1 (from the boot
     * suite), create the window, draw + flip once, then close cleanly and
     * exit 0 - an end-to-end window-surface round trip without a user. */
    int test_exit = 0;
    if (envp) {
        for (int i = 0; envp[i]; i++) {
            const char *e = envp[i];
            if (e[0]=='Y'&&e[1]=='A'&&e[2]=='R'&&e[3]=='T'&&e[4]=='_'&&
                e[5]=='T'&&e[6]=='E'&&e[7]=='S'&&e[8]=='T'&&e[9]=='_'&&
                e[10]=='E'&&e[11]=='X'&&e[12]=='I'&&e[13]=='T'&&e[14]=='='&&
                e[15]=='1') test_exit = 1;
        }
    }

    if (cursors_init() <= 0) {
        klog("settings: no cursor themes loaded\n");
    }
    /* theme rows: classic + every photo theme */
    G_row_n = 0;
    G_rows[G_row_n].name = "Classic (built-in)";
    G_rows[G_row_n].cfg  = "classic";
    G_rows[G_row_n].kind = 0;
    G_row_n++;
    for (int t = 0; t < cursors_theme_count() && G_row_n < 6; t++) {
        const char *n = cursors_theme_name(t);
        if (!n) break;
        G_rows[G_row_n].name = n;         /* "photo-white", ... */
        G_rows[G_row_n].cfg  = n;
        G_rows[G_row_n].kind = t + 1;
        G_row_n++;
    }
    G_sel = 0;
    /* select the row matching the CURRENT config (the wm polls this file,
     * so the app must show what is actually active, not always row 0) */
    {
        int fd = open("/home/yart/cursor.conf", 0);      /* O_RDONLY */
        if (fd >= 0) {
            char cfg[64];
            long n = read(fd, cfg, (long)sizeof cfg - 1);
            close(fd);
            if (n > 0) {
                cfg[n] = 0;
                char *p = cfg;
                while (*p) {
                    if (p[0]=='t'&&p[1]=='h'&&p[2]=='e'&&p[3]=='m'&&p[4]=='e'&&p[5]=='=') {
                        char name[24];
                        int i = 0;
                        p += 6;
                        while (*p && *p!='\n' && *p!='\r' && i<23) name[i++] = *p++;
                        name[i] = 0;
                        for (int r = 0; r < G_row_n; r++)
                            if (strcmp(G_rows[r].cfg, name) == 0) { G_sel = r; break; }
                        break;
                    }
                    p++;
                }
            }
        }
    }

    long id = wm_create(WIN_W, WIN_H, &G_info);
    if (id < 0) {
        klog("settings: wm_create failed\n");
        return 1;
    }
    G_surf.px = (u32 *)(unsigned long)G_info.app_va;
    G_surf.w = (int)G_info.w;
    G_surf.h = (int)G_info.h;
    G_surf.pitch = (int)G_info.w;

    wm_title((unsigned)id, "Settings");
    draw_ui();
    wm_flip((unsigned)id);
    klog("settings: window up (");
    put_dec((long)G_info.w);
    klog("x");
    put_dec((long)G_info.h);
    klog(")\n");

    if (test_exit) {
        sleep(500);                 /* let the compositor scan + draw it */
        wm_destroy((unsigned)id);
        klog("settings: test mode - window round trip OK, exiting\n");
        return 0;
    }

    /* POLL_MOUSE delivers DELTAS; the app tracks its own absolute
     * position (seeded at the window center - where the compositor puts
     * the window) and converts to window-local coordinates. */
    int mx = (int)(G_info.win_x + G_info.w / 2);
    int my = (int)(G_info.win_y + G_info.h / 2);
    unsigned char mb = 0;

    int running = 1;
    while (running) {
        mouse_ev_t m;
        while (poll_mouse(&m)) {
            mx += m.dx; my += m.dy;
            if (mx < 0) mx = 0;
            if (my < 0) my = 0;
            int ly = my - (int)G_info.win_y;
            unsigned char prev = mb;
            mb = m.buttons;
            if ((mb & 1) && !(prev & 1)) {
                /* click: theme row (the title-bar X is the compositor's -
                 * clicks there never reach this surface) */
                int row = hit_row(ly);
                if (row >= 0 && row != G_sel) {
                    G_sel = row;
                    draw_ui();
                    wm_flip((unsigned)id);
                    write_config();
                }
            }
        }
        /* keyboard: 1..9 select, Esc closes */
        int ev;
        while ((ev = poll_key()) != 0) {
            int ascii = ev & 0xFF;
            int make = !(ev & 0x80);
            if (!make) continue;
            if (ascii >= '1' && ascii <= '9') {
                int i = ascii - '1';
                if (i < G_row_n) { G_sel = i; draw_ui(); wm_flip((unsigned)id); write_config(); }
            } else if (ascii == 27) {         /* Esc */
                running = 0;
            }
        }
        sleep(30);
    }

    wm_destroy((unsigned)id);
    klog("settings: closed\n");
    return 0;
}
