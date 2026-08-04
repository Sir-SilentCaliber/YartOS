/* Yart OS - ring-3 compositor (wm) — "quartz" 60fps polish pass, v2.
 *
 * Pure ring-3. Kernel exposes FB_INFO, FB_FLIP, POLL_KEY, POLL_MOUSE, TIME_MS,
 * TIME (RTC wall clock).
 *
 * Perf notes:
 *  - Static chrome (panel, dock body+shadow+glass) is cached in offscreen
 *    sprites at startup. Per-frame work is:
 *      1. wallpaper-restore only the small dirty rects (cursor, tooltip,
 *         dock region when tweening, panel strip only when clock changes),
 *      2. blit cached chrome sprites (u64 memcpy),
 *      3. paint dynamic bits: icons (scaled nearest-neighbour), text, cursor.
 *  - fb_flip() is called ONLY when pixels actually changed.
 *  - Tween uses a deadband so it actually CONVERGES (no 1px hunt/oscillate,
 *    which was the source of the "freeze" on TCG: tweening flag stayed true
 *    forever, forcing a full repaint every yield).
 *  - Magnification picks the SINGLE nearest icon within the magnify zone,
 *    not every icon within MAG_RADIUS (which caused both dock items to
 *    bloat at once when hovering the dock midpoint).
 *  - Adaptive 16ms pacing with mid-yield mouse poll for 60Hz cursor feel.
 *    We call sched_yield() in short slices so the cursor stays responsive
 *    even when the scheduler is tick-slow.
 *
 * Visual polish:
 *  - Dock flush to bottom (4px margin), multi-pass soft shadow, frosted glass,
 *    rounded 14px corners, gaussian-bell magnification on the SINGLE hovered
 *    icon, smooth tween animation with deadband, running-indicator dots,
 *    hover pill, fading tooltip with pointer triangle (macOS-style).
 *  - Panel 28px dark glass, top highlight + bottom hairline, custom blue "Y"
 *    logo, workspace dots (active larger), "+", tray icons (clickable),
 *    REAL RTC clock.
 *  - Proper multi-state cursors (arrow/hand/ibeam/wait/forbidden/h-resize/
 *    v-resize) selected per hot-spot region; clean procedurally-drawn 24px
 *    sprites with soft shadow and dark outline.
 */
#include "sys.h"
#include "gfx.h"
#include "kora.h"
#include "cursors.h"

/* =================== real apps (window surfaces) =================== */
#define WM_MAX_WIN 8
typedef struct {
    bool active;        /* the kernel still reports this surface          */
    bool seen;          /* the kernel reported it this scan               */
    u32  id;            /* kernel surface id                             */
    u32  owner;         /* pid of the app                                */
    int  x, y, w, h;    /* screen rect of the surface                     */
    u64  va;            /* wm-side mapping of the surface (from scan)     */
    bool dirty;
    char title[32];     /* window title (set by the app via SYS_WM_TITLE) */
} win_t;
static win_t G_wins[WM_MAX_WIN];
static int  G_app_pid;      /* pid of the launched real app (settings)   */
static int  G_win_dirty;    /* any window needs recompositing            */

/* =================== cursor themes =================== */
static int  G_cur_theme;            /* 0 = classic procedural, 1.. photo */
static char G_theme_name[24];       /* current theme name in config      */
static long G_cfg_last_check;       /* ms of the last cursor.conf poll   */

/* =================== theme =================== */
#define C_PANEL_BG      ARGB(0xD2,0x14,0x16,0x1B)
#define C_PANEL_FG      RGB(0xEC,0xEE,0xF1)
#define C_PANEL_DIM     RGB(0x88,0x8D,0x96)
#define C_PANEL_HAIR    RGB(0x06,0x08,0x0C)
#define C_PANEL_HILITE  ARGB(0x28,0xFF,0xFF,0xFF)
#define C_ACCENT        RGB(0x5B,0xA7,0xDF)
#define C_DOCK_TINT     ARGB(0xBC,0x1A,0x1D,0x24)
#define C_DOCK_BORDER   ARGB(0x6E,0x5C,0x62,0x6C)
#define C_DOCK_INNER    ARGB(0x1E,0xFF,0xFF,0xFF)
#define C_DOCK_INNER2   ARGB(0x0A,0xFF,0xFF,0xFF)
#define C_HOVER         ARGB(0x30,0xFF,0xFF,0xFF)
#define C_PRESS         ARGB(0x50,0xFF,0xFF,0xFF)
#define C_DOT_RUN       ARGB(0xBB,0xC8,0xCB,0xD0)
#define C_DOT_HOVER     C_PANEL_FG
#define TRAY_TINT       C_PANEL_FG

/* =================== layout =================== */
#define PANEL_H     28
#define DOCK_H      58
#define ICON_SZ     34       /* dock icons rest size */
#define DOCK_MW     44       /* slot pitch between icon centers */
#define MAG         8        /* max extra px added on hover (subtle) */
#define MAG_RADIUS  30       /* gaussian bell half-width; only nearest icon wins */
#define DOCK_MARGIN 4
#define DOCK_RAD    14
#define DOCK_PADX   14
#define DOCK_SHADOW 16

/* =================== dock items ===================
 * NO FAKE APPS: only things that actually exist.  "Settings" launches the
 * real ring-3 app (/bin/settings); "Trash" is a real concept (empty). */
typedef struct { const char *name; int icon; } dock_item_t;
static const dock_item_t G_dock[] = {
    { "Settings",   ICON_DOCK_SETTINGS },
    { "Trash",      ICON_DOCK_TRASH    },
};
#define DOCK_N ((int)(sizeof G_dock / sizeof G_dock[0]))

/* =================== cursors =================== */
enum {
    CUR_ARROW = 0,
    CUR_HAND,
    CUR_IBEAM,
    CUR_WAIT,
    CUR_FORBIDDEN,
    CUR_HRESIZE,
    CUR_VRESIZE,
    CUR_N,
};
static int G_cursor_icon[CUR_N] = {
    ICON_CURSOR_ARROW, ICON_CURSOR_HAND, ICON_CURSOR_IBEAM, ICON_CURSOR_WAIT,
    ICON_CURSOR_FORBIDDEN, ICON_CURSOR_HRESIZE, ICON_CURSOR_VRESIZE,
};
/* Hotspots per cursor (tip vs center etc.), in 24px canvas coords. */
static const int G_cursor_hotx[CUR_N] = {2, 12, 11, 2, 11, 11, 11};
static const int G_cursor_hoty[CUR_N] = {2,  3, 11, 2, 11, 11, 11};

/* =================== workspaces =================== */
#define MAX_WS 9
static int G_ws_count = 1;
static int G_ws_active = 0;

/* Pre-scaled dock icons at rest size (34px): while hovering/tweening we
 * blit these instead of per-pixel scaling the 48px atlas icons every
 * frame - the single biggest per-frame cost on TCG. */
static surface_t G_icon_cache[DOCK_N];
static int       G_icon_cache_ok;

static void build_icon_cache(void) {
    for (int i=0;i<DOCK_N;i++) {
        if (G_icon_cache[i].px) sf_free(&G_icon_cache[i]);
        icon_t ico = icon_get(G_dock[i].icon);
        if (!ico.px || !ico.w || !ico.h) continue;
        G_icon_cache[i] = sf_alloc(ICON_SZ, ICON_SZ);
        if (!G_icon_cache[i].px) continue;
        surface_t *c = &G_icon_cache[i];
        sf_icon_scaled(c, ICON_SZ/2, ICON_SZ/2, ico, 0, ICON_SZ, 48);
    }
    G_icon_cache_ok = 1;
}

/* =================== animated tween state =================== */
static int G_slot_size[DOCK_N];
static int G_slot_lift[DOCK_N];
static int G_slot_target_sz[DOCK_N];
static int G_slot_target_lift[DOCK_N];
static int G_slot_bounce[DOCK_N];     /* decaying bounce velocity on click */
static long G_slot_bounce_t0[DOCK_N];

/* =================== cached chrome sprites =================== */
static surface_t G_panel_spr;
static surface_t G_dock_spr;
static int G_dock_spr_x0, G_dock_spr_y0, G_dock_spr_w, G_dock_spr_h;
static int G_dock_x, G_dock_y, G_dock_w, G_dock_h;

/* =================== framebuffer =================== */
static surface_t G_fb;
static surface_t G_wp;
static int G_cx, G_cy;
static unsigned char G_mb;
static int G_net_up, G_audio_up, G_bat_up;
static long G_start_ms;
static int G_full_repaint;
static int G_wp_index = 0;

/* =================== tray hit-boxes =================== */
typedef struct { const char *name; int x0,y0,x1,y1; int icon; } tray_hit_t;
static tray_hit_t G_tray[8];
static int G_tray_n = 0;

/* =================== dirty tracking =================== */
static int G_last_cx = -1, G_last_cy = -1;
static int G_last_cursor = -1;
static int G_panel_dirty = 1;
static int G_dock_dirty  = 1;
static int G_tooltip_dirty = 0;
static int G_tip_x0, G_tip_y0, G_tip_x1, G_tip_y1;
static unsigned long G_last_clock_s = (unsigned long)-1;
static char G_clk_text[16] = "00:00:00";

/* =================== tooltip =================== */
static int G_tooltip_item = -1;
static long G_tooltip_t0 = 0;
#define TOOLTIP_DELAY_MS 450
#define TOOLTIP_FADE_MS  150

/* =================== on-screen display (wallpaper name etc.) ========== */
static long G_osd_t0 = 0;
static char G_osd_text[64];

/* Wallpaper names, indexed in the same order as gen_wallpaper_pack.py */
static const char * const WP_NAMES[] = {
    "Twilight Dunes", "Midnight", "Ocean Breeze", "Graphite"
};
#define WP_NAMES_N ((int)(sizeof WP_NAMES / sizeof WP_NAMES[0]))

/* =================== helpers =================== */
static void format_wallclock(long wt, char *buf) {
    long s = wt % 100; wt /= 100;
    long m = wt % 100; wt /= 100;
    long h = wt % 100;
    buf[0]='0'+h/10; buf[1]='0'+h%10; buf[2]=':';
    buf[3]='0'+m/10; buf[4]='0'+m%10; buf[5]=':';
    buf[6]='0'+s/10; buf[7]='0'+s%10; buf[8]=0;
}

static int abs_int(int x) { return x<0 ? -x : x; }

/* ================================================================
 * Cursor: build + draw.  Each cursor sprite is taken directly from
 * the asset atlas (Kora-style procedural cursors at 24px).  We
 * render them with a soft drop shadow for depth.
 * ================================================================ */
static u32 G_cursor_buf[CUR_N][24*24] __attribute__((aligned(16)));
/* Active cursor sprites: point either at the procedural buffers above or
 * at photo-theme images (classic = procedural, photo themes = real
 * raster art from the web).  `w/h` vary per sprite. */
static surface_t G_cursor_spr[CUR_N];
static int       G_cursor_hotx2[CUR_N];
static int       G_cursor_hoty2[CUR_N];

/* Inline alpha-blend helper for raw u32 buffers (used during cursor build).
 * Matches sf_putpx_blend's math exactly. c and d are ARGB-packed u32s. */
static inline void sf_putpx_blend_at(u32 *buf, int x, int y, int pitch, u32 c) {
    u32 d = buf[y*pitch + x];
    u8 sa = (u8)(c>>24);
    if (sa == 0) return;
    u8 cr=(u8)(c>>16), cg=(u8)(c>>8), cb=(u8)c;
    if (sa == 255) { buf[y*pitch+x] = 0xFF000000 | ((u32)cr<<16) | ((u32)cg<<8) | cb; return; }
    u32 ia = 255 - sa;
    u8 dr=(u8)(d>>16), dg=(u8)(d>>8), db=(u8)d;
    u8 nr=(u8)(((u32)cr*sa + (u32)dr*ia + 127)/255);
    u8 ng=(u8)(((u32)cg*sa + (u32)dg*ia + 127)/255);
    u8 nb=(u8)(((u32)cb*sa + (u32)db*ia + 127)/255);
    buf[y*pitch+x] = 0xFF000000 | ((u32)nr<<16) | ((u32)ng<<8) | nb;
}

static void cursor_build_one(int id) {
    icon_t ico = icon_get(G_cursor_icon[id]);
    u32 *dst = G_cursor_buf[id];
    for (int i=0;i<24*24;i++) dst[i] = 0;
    if (!ico.px) return;
    int sz = ico.w < 24 ? ico.w : 24;
    /* Soft shadow (Gaussian-ish 3x3 kernel, offset +2,+2). We paint into a
     * temporary alpha mask then blur so overlapping shadow edges accumulate
     * correctly rather than the "max alpha" look of the old code. */
    u8 shadow[24*24] = {0};
    for (int j=0;j<sz;j++) for (int i=0;i<sz;i++) {
        u8 sa = (u8)(ico.px[j*ico.pitch+i]>>24);
        if (sa < 8) continue;
        int base = (sa*140)/255;
        for (int dy=-1;dy<=2;dy++) for (int dx=-1;dx<=2;dx++) {
            int nx=i+dx+2, ny=j+dy+2;
            if ((u32)nx>=24||(u32)ny>=24) continue;
            int d2=dx*dx+dy*dy;
            int a = base;
            if (d2<=1) a = base;
            else if (d2<=3) a = (base*60)/100;
            else if (d2<=5) a = (base*28)/100;
            else if (d2<=8) a = (base*12)/100;
            else continue;
            int idx=ny*24+nx;
            int na = shadow[idx] + a;
            shadow[idx] = (u8)(na>255?255:na);
        }
    }
    for (int i=0;i<24*24;i++) {
        if (shadow[i]) dst[i] = ARGB(shadow[i], 0,0,0);
    }
    /* Body: blit sprite at (0,0) using native alpha OVER the shadow */
    for (int j=0;j<sz;j++) for (int i=0;i<sz;i++) {
        u32 sp = ico.px[j*ico.pitch+i];
        u8 sa=(u8)(sp>>24);
        if (!sa) continue;
        u8 sr=(u8)sp, sg=(u8)(sp>>8), sb=(u8)(sp>>16);
        /* Alpha blend over whatever's already there (shadow/outline) */
        sf_putpx_blend_at(dst, i, j, 24, ARGB(sa,sr,sg,sb));
    }
}
static void cursor_build_all(void) {
    for (int i=0;i<CUR_N;i++) cursor_build_one(i);
    /* default theme: classic procedural sprites */
    for (int i=0;i<CUR_N;i++) {
        G_cursor_spr[i].px = G_cursor_buf[i];
        G_cursor_spr[i].w = 24; G_cursor_spr[i].h = 24;
        G_cursor_spr[i].pitch = 24;
        G_cursor_hotx2[i] = G_cursor_hotx[i];
        G_cursor_hoty2[i] = G_cursor_hoty[i];
    }
}

/* Switch the cursor theme: 0 = classic procedural; t >= 1 = photo theme
 * (t-1 indexes the blob).  Photo themes replace the arrow + hand with the
 * real raster art; all other cursor kinds keep the procedural sprites. */
static void cursor_apply_theme(int t) {
    int n = cursors_theme_count();
    if (t < 0 || t > n) return;
    G_cur_theme = t;
    if (t == 0) {
        for (int i=0;i<CUR_N;i++) {
            G_cursor_spr[i].px = G_cursor_buf[i];
            G_cursor_spr[i].w = 24; G_cursor_spr[i].h = 24;
            G_cursor_spr[i].pitch = 24;
            G_cursor_hotx2[i] = G_cursor_hotx[i];
            G_cursor_hoty2[i] = G_cursor_hoty[i];
        }
        return;
    }
    cursor_theme_t *th = cursors_theme(t - 1);
    if (!th) return;
    /* map cursor kinds to photo kinds: ARROW, HAND (others keep procedural) */
    int kind_of[CUR_N] = { CURSOR_KIND_ARROW, CURSOR_KIND_HAND,
                           -1, -1, -1, -1, -1 };
    for (int i=0;i<CUR_N;i++) {
        if (kind_of[i] >= 0 && th->img[kind_of[i]].present) {
            cursor_img_t *im = &th->img[kind_of[i]];
            G_cursor_spr[i].px = (u32 *)im->px;
            G_cursor_spr[i].w = im->w; G_cursor_spr[i].h = im->h;
            G_cursor_spr[i].pitch = im->w;
            G_cursor_hotx2[i] = im->hotx;
            G_cursor_hoty2[i] = im->hoty;
        } else {
            G_cursor_spr[i].px = G_cursor_buf[i];
            G_cursor_spr[i].w = 24; G_cursor_spr[i].h = 24;
            G_cursor_spr[i].pitch = 24;
            G_cursor_hotx2[i] = G_cursor_hotx[i];
            G_cursor_hoty2[i] = G_cursor_hoty[i];
        }
    }
}

static void cursor_draw(surface_t *s, int id, int x, int y) {
    if (id<0||id>=CUR_N) id=0;
    surface_t *sp = &G_cursor_spr[id];
    if (!sp->px || sp->w <= 0 || sp->h <= 0) return;
    int x0 = x - G_cursor_hotx2[id], y0 = y - G_cursor_hoty2[id];
    for (int j=0;j<sp->h;j++) {
        int py = y0+j;
        if ((u32)py >= (u32)s->h) continue;
        for (int i=0;i<sp->w;i++) {
            u32 c = sp->px[j*sp->pitch+i];
            if (!A(c)) continue;
            int px = x0+i;
            if ((u32)px >= (u32)s->w) continue;
            sf_putpx_blend(s, px, py, c);
        }
    }
}

/* ================================================================
 * Restore helpers (blit wallpaper back into dirty rects)
 * ================================================================ */
static void restore_rect(surface_t *fb, int x0, int y0, int w, int h) {
    if (x0<0) x0=0;
    if (y0<0) y0=0;
    if (x0+w>fb->w) w=fb->w-x0;
    if (y0+h>fb->h) h=fb->h-y0;
    if (w<=0||h<=0) return;
    sf_blit(fb, x0, y0, &G_wp, x0, y0, w, h);
}
static void draw_one_window(win_t *w);   /* defined later */

/* Wallpaper-restore that re-blits any app window intersecting the rect
 * (otherwise moving the cursor over a window would punch wallpaper holes
 * through it). */
static void restore_rect_win(surface_t *fb, int x0, int y0, int w, int h) {
    restore_rect(fb, x0, y0, w, h);
    for (int i=0;i<WM_MAX_WIN;i++) {
        win_t *win = &G_wins[i];
        if (!win->active || !win->va) continue;
        int ty = win->y - 24;
        if (ty < PANEL_H) ty = PANEL_H;
        if (x0 < win->x + win->w && x0 + w > win->x &&
            y0 < ty + 24 + win->h && y0 + h > ty)
            draw_one_window(win);
    }
}

static void restore_cursor(surface_t *fb) {
    if (G_last_cx < 0) return;
    /* 44x44 covers both the 24px procedural and the 32px photo cursors */
    restore_rect_win(fb, G_last_cx-14, G_last_cy-14, 44, 44);
}
static void restore_tooltip(surface_t *fb) {
    if (!G_tooltip_dirty) return;
    restore_rect(fb, G_tip_x0, G_tip_y0, G_tip_x1-G_tip_x0, G_tip_y1-G_tip_y0);
    G_tooltip_dirty = 0;
}
static void restore_dock_region(surface_t *fb) {
    if (!G_dock_dirty) return;
    restore_rect(fb, G_dock_spr_x0, G_dock_spr_y0, G_dock_spr_w, G_dock_spr_h);
    G_dock_dirty = 0;
}
static void restore_panel(surface_t *fb) {
    if (!G_panel_dirty) return;
    restore_rect(fb, 0, 0, fb->w, PANEL_H+2);
    G_panel_dirty = 0;
}

/* ================================================================
 * Chrome renderers (called once at startup / resolution change)
 * ================================================================ */
static void draw_logo(surface_t *s, int x, int y) {
    u32 fg = C_ACCENT;
    int pts[][2] = {
        {1,2},{2,2},{5,2},{6,2},
        {2,3},{3,3},{4,3},{5,3},
        {3,4},{4,4},{3,5},{4,5},{3,6},{4,6},{3,7},{4,7},{3,8},{4,8},{3,9},{4,9},
    };
    for (unsigned i=0; i<sizeof(pts)/sizeof(pts[0]); i++)
        sf_putpx(s, x+pts[i][0], y+pts[i][1], fg);
}

static void build_panel_sprite(void) {
    int w = G_fb.w, h = PANEL_H+2;
    if (G_panel_spr.px) sf_free(&G_panel_spr);
    G_panel_spr = sf_alloc(w, h);
    sf_blit(&G_panel_spr, 0, 0, &G_wp, 0, 0, w, h);
    sf_fill_rect_blend(&G_panel_spr, 0, 0, w, PANEL_H, C_PANEL_BG);
    sf_hline(&G_panel_spr, 0, 0, w, C_PANEL_HILITE);
    sf_hline(&G_panel_spr, 0, PANEL_H, w, C_PANEL_HAIR);
    draw_logo(&G_panel_spr, 12, 6);
    sf_text(&G_panel_spr, 24, 6, "Yart", C_PANEL_FG);
}

static void build_dock_sprite(void) {
    int content_w = DOCK_N*DOCK_MW + 8;
    int dock_w = content_w + DOCK_PADX*2;
    int dock_h = DOCK_H;
    int dock_x = G_fb.w/2 - dock_w/2;
    int dock_y = G_fb.h - dock_h - DOCK_MARGIN;
    int dock_r = DOCK_RAD;
    G_dock_x = dock_x; G_dock_y = dock_y; G_dock_w = dock_w; G_dock_h = dock_h;

    int spr_w = dock_w + DOCK_SHADOW*2 + 4;
    int spr_h = dock_h + DOCK_SHADOW*2 + 6;
    int spr_x = dock_x - DOCK_SHADOW - 2;
    int spr_y = dock_y - DOCK_SHADOW - 2;
    G_dock_spr_x0 = spr_x; G_dock_spr_y0 = spr_y;
    G_dock_spr_w = spr_w; G_dock_spr_h = spr_h;
    if (G_dock_spr.px) sf_free(&G_dock_spr);
    G_dock_spr = sf_alloc(spr_w, spr_h);

    int wsx = spr_x, wsy = spr_y, wsw = spr_w, wsh = spr_h;
    if (wsx < 0) { wsw += wsx; wsx = 0; }
    if (wsy < 0) { wsh += wsy; wsy = 0; }
    if (wsx + wsw > G_wp.w) wsw = G_wp.w - wsx;
    if (wsy + wsh > G_wp.h) wsh = G_wp.h - wsy;
    if (wsw > 0 && wsh > 0) sf_blit(&G_dock_spr, wsx - spr_x, wsy - spr_y, &G_wp, wsx, wsy, wsw, wsh);

    int lx = DOCK_SHADOW+2, ly = DOCK_SHADOW+2;
    for (int pass=0; pass<5; pass++) {
        int e = 5-pass;
        int a = (int[]){6,12,20,28,36}[pass];
        sf_round_rect_blend(&G_dock_spr, lx-e, ly-e+3, dock_w+2*e, dock_h+2*e+2,
                            dock_r+e, ARGB(a,0,0,0));
    }
    sf_round_rect_blend(&G_dock_spr, lx, ly, dock_w, dock_h, dock_r, C_DOCK_TINT);
    sf_round_rect_blend(&G_dock_spr, lx, ly, dock_w, dock_h*2/5, dock_r, C_DOCK_INNER);
    sf_fill_rect_blend(&G_dock_spr, lx+dock_r, ly+dock_h*2/5,
                       dock_w-2*dock_r, dock_h*2/5, C_DOCK_INNER2);
    int x0 = lx+dock_r, x1 = lx+dock_w-dock_r;
    for (int x=x0; x<x1; x++) sf_putpx_blend(&G_dock_spr, x, ly+1, C_DOCK_INNER);
    for (int dy=0; dy<dock_r; dy++) for (int dx=0; dx<dock_r; dx++) {
        int rr=dx*dx+dy*dy, ro=dock_r*dock_r, ri=(dock_r-2)*(dock_r-2);
        if (rr<=ro && rr>=ri && dy<=3) {
            sf_putpx_blend(&G_dock_spr, lx+dock_r-1-dx, ly+dock_r-1-dy, C_DOCK_INNER);
            sf_putpx_blend(&G_dock_spr, lx+dock_w-dock_r+dx, ly+dock_r-1-dy, C_DOCK_INNER);
        }
    }
    sf_round_rect_blend(&G_dock_spr, lx, ly, dock_w, dock_h, dock_r, C_DOCK_BORDER);
}

/* ================================================================
 * Panel content (text/dots/tray/clock) — drawn per frame over cached bg.
 * Returns the rightmost x it painted so the caller can track tray hits.
 * ================================================================ */
static int draw_panel_content(surface_t *s, long now_ms) {
    (void)now_ms;
    int wx = 24 + sf_text_width("Yart") + 20;
    int cy = PANEL_H/2;
    for (int i=0;i<G_ws_count;i++) {
        int active=(i==G_ws_active);
        u32 c = active?C_PANEL_FG:C_PANEL_DIM;
        int r = active?3:2;
        for (int dy=-r;dy<=r;dy++) for (int dx=-r;dx<=r;dx++)
            if (dx*dx+dy*dy <= r*r) sf_putpx(s, wx+dx+i*12, cy+dy, c);
    }
    int plus_x = wx + G_ws_count*12 + 6;
    {
        u32 c = C_PANEL_DIM;
        sf_putpx(s, plus_x,cy,c); sf_putpx(s,plus_x-1,cy,c); sf_putpx(s,plus_x+1,cy,c);
        sf_putpx(s, plus_x,cy-1,c); sf_putpx(s,plus_x,cy+1,c);
    }

    G_tray_n = 0;
    int rx = s->w-12;
    int cw = sf_text_width(G_clk_text);
    /* Clock isn't in the tray hit list (it's just a label) */
    sf_text(s, rx-cw, 6, G_clk_text, C_PANEL_FG);
    rx -= cw+12;

    int ty = (PANEL_H-18)/2;
    icon_t ico;
    ico = icon_get(ICON_TRAY_BATTERY);
    if (ico.px && rx-ico.w > 80) {
        int ix = rx-ico.w;
        G_tray[G_tray_n++] = (tray_hit_t){"battery", ix, ty, ix+ico.w, ty+ico.h, ICON_TRAY_BATTERY};
        sf_icon_tl(s, ix, ty, ico, TRAY_TINT); rx -= ico.w+6;
    }
    ico = icon_get(G_net_up?ICON_TRAY_NET_WIRED:ICON_TRAY_NET_IDLE);
    if (ico.px && rx-ico.w > 80) {
        int ix = rx-ico.w;
        G_tray[G_tray_n++] = (tray_hit_t){"network", ix, ty, ix+ico.w, ty+ico.h,
                                          G_net_up?ICON_TRAY_NET_WIRED:ICON_TRAY_NET_IDLE};
        sf_icon_tl(s, ix, ty, ico, TRAY_TINT); rx -= ico.w+6;
    }
    ico = icon_get(G_audio_up?ICON_TRAY_AUDIO_HI:ICON_TRAY_AUDIO_MUTE);
    if (ico.px && rx-ico.w > 80) {
        int ix = rx-ico.w;
        G_tray[G_tray_n++] = (tray_hit_t){"audio", ix, ty, ix+ico.w, ty+ico.h,
                                          G_audio_up?ICON_TRAY_AUDIO_HI:ICON_TRAY_AUDIO_MUTE};
        sf_icon_tl(s, ix, ty, ico, TRAY_TINT); rx -= ico.w+6;
    }
    return rx;
}

/* ================================================================
 * Tooltip
 * ================================================================ */
static void draw_tooltip(surface_t *s, int item, long now_ms) {
    if (item<0||item>=DOCK_N) return;
    long dt = now_ms - G_tooltip_t0;
    if (dt < TOOLTIP_DELAY_MS) return;
    int a=235;
    if (dt < TOOLTIP_DELAY_MS+TOOLTIP_FADE_MS) {
        a = (int)(235*(dt-TOOLTIP_DELAY_MS)/TOOLTIP_FADE_MS);
        if (a<0) a=0;
        if (a>235) a=235;
    }
    const char *txt = G_dock[item].name;
    int tw = sf_text_width(txt), padx=10, pady=4;
    int w = tw+padx*2, h = 16+pady*2;
    int start_x = G_dock_x + G_dock_w/2 - (DOCK_N-1)*DOCK_MW/2;
    int slot_cx = start_x + item*DOCK_MW;
    int tx = slot_cx - w/2;
    int ty = G_dock_y - h - 8;
    if (tx<4) tx=4;
    if (tx+w>s->w-4) tx=s->w-4-w;
    G_tip_x0=tx-2; G_tip_y0=ty-2; G_tip_x1=tx+w+2; G_tip_y1=ty+h+2;
    u32 bg = ARGB(a,0x24,0x27,0x2E);
    u32 bd = ARGB(a*200/235,0x6B,0x71,0x7B);
    u32 fg = ARGB(a,0xFF,0xFF,0xFF);
    sf_round_rect_blend(s, tx-1, ty-1, w+2, h+2, 6, ARGB(a/3,0,0,0));
    sf_round_rect_blend(s, tx, ty, w, h, 5, bg);
    sf_round_rect_blend(s, tx, ty, w, h, 5, bd);
    sf_text_blend(s, tx+padx, ty+pady, txt, fg);
    sf_putpx_blend(s, slot_cx,   ty+h,   bg);
    sf_putpx_blend(s, slot_cx-1, ty+h,   bg);
    sf_putpx_blend(s, slot_cx+1, ty+h,   bg);
    sf_putpx_blend(s, slot_cx,   ty+h-1, bg);
    G_tooltip_dirty = 1;
}

/* ================================================================
 * Dock: blit cached sprite, draw icons + dots.
 *
 * Magnification picks the single NEAREST icon to the mouse (within
 * MAG_RADIUS horizontally and within the dock+shadow band vertically)
 * and applies a gaussian-bell scale to that icon alone. Neighbors
 * stay at rest size unless they also fall into the bell, which only
 * happens for very tight spacing - with DOCK_MW=40 and MAG_RADIUS=28
 * only the hovered icon grows, which is the macOS-like behavior
 * users expect.
 * ================================================================ */
static void draw_dock_content(surface_t *s, long now_ms, int pressed) {
    int mx=G_cx, my=G_cy;
    int slot_pos[DOCK_N];
    int start_x = G_dock_x + G_dock_w/2 - (DOCK_N-1)*DOCK_MW/2;
    int over_dock = (my >= G_dock_y-24 && my <= G_dock_y+G_dock_h+10);
    /* Find nearest icon horizontally */
    int hover = -1;
    int best_d = 0x7FFFFFFF;
    for (int i=0;i<DOCK_N;i++) {
        slot_pos[i] = start_x + i*DOCK_MW;
        int d = abs_int(slot_pos[i]-mx);
        if (over_dock && d < best_d) { best_d = d; hover = i; }
    }
    /* Only magnify if within radius (otherwise hover stays -1) */
    if (best_d > MAG_RADIUS) hover = -1;

    for (int i=0;i<DOCK_N;i++) {
        if (i==hover) {
            int t = (best_d*1024)/MAG_RADIUS;
            int w = 1024 - (t*t)/1024;
            int m = (MAG*w)/1024;
            G_slot_target_sz[i] = ICON_SZ + m*2;
            G_slot_target_lift[i] = m*3/4;
        } else {
            G_slot_target_sz[i] = ICON_SZ;
            G_slot_target_lift[i] = 0;
        }
    }
    if (hover>=0) {
        if (G_tooltip_item!=hover) { G_tooltip_item=hover; G_tooltip_t0=now_ms; G_tooltip_dirty=1; }
    } else {
        if (G_tooltip_item!=-1) { G_tooltip_dirty=1; G_tooltip_item=-1; G_tip_x0=G_tip_y0=G_tip_x1=G_tip_y1=0; }
    }

    /* Smooth tween toward targets. IMPORTANT: snap when within 1px to
     * avoid 1px oscillation (the old code did +1/-1 every frame when
     * ds==±1, which kept tweening==true forever and caused TCG freezes).
     * Bounce physics: when G_slot_bounce[i] is non-zero we decay it toward
     * zero and add it to the target lift so clicked icons spring up and
     * settle like macOS. */
    int tweening = 0;
    for (int i=0;i<DOCK_N;i++) {
        /* decay bounce */
        if (G_slot_bounce[i] != 0) {
            G_slot_bounce[i] = (G_slot_bounce[i] * 7) / 10;
            if (abs_int(G_slot_bounce[i]) < 2) G_slot_bounce[i] = 0;
        }
        int target_lift = G_slot_target_lift[i] + G_slot_bounce[i];
        int ds = G_slot_target_sz[i] - G_slot_size[i];
        int dl = target_lift - G_slot_lift[i];
        if (abs_int(ds) <= 1) G_slot_size[i] = G_slot_target_sz[i];
        else                  G_slot_size[i] += ds/3 + (ds>0?1:-1);
        if (abs_int(dl) <= 1) G_slot_lift[i] = target_lift;
        else                  G_slot_lift[i] += dl/3 + (dl>0?1:-1);
        if (G_slot_size[i] != G_slot_target_sz[i] ||
            G_slot_lift[i] != target_lift ||
            G_slot_bounce[i] != 0) tweening = 1;
        if (G_slot_size[i] < ICON_SZ)    G_slot_size[i] = ICON_SZ;
        if (G_slot_size[i] > ICON_SZ+MAG*2) G_slot_size[i] = ICON_SZ+MAG*2;
    }
    (void)tweening;

    /* Running dots */
    for (int i=0;i<DOCK_N;i++) {
        int cx=slot_pos[i], cy=G_dock_y+G_dock_h-4;
        u32 c = (i==hover)?C_DOT_HOVER:C_DOT_RUN;
        sf_putpx(s,cx,cy,c); sf_putpx(s,cx-1,cy,c); sf_putpx(s,cx+1,cy,c);
    }

    /* Hover pill (only under hovered icon; press = brighter) */
    if (hover>=0) {
        int cx = slot_pos[hover];
        sf_round_rect_blend(s, cx-DOCK_MW/2+4, G_dock_y+4,
                            DOCK_MW-8, G_dock_h-8, 8,
                            pressed ? C_PRESS : C_HOVER);
    }

    /* Icons: rest size -> cached sprite blit; magnified -> per-pixel
     * scale (only the hovered icon ever magnifies) */
    for (int i=0;i<DOCK_N;i++) {
        int cx=slot_pos[i], cy=G_dock_y+G_dock_h/2-1-G_slot_lift[i];
        int sc=G_slot_size[i];
        if (sc == ICON_SZ && G_icon_cache_ok && G_icon_cache[i].px) {
            sf_blit(s, cx-ICON_SZ/2, cy-ICON_SZ/2, &G_icon_cache[i],
                    0, 0, ICON_SZ, ICON_SZ);
            continue;
        }
        icon_t ico = icon_get(G_dock[i].icon);
        if (!ico.px) continue;
        sf_icon_scaled(s, cx, cy, ico, 0, sc, ICON_SZ);
    }
    if (G_tooltip_item>=0) draw_tooltip(s, G_tooltip_item, now_ms);
}

static void draw_osd(surface_t *s, long now) {
    long dt = now - G_osd_t0;
    if (dt < 0) return;
    if (dt > 1800) return;
    int a;
    if (dt < 200)       a = (int)(220*dt/200);          /* fade in */
    else if (dt < 1300) a = 220;                         /* hold */
    else                a = (int)(220*(1800-dt)/500);    /* fade out */
    if (a<0) a=0;
    if (a>255) a=255;
    int tw = sf_text_width(G_osd_text);
    int padx=18, pady=8;
    int w=tw+padx*2, h=16+pady*2;
    int x = s->w/2 - w/2;
    int y = s->h - DOCK_H - DOCK_MARGIN - h - 30;
    if (y < PANEL_H+10) y = PANEL_H+10;
    u32 bg  = ARGB((u8)(a*200/220), 0x1E,0x21,0x28);
    u32 bd  = ARGB((u8)(a*150/220), 0x6B,0x71,0x7B);
    u32 fg  = ARGB((u8)a, 0xFF,0xFF,0xFF);
    sf_round_rect_blend(s, x-2, y-2, w+4, h+4, 8, ARGB(a/3,0,0,0));
    sf_round_rect_blend(s, x, y, w, h, 7, bg);
    sf_round_rect_blend(s, x, y, w, h, 7, bd);
    sf_text_blend(s, x+padx, y+pady, G_osd_text, fg);
}

/* ================================================================
 * Cursor selection per region
 * ================================================================ */
static int cursor_pick(int x, int y) {
    /* App window title bars -> hand */
    for (int i=0;i<WM_MAX_WIN;i++) {
        win_t *w = &G_wins[i];
        if (!w->active) continue;
        int ty = w->y - 24;
        if (ty < PANEL_H) ty = PANEL_H;
        if (x >= w->x && x < w->x + w->w && y >= ty && y < ty + 24)
            return CUR_HAND;
    }
    /* Panel tray icons -> hand */
    for (int i=0;i<G_tray_n;i++) {
        if (x>=G_tray[i].x0 && x<G_tray[i].x1 && y>=G_tray[i].y0 && y<G_tray[i].y1)
            return CUR_HAND;
    }
    /* Workspace dots / plus */
    if (y < PANEL_H) {
        int wx = 24 + sf_text_width("Yart") + 20;
        if (x >= wx-4 && x < wx + MAX_WS*12 + 18) return CUR_HAND;
    }
    /* Dock icons -> hand */
    if (y >= G_dock_y-8 && y <= G_dock_y+G_dock_h) {
        int start_x = G_dock_x + G_dock_w/2 - (DOCK_N-1)*DOCK_MW/2;
        for (int i=0;i<DOCK_N;i++) {
            int cx = start_x + i*DOCK_MW;
            if (abs_int(x-cx) < DOCK_MW/2) return CUR_HAND;
        }
    }
    return CUR_ARROW;
}

/* ================================================================
 * Click handling
 * ================================================================ */
static void launch_settings(void);   /* defined below */
static void handle_click(int x, int y, int button) {
    if (button != 1) return;
    /* App windows: title bar (close / refocus) and surface (focus).  A
     * click anywhere else drops the input focus back to the wm. */
    for (int i=0;i<WM_MAX_WIN;i++) {
        win_t *w = &G_wins[i];
        if (!w->active) continue;
        int ty = w->y - 24;
        if (ty < PANEL_H) ty = PANEL_H;
        if (x >= w->x && x < w->x + w->w) {
            if (y >= ty && y < ty + 24) {
                /* title bar: close button or focus */
                int cx = w->x + w->w - 18, cy = ty + 12;
                if ((x-cx)*(x-cx) + (y-cy)*(y-cy) <= 81) {
                    /* close = destroy the surface AND kill the app (the
                     * app keeps running invisible otherwise, and clicking
                     * the dock icon again would refocus a windowless
                     * ghost that never relaunches) */
                    wm_destroy(w->id);
                    kill((long)w->owner);
                    if (G_app_pid == (int)w->owner) G_app_pid = 0;
                    klog("wm: close button clicked\n");
                } else {
                    wm_focus(w->owner);
                }
                return;
            }
            if (y >= ty + 24 && y < ty + 24 + w->h) {
                wm_focus(w->owner);
                return;
            }
        }
    }
    /* clicking outside any window releases input focus */
    {
        bool inside = false;
        for (int i=0;i<WM_MAX_WIN;i++) {
            win_t *w = &G_wins[i];
            if (!w->active) continue;
            int ty = w->y - 24;
            if (ty < PANEL_H) ty = PANEL_H;
            if (x >= w->x && x < w->x + w->w &&
                y >= ty && y < ty + 24 + w->h) { inside = true; break; }
        }
        if (!inside) wm_focus(0);
    }
    /* Tray */
    for (int i=0;i<G_tray_n;i++) {
        if (x>=G_tray[i].x0 && x<G_tray[i].x1 && y>=G_tray[i].y0 && y<G_tray[i].y1) {
            /* For now: toggle audio / visual feedback only.
             * Real actions (popup menus, etc.) come later. */
            if (G_tray[i].icon == ICON_TRAY_AUDIO_HI ||
                G_tray[i].icon == ICON_TRAY_AUDIO_MUTE) G_audio_up ^= 1;
            else if (G_tray[i].icon == ICON_TRAY_NET_WIRED ||
                     G_tray[i].icon == ICON_TRAY_NET_IDLE) G_net_up ^= 1;
            G_panel_dirty = 1;
            return;
        }
    }
    /* Workspace plus button */
    if (y < PANEL_H) {
        int wx = 24 + sf_text_width("Yart") + 20;
        int plus_x = wx + G_ws_count*12 + 6;
        if (G_ws_count<MAX_WS && abs_int(x-plus_x)<4 && abs_int(y-PANEL_H/2)<6) {
            G_ws_count++; G_panel_dirty=1; return;
        }
        /* Workspace dot click -> switch */
        for (int i=0;i<G_ws_count;i++) {
            int cx = wx+i*12, cy=PANEL_H/2;
            if ((x-cx)*(x-cx)+(y-cy)*(y-cy) <= 16) { G_ws_active=i; G_panel_dirty=1; return; }
        }
    }
    /* Dock icon click: launch the real app / act on the concept. */
    if (y >= G_dock_y-4 && y <= G_dock_y+G_dock_h) {
        int start_x = G_dock_x + G_dock_w/2 - (DOCK_N-1)*DOCK_MW/2;
        for (int i=0;i<DOCK_N;i++) {
            int cx = start_x + i*DOCK_MW;
            if (abs_int(x-cx) < DOCK_MW/2) {
                G_slot_bounce[i] = 16;   /* initial upward velocity */
                G_slot_bounce_t0[i] = time_ms();
                G_dock_dirty = 1;
                if (i == 0) {
                    launch_settings();           /* the REAL app */
                } else {
                    /* Trash: a real concept, currently always empty */
                    const char *msg = "Trash is empty";
                    int k=0; const char *p=msg;
                    while (*p && k<(int)sizeof(G_osd_text)-1) G_osd_text[k++]=*p++;
                    G_osd_text[k]=0;
                    G_osd_t0 = time_ms();
                }
                return;
            }
        }
    }
}

/* ================================================================
 * Real apps: window surfaces (composited by this wm)
 * ================================================================ */
static win_t *win_find(u32 id) {
    for (int i=0;i<WM_MAX_WIN;i++)
        if (G_wins[i].active && G_wins[i].id == id) return &G_wins[i];
    return NULL;
}
static win_t *win_alloc(void) {
    for (int i=0;i<WM_MAX_WIN;i++)
        if (!G_wins[i].active) return &G_wins[i];
    return NULL;
}

/* Ask the kernel for the current surfaces; track + focus new ones. */
static void wm_scan_windows(void) {
    wm_surf_info_t arr[WM_MAX_WIN];
    int n = (int)wm_scan(arr, WM_MAX_WIN);
    if (n < 0) return;
    /* mark all as unseen this pass */
    for (int i=0;i<WM_MAX_WIN;i++) G_wins[i].seen = false;
    for (int i=0;i<n;i++) {
        wm_surf_info_t *si = &arr[i];
        win_t *w = win_find(si->id);
        if (!w) {
            w = win_alloc();
            if (!w) continue;
            w->active = true; w->id = si->id;
            w->owner = si->owner_pid;
            w->dirty = true;
            G_win_dirty = 1;
            /* new window: give it input focus */
            wm_focus(si->owner_pid);
            if (G_app_pid == 0) G_app_pid = (int)si->owner_pid;
            klog("wm: new app window (surface ");
            { char b[8]; itoa0((int)si->id,b,0); klog(b); }
            klog(") pid ");
            { char b[8]; itoa0((int)si->owner_pid,b,0); klog(b); klog("\n"); }
        }
        w->seen = true;
        w->x = (int)si->win_x; w->y = (int)si->win_y;
        w->w = (int)si->w;     w->h = (int)si->h;
        w->va = si->app_va;                 /* wm-side mapping           */
        if (si->dirty) { w->dirty = true; G_win_dirty = 1; }
    }
    /* windows that vanished: the app exited - clear state */
    for (int i=0;i<WM_MAX_WIN;i++) {
        if (G_wins[i].active && !G_wins[i].seen) {
            if (G_app_pid == (int)G_wins[i].owner) G_app_pid = 0;
            G_wins[i].active = false;
            G_wins[i].dirty = false;
            G_win_dirty = 1;
            G_full_repaint = 1;      /* scrub its old pixels */
        }
    }
}

static void draw_one_window(win_t *w);   /* defined below */

/* Draw every window that needs it (called from the paint path). */
/* Erase every DIRTY window's old rect back to wallpaper (drag moves the
 * window; without this the old position keeps its pixels and smears). */
static void restore_dirty_windows(void) {
    for (int i=0;i<WM_MAX_WIN;i++) {
        win_t *w = &G_wins[i];
        if (!w->active || !w->dirty) continue;
        int ty = w->y - 24;
        if (ty < PANEL_H) ty = PANEL_H;
        restore_rect(&G_fb, w->x - 6, ty - 6, w->w + 12,
                     (ty + 24 + w->h) - (ty - 6));
    }
}

static void draw_windows(void) {
    if (!G_win_dirty && !G_full_repaint) return;
    int drew = 0;
    for (int i=0;i<WM_MAX_WIN;i++) {
        if (!G_wins[i].active) continue;
        if (!G_wins[i].dirty && !G_full_repaint) continue;
        draw_one_window(&G_wins[i]);
        drew++;
    }
    if (drew) G_win_dirty = 0;
}

/* Dock click on Settings: launch /bin/settings if it isn't running, else
 * refocus it.  fork() + exec() - the child becomes the app. */
static void launch_settings(void) {
    if (G_app_pid != 0) {
        /* ghost check: if the app is alive but has NO window anymore
         * (it was closed via the title-bar X), kill the leftover and
         * relaunch fresh */
        bool has_win = false;
        for (int i=0;i<WM_MAX_WIN;i++)
            if (G_wins[i].active && G_wins[i].owner == (u32)G_app_pid)
                { has_win = true; break; }
        if (!has_win) {
            kill((long)G_app_pid);
            G_app_pid = 0;
        } else if (wm_focus((unsigned)G_app_pid) != 0) {
            /* app died but we hadn't noticed: relaunch */
            G_app_pid = 0;
        } else {
            return;
        }
    }
    long pid = fork();
    if (pid == 0) {
        char *argv[] = { "/bin/settings", 0 };
        char *envp[] = { "HOME=/home/yart", 0 };
        long r = exec("/bin/settings", argv, envp);
        (void)r;
        klog("wm: exec /bin/settings failed\n");
        exit(1);
    } else if (pid > 0) {
        G_app_pid = (int)pid;
        klog("wm: launched settings (pid ");
        { char b[8]; itoa0((int)pid,b,0); klog(b); }
        klog(")\n");
    }
}

/* Reap our app child when it exits (WNOHANG - never blocks the wm) so
 * G_app_pid doesn't point at a zombie and the dock can relaunch. */
static void poll_app_child(void) {
    if (G_app_pid <= 0) return;
    int st = 0;
    long r = waitpid_nohang((long)G_app_pid, &st);
    if (r == G_app_pid || r == -1)
        G_app_pid = 0;         /* reaped or already gone */
}

/* Poll /home/yart/cursor.conf once a second; the Settings app writes it
 * when the user picks a cursor theme. */
static void poll_cursor_config(long now_ms) {
    if (now_ms - G_cfg_last_check < 1000) return;
    G_cfg_last_check = now_ms;
    int fd = open("/home/yart/cursor.conf", 0);          /* O_RDONLY */
    if (fd < 0) return;
    char buf[96];
    long n = read(fd, buf, (long)sizeof buf - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = 0;
    /* parse "theme=NAME" */
    char *p = buf;
    while (*p) {
        if (p[0]=='t' && p[1]=='h' && p[2]=='e' && p[3]=='m' && p[4]=='e' && p[5]=='=') {
            char name[24];
            int i = 0;
            p += 6;
            while (*p && *p!='\n' && *p!='\r' && i<23) name[i++] = *p++;
            name[i] = 0;
            if (strcmp(name, G_theme_name) != 0) {
                int t = 0;
                if (strcmp(name, "classic") != 0) {
                    int idx = cursors_theme_by_name(name);
                    t = (idx >= 0) ? idx + 1 : 0;
                }
                cursor_apply_theme(t);
                strncpy(G_theme_name, name, sizeof G_theme_name - 1);
                G_last_cursor = -1;          /* force a repaint of the cursor */
                klog("wm: cursor theme -> ");
                klog(G_theme_name);
                klog("\n");
            }
            return;
        }
        p++;
    }
}

/* ================================================================
 * Input
 * ================================================================ */
static void draw_one_window(win_t *w) {
    int ty = w->y - 24;                 /* title bar above the surface  */
    if (ty < PANEL_H) ty = PANEL_H;
    int x0 = w->x - 6, y0 = ty - 6;
    int x1 = w->x + w->w + 6, y1 = w->y + w->h + 6;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > G_fb.w) x1 = G_fb.w;
    if (y1 > G_fb.h) y1 = G_fb.h;
    /* restore the wallpaper under the window first */
    sf_blit(&G_fb, x0, y0, &G_wp, x0, y0, x1-x0, y1-y0);

    /* drop shadow */
    sf_round_rect_blend(&G_fb, w->x-3, ty-3, w->w+6, w->h+ty-w->y+6, 10,
                        ARGB(90,0,0,0));
    sf_round_rect_blend(&G_fb, w->x-2, ty-2, w->w+4, w->h+ty-w->y+4, 10,
                        ARGB(50,0,0,0));
    /* title bar */
    sf_round_rect(&G_fb, w->x, ty, w->w, 24, 8, RGB(0x1E,0x21,0x28));
    sf_fill_rect(&G_fb, w->x, ty+10, w->w, 14, RGB(0x1E,0x21,0x28));
    sf_text(&G_fb, w->x+10, ty+5,
            w->title[0] ? w->title : "App", RGB(0xEC,0xEE,0xF1));
    /* close button (right) */
    int cx = w->x + w->w - 18, cy = ty + 12;
    sf_round_rect(&G_fb, cx-9, cy-9, 18, 18, 9, RGB(0x3A,0x3E,0x46));
    sf_hline(&G_fb, cx-4, cy, 8, RGB(0xEC,0xEE,0xF1));
    sf_vline(&G_fb, cx, cy-4, 8, RGB(0xEC,0xEE,0xF1));
    /* body */
    sf_fill_rect(&G_fb, w->x, ty+24, w->w, w->h, RGB(0x14,0x16,0x1B));
    /* app content: the surface is mapped into OUR address space (the
     * scan returned the wm-side VA) */
    surface_t src;
    src.px = (u32 *)w->va;
    src.w = w->w; src.h = w->h; src.pitch = w->w;
    if (src.px)
        sf_blit(&G_fb, w->x, ty+24, &src, 0, 0, w->w, w->h);
    w->dirty = false;
}

static void poll_input(void) {
    int ev;
    while ((ev = poll_key()) != 0) {
        int scancode=(ev>>8)&0xFF, make=!(ev&0x80);
        if (!make) continue;
        /* F1..F12 and Super combos reserved for future; for now, just
         * workspace switching with F1/F2/F3 and the existing bindings. */
        if (scancode==0x3B && G_ws_count<MAX_WS) { G_ws_count++; G_panel_dirty=1; }
        else if (scancode==0x3C && G_ws_count>1) {
            G_ws_count--;
            if (G_ws_active>=G_ws_count) G_ws_active=G_ws_count-1;
            G_panel_dirty=1;
        } else if (scancode==0x4D && G_ws_active+1<G_ws_count) { G_ws_active++; G_panel_dirty=1; }
        else if (scancode==0x4B && G_ws_active>0) { G_ws_active--; G_panel_dirty=1; }
        /* F5 = cycle wallpaper */
        else if (scancode==0x3F) {
            int n = wallpaper_count();
            if (n > 0) {
                G_wp_index = (G_wp_index + 1) % n;
                wallpaper_load_index(G_wp_index);
                wallpaper_bind(&G_wp);
                G_full_repaint = 1;
                G_panel_dirty = 1; G_dock_dirty = 1;
                /* OSD: show wallpaper name */
                const char *name = (G_wp_index < WP_NAMES_N) ? WP_NAMES[G_wp_index] : "Wallpaper";
                int k=0; const char *p=name; while (*p && k<(int)sizeof(G_osd_text)-1) G_osd_text[k++]=*p++; G_osd_text[k]=0;
                G_osd_t0 = time_ms();
            }
        }
    }
    mouse_ev_t m;
    int moved=0, clicked=0;
    while (poll_mouse(&m)) {
        G_cx+=m.dx; G_cy+=m.dy;
        if (G_cx<0) G_cx=0;
        if (G_cy<0) G_cy=0;
        if (G_cx>=G_fb.w) G_cx=G_fb.w-1;
        if (G_cy>=G_fb.h) G_cy=G_fb.h-1;
        /* Detect a click: any button transition 0 -> 1 */
        unsigned char prev = G_mb;
        G_mb = m.buttons;
        if ((G_mb & 1) && !(prev & 1)) { handle_click(G_cx, G_cy, 1); clicked=1; }
        if ((G_mb & 2) && !(prev & 2)) { handle_click(G_cx, G_cy, 3); clicked=1; }
        if ((G_mb & 4) && !(prev & 4)) { handle_click(G_cx, G_cy, 2); clicked=1; }
        moved=1;
    }
    if (moved || clicked) G_dock_dirty = 1;
}

static void update_status(void) {
    unsigned ni[5];
    G_net_up = (net_info(ni)==0 && ni[4]!=0)?1:0;
    G_audio_up = 1;
    G_bat_up = 1;
}

/* ================================================================
 * Entry
 * ================================================================ */
void wm_run(void) {
    fb_info_t fi;
    void *fb_px = fb_info(&fi);
    if (!fb_px) { klog("wm: fb_info failed\n"); return; }
    G_fb.px=(u32*)fb_px;
    G_fb.w=(int)fi.w; G_fb.h=(int)fi.h; G_fb.pitch=(int)fi.pitch;
    G_cx=G_fb.w/2; G_cy=G_fb.h/2;

    assets_init();
    cursors_init();                 /* photo cursor themes (from the web) */
    cursor_build_all();
    strncpy(G_theme_name, "classic", sizeof G_theme_name - 1);
    for (int i=0;i<DOCK_N;i++) {
        G_slot_size[i]=ICON_SZ; G_slot_lift[i]=0;
        G_slot_target_sz[i]=ICON_SZ; G_slot_target_lift[i]=0;
        G_slot_bounce[i]=0; G_slot_bounce_t0[i] = 0;
    }

    if (wallpaper_load(&G_wp)!=0 || G_wp.w!=G_fb.w || G_wp.h!=G_fb.h) {
        klog("wm: wallpaper load failed\n"); return;
    }
    int n_wp = wallpaper_count();
    sf_blit(&G_fb,0,0,&G_wp,0,0,G_fb.w,G_fb.h);
    (void)n_wp;

    build_icon_cache();
    build_panel_sprite();
    build_dock_sprite();
    G_panel_dirty = 1; G_dock_dirty = 1;

    G_start_ms = time_ms();
    long wt = wall_time();
    if (wt>0) format_wallclock(wt, G_clk_text);

    klog("wm: ring-3 compositor up [cached chrome, single-icon tween, cursors, tray clicks]\n");

    unsigned long frames=0;
    long fps_start=G_start_ms;
    for (;;) {
        poll_input();
        update_status();
        long now = time_ms();
        long uptime = now - G_start_ms;
        poll_cursor_config(now);        /* pick up Settings changes ~1s */
        poll_app_child();               /* reap the app when it exits    */
        wm_scan_windows();              /* new app windows -> focus+draw */

        unsigned long s = (unsigned long)(uptime/1000);
        if (s != G_last_clock_s) {
            long wt2 = wall_time();
            if (wt2>0) format_wallclock(wt2, G_clk_text);
            else {
                long secs=uptime/1000;
                int h=(int)((secs/3600)%24),m_=(int)((secs/60)%60),s_=(int)(secs%60);
                G_clk_text[0]='0'+h/10;G_clk_text[1]='0'+h%10;G_clk_text[2]=':';
                G_clk_text[3]='0'+m_/10;G_clk_text[4]='0'+m_%10;G_clk_text[5]=':';
                G_clk_text[6]='0'+s_/10;G_clk_text[7]='0'+s_%10;G_clk_text[8]=0;
            }
            G_panel_dirty=1; G_last_clock_s=s;
        }

        int cur_id = cursor_pick(G_cx, G_cy);
        int cursor_moved = (G_last_cx!=G_cx || G_last_cy!=G_cy || G_last_cursor!=cur_id);
        int pressed = (G_mb & 1) ? 1 : 0;
        long osd_age = now - G_osd_t0;
        int osd_visible = (osd_age >= 0 && osd_age < 1800);
        static int s_osd_was_visible = 0;
        int osd_just_gone = (s_osd_was_visible && !osd_visible);
        s_osd_was_visible = osd_visible;

        int tweening=0;
        for (int i=0;i<DOCK_N;i++)
            if (G_slot_size[i]!=G_slot_target_sz[i] || G_slot_lift[i]!=G_slot_target_lift[i])
                { tweening=1; break; }
        long dt = now - G_tooltip_t0;
        int tooltip_anim = (G_tooltip_item>=0) &&
            (dt < (long)(TOOLTIP_DELAY_MS+TOOLTIP_FADE_MS));

        int need_paint = G_panel_dirty || G_dock_dirty || cursor_moved || tweening || tooltip_anim || G_full_repaint || osd_visible || osd_just_gone || G_win_dirty;
        if (osd_just_gone) {
            /* Clear the OSD area back to wallpaper so it doesn't linger. */
            int tw = sf_text_width(G_osd_text);
            int padx=18, w=tw+padx*2, h=32;
            int x = G_fb.w/2 - w/2;
            int y = G_fb.h - DOCK_H - DOCK_MARGIN - h - 30;
            if (y < PANEL_H+10) y = PANEL_H+10;
            restore_rect_win(&G_fb, x-6, y-6, w+12, h+12);
        }
        if (need_paint) {
            if (G_full_repaint) {
                /* Wallpaper switched: blit the entire wallpaper back */
                sf_blit(&G_fb, 0, 0, &G_wp, 0, 0, G_fb.w, G_fb.h);
                G_full_repaint = 0;
                /* rebuild cached chrome so it re-samples the new wallpaper */
                build_panel_sprite();
                build_dock_sprite();
                G_win_dirty = 1;   /* windows must be redrawn over the
                                      new wallpaper */
            } else {
                /* 1. Restore wallpaper under dirty regions */
                restore_cursor(&G_fb);
                restore_tooltip(&G_fb);
                if (G_dock_dirty || tweening || tooltip_anim) restore_dock_region(&G_fb);
                if (G_panel_dirty) restore_panel(&G_fb);
                if (G_win_dirty) restore_dirty_windows();
            }
            /* 2. Blit cached chrome */
            sf_blit(&G_fb, 0, 0, &G_panel_spr, 0, 0, G_panel_spr.w, G_panel_spr.h);
            sf_blit(&G_fb, G_dock_spr_x0, G_dock_spr_y0, &G_dock_spr,
                    0, 0, G_dock_spr_w, G_dock_spr_h);
            /* 3. Dynamic content */
            draw_panel_content(&G_fb, now);
            draw_dock_content(&G_fb, now, pressed);
            /* 4. App windows (title bars + surfaces) */
            draw_windows();
            if (osd_visible) {
                /* Wallpaper+chrome already painted under it; draw OSD on top.
                 * We also need to have restored the OSD area from wallpaper,
                 * which happens implicitly because osd_visible -> need_paint
                 * and we re-blit the panel/dock sprites which cover the top/
                 * bottom edges. For the desktop area the cursor restore /
                 * dock_region restore doesn't cover it, so erase by blitting
                 * wallpaper at OSD location first. */
                int tw = sf_text_width(G_osd_text);
                int padx=18, w=tw+padx*2, h=32;
                int x = G_fb.w/2 - w/2;
                int y = G_fb.h - DOCK_H - DOCK_MARGIN - h - 30;
                if (y < PANEL_H+10) y = PANEL_H+10;
                /* Clear prior frame (wallpaper + any window underneath) */
                restore_rect_win(&G_fb, x-4, y-4, w+8, h+8);
                /* Re-blit any dock shadow that overlaps that area (none at 30px above dock) */
                draw_osd(&G_fb, now);
            }
            cursor_draw(&G_fb, cur_id, G_cx, G_cy);
            G_last_cx=G_cx; G_last_cy=G_cy; G_last_cursor=cur_id;
            fb_flip(fb_px);
            frames++;
        }

        if (now - fps_start >= 5000) {
            char m[64],b[8]; int k=0;
            const char *p="wm: "; while(*p)m[k++]=*p++;
            itoa0((int)(frames*1000/(unsigned long)(now-fps_start)),b,0);
            const char *q=b; while(*q)m[k++]=*q++;
            const char *e=" fps\n"; while(*e)m[k++]=*e++; m[k]=0;
            klog(m);
            frames=0; fps_start=now;
        }

        /* --- pacing: bounded yields, mid-slice input poll ---
         * THE FREEZE FIX: the old loop spun on `time_ms() < target` with
         * yield() inside; every yield parks us until the next timer tick,
         * so a slow/stopped tick made the loop never converge (and mouse
         * movement made it exit WITHOUT yielding, monopolizing the CPU).
         * Now we yield a FIXED 2 times per frame (~50 fps max on a
         * 100 Hz tick) and poll the mouse in the slices - the frame
         * ALWAYS completes and the compositor ALWAYS gives the CPU back,
         * no matter what the clock does. */
        for (int slice = 0; slice < 2; slice++) {
            yield();
            mouse_ev_t me;
            while (poll_mouse(&me)) {
                G_cx+=me.dx; G_cy+=me.dy;
                if (G_cx<0) G_cx=0;
                if (G_cy<0) G_cy=0;
                if (G_cx>=G_fb.w) G_cx=G_fb.w-1;
                if (G_cy>=G_fb.h) G_cy=G_fb.h-1;
                unsigned char prev = G_mb;
                G_mb = me.buttons;
                if ((G_mb & 1) && !(prev & 1)) handle_click(G_cx,G_cy,1);
                if ((G_mb & 2) && !(prev & 2)) handle_click(G_cx,G_cy,3);
                if ((G_mb & 4) && !(prev & 4)) handle_click(G_cx,G_cy,2);
                G_dock_dirty=1;
            }
        }
    }
}
