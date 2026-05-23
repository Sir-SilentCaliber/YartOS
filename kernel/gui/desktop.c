/* Yart OS - desktop / window manager / dock / app drawer / taskbar.
 *
 * Stage-10 features:
 *   - pinned apps come from /etc/yart.conf (not a hardcoded array)
 *   - right-click on a dock slot offers pin/unpin/close-window
 *   - drawer overlay has a live search box
 *   - 4 virtual workspaces, Ctrl+Alt+Left/Right to switch
 *   - wallpaper can be a real BMP or a procedural gradient
 *   - cursor is rendered last every frame (decoupled from app paint)
 *   - night-light tint applied at the end
 */
#include <yart/gui.h>
#include <yart/drivers.h>
#include <yart/mm.h>
#include <yart/string.h>
#include <yart/console.h>
#include <yart/hal.h>
#include <yart/theme.h>
#include <yart/icons.h>
#include <yart/fs.h>
#include <yart/task.h>
#include <yart/menu.h>
#include <yart/apps.h>
#include <yart/config.h>
#include <yart/bmp.h>
#include <yart/pci.h>
#include <yart/session.h>
#include <yart/login_overlay.h>

#define BAR_H        26
#define DOCK_H       58
#define EDGE_GRAB    6
#define WIN_TITLE_H  24
#define BTN_R        7
#define WORKSPACES   4

/* ---------- forward declarations of bundled apps ---------- */
extern void open_terminal(void);
extern void open_files(const char *path);
extern void open_editor(const char *path);

/* ---------- frame scheduling ---------- */
static bool g_dirty = true;
static u64  g_last_frame_ms;
void wm_dirty(void) { g_dirty = true; }

/* ---------- workspaces ---------- */
static int current_ws = 0;
/* workspace each window belongs to, looked up via win->ud_workspace
   stored in the high bits of win->flags; cheaper: a tiny side map. */
#define MAX_TRACK 64
static struct { window_t *w; int ws; } ws_map[MAX_TRACK];
static int ws_map_count;

static void ws_remember(window_t *w) {
    for (int i = 0; i < ws_map_count; i++)
        if (ws_map[i].w == w) { ws_map[i].ws = current_ws; return; }
    if (ws_map_count < MAX_TRACK) {
        ws_map[ws_map_count].w  = w;
        ws_map[ws_map_count].ws = current_ws;
        ws_map_count++;
    }
}
static int ws_of(window_t *w) {
    for (int i = 0; i < ws_map_count; i++)
        if (ws_map[i].w == w) return ws_map[i].ws;
    return 0;
}
static void ws_forget(window_t *w) {
    for (int i = 0; i < ws_map_count; i++)
        if (ws_map[i].w == w) {
            ws_map[i] = ws_map[--ws_map_count];
            return;
        }
}

/* ---------- click hooks (back-compat for apps that poll) ---------- */
int  g_files_click_x, g_files_click_y;
bool g_files_click_pending;
int  g_term_click_x,  g_term_click_y;
bool g_term_click_pending;
int  g_editor_click_x, g_editor_click_y;
bool g_editor_click_pending;

/* ---------- window list (head = top of z-order) ---------- */
static window_t *windows;

window_t *window_focused(void) {
    /* return first visible, non-min, on-current-workspace */
    for (window_t *w = windows; w; w = w->next)
        if ((w->flags & WIN_VIS) && !(w->flags & WIN_MIN) && ws_of(w) == current_ws)
            return w;
    return 0;
}

window_t *window_create(const char *title, icon_id_t icon,
                        int x, int y, int w, int h,
                        void (*paint)(window_t *)) {
    window_t *win = kzalloc(sizeof *win);
    if (!win) return 0;
    strncpy(win->title, title, sizeof win->title - 1);
    win->icon  = icon;
    win->x = x; win->y = y; win->w = w; win->h = h;
    win->saved_x = x; win->saved_y = y; win->saved_w = w; win->saved_h = h;
    win->min_w = 200; win->min_h = 120;
    win->paint = paint;
    win->bg    = TH_WIN_BG;
    win->flags = WIN_VIS | WIN_FOCUS;

    if (windows) windows->flags &= ~WIN_FOCUS;
    win->next = windows;
    windows = win;
    ws_remember(win);
    g_dirty = true;
    return win;
}

void window_destroy(window_t *w) {
    if (!w) return;
    if (w->on_close) w->on_close(w);
    window_t **link = &windows;
    while (*link && *link != w) link = &(*link)->next;
    if (*link) *link = w->next;
    if (windows) windows->flags |= WIN_FOCUS;
    ws_forget(w);
    if (w->ud) kfree(w->ud);
    kfree(w);
    g_dirty = true;
}

void window_focus(window_t *w) {
    if (!w || w == windows) return;
    window_t **link = &windows;
    while (*link && *link != w) link = &(*link)->next;
    if (!*link) return;
    *link = w->next;
    if (windows) windows->flags &= ~WIN_FOCUS;
    w->next = windows;
    w->flags |= WIN_FOCUS;
    w->flags &= ~WIN_MIN;
    windows = w;
    g_dirty = true;
}

void window_minimize(window_t *w) {
    if (!w) return;
    w->flags |= WIN_MIN;
    w->flags &= ~WIN_FOCUS;
    g_dirty = true;
}

void window_maximize(window_t *w) {
    if (!w) return;
    if (!(w->flags & WIN_MAX)) {
        w->saved_x = w->x; w->saved_y = w->y;
        w->saved_w = w->w; w->saved_h = w->h;
        w->x = 0; w->y = BAR_H;
        w->w = g_fb.width;
        w->h = g_fb.height - BAR_H - DOCK_H;
        w->flags |= WIN_MAX;
    } else {
        window_restore(w);
    }
    g_dirty = true;
}

void window_restore(window_t *w) {
    if (!w) return;
    if (w->flags & WIN_MAX) {
        w->x = w->saved_x; w->y = w->saved_y;
        w->w = w->saved_w; w->h = w->saved_h;
        w->flags &= ~WIN_MAX;
    }
    w->flags &= ~WIN_MIN;
    window_focus(w);
}

void window_close_requested(window_t *w) { window_destroy(w); }

/* ---------- wallpaper ---------- */
extern const u32 yart_wallpaper_pixels[];
extern const int yart_wallpaper_w;
extern const int yart_wallpaper_h;

static bmp_image_t g_wallpaper;
static bool        g_wallpaper_loaded;

static void try_load_wallpaper(void) {
    if (g_wallpaper_loaded) return;
    if (strcmp(g_config.wallpaper_mode, "image") != 0) return;
    if (bmp_load(g_config.wallpaper_path, &g_wallpaper) == 0) {
        g_wallpaper_loaded = true;
        kprintf("wallpaper: loaded %s (%dx%d)\n",
                g_config.wallpaper_path, g_wallpaper.w, g_wallpaper.h);
    } else {
        kprintf("wallpaper: failed to load %s, using fallback\n",
                g_config.wallpaper_path);
    }
}

static void draw_wallpaper(void) {
    if (g_wallpaper_loaded) {
        bmp_blit(&g_wallpaper, 0, 0, g_fb.width, g_fb.height);
        return;
    }
    /* fallback: stretched ARGB asset */
    u32 fbw = g_fb.width, fbh = g_fb.height;
    int aw = yart_wallpaper_w, ah = yart_wallpaper_h;
    for (u32 y = 0; y < fbh; y++) {
        int sy = (int)((u64)y * ah / fbh);
        const u32 *src = &yart_wallpaper_pixels[sy * aw];
        for (u32 x = 0; x < fbw; x++) {
            int sx = (int)((u64)x * aw / fbw);
            draw_pixel(x, y, src[sx]);
        }
    }
}

/* ---------- chrome / hit test ---------- */
typedef enum {
    HIT_NONE, HIT_BODY, HIT_TITLE,
    HIT_BTN_CLOSE, HIT_BTN_MIN, HIT_BTN_MAX,
    HIT_RESIZE_R, HIT_RESIZE_B, HIT_RESIZE_BR,
    HIT_RESIZE_L, HIT_RESIZE_T, HIT_RESIZE_BL,
    HIT_RESIZE_TR, HIT_RESIZE_TL
} hit_t;

static hit_t hit_test(window_t *w, int px, int py) {
    if (px < w->x - EDGE_GRAB || px >= w->x + w->w + EDGE_GRAB) return HIT_NONE;
    if (py < w->y - EDGE_GRAB || py >= w->y + w->h + EDGE_GRAB) return HIT_NONE;
    bool L = px < w->x + EDGE_GRAB;
    bool R = px >= w->x + w->w - EDGE_GRAB;
    bool T = py < w->y + EDGE_GRAB;
    bool B = py >= w->y + w->h - EDGE_GRAB;
    if (!(w->flags & (WIN_NORES | WIN_MAX))) {
        if (L && T) return HIT_RESIZE_TL;
        if (R && T) return HIT_RESIZE_TR;
        if (L && B) return HIT_RESIZE_BL;
        if (R && B) return HIT_RESIZE_BR;
        if (L) return HIT_RESIZE_L;
        if (R) return HIT_RESIZE_R;
        if (T) return HIT_RESIZE_T;
        if (B) return HIT_RESIZE_B;
    }
    if (py < w->y + WIN_TITLE_H) {
        int bx_close = w->x + w->w - 14;
        int bx_max   = bx_close - 22;
        int bx_min   = bx_max   - 22;
        int by = w->y + WIN_TITLE_H/2;
        int dx, dy;
        dx = px - bx_close; dy = py - by;
        if (dx*dx + dy*dy <= BTN_R * BTN_R + 2) return HIT_BTN_CLOSE;
        dx = px - bx_max;   dy = py - by;
        if (dx*dx + dy*dy <= BTN_R * BTN_R + 2) return HIT_BTN_MAX;
        dx = px - bx_min;   dy = py - by;
        if (dx*dx + dy*dy <= BTN_R * BTN_R + 2) return HIT_BTN_MIN;
        return HIT_TITLE;
    }
    return HIT_BODY;
}

static void draw_traffic_lights(window_t *w) {
    bool focused = (w->flags & WIN_FOCUS) != 0;
    int by = w->y + WIN_TITLE_H/2;
    int bx_close = w->x + w->w - 14;
    int bx_max   = bx_close - 22;
    int bx_min   = bx_max   - 22;
    color_t cc = focused ? TH_BTN_CLOSE : TH_BTN_DIM;
    color_t cm = focused ? TH_BTN_MIN   : TH_BTN_DIM;
    color_t cx_ = focused ? TH_BTN_MAX  : TH_BTN_DIM;
    for (int dy = -BTN_R; dy <= BTN_R; dy++)
        for (int dx = -BTN_R; dx <= BTN_R; dx++) {
            if (dx*dx + dy*dy > BTN_R*BTN_R) continue;
            draw_pixel(bx_min   + dx, by + dy, cm);
            draw_pixel(bx_max   + dx, by + dy, cx_);
            draw_pixel(bx_close + dx, by + dy, cc);
        }
}

static void draw_window(window_t *w) {
    if (!(w->flags & WIN_VIS) || (w->flags & WIN_MIN)) return;
    if (ws_of(w) != current_ws) return;
    for (int s = 1; s <= 6; s++) {
        u32 alpha = 80 - s * 12;
        color_t sc = (alpha << 24) | 0;
        draw_hline(w->x + s, w->y + w->h, w->w, sc);
        draw_vline(w->x + w->w, w->y + s, w->h, sc);
    }
    int r = g_config.corner_radius;
    draw_rounded_rect(w->x, w->y, w->w, w->h, r, w->bg);
    draw_rounded_rect(w->x, w->y, w->w, WIN_TITLE_H, r, TH_WIN_TITLE_FG);
    if (!(w->flags & WIN_FOCUS))
        draw_rounded_rect(w->x, w->y, w->w, WIN_TITLE_H, r, TH_WIN_TITLE);
    draw_hline(w->x + 1, w->y + WIN_TITLE_H, w->w - 2, TH_WIN_BORDER);
    color_t border = (w->flags & WIN_FOCUS) ? g_config.border : TH_WIN_BORDER;
    draw_rounded_rect_outline(w->x, w->y, w->w, w->h, r, border);
    color_t tc = (w->flags & WIN_FOCUS) ? TH_TEXT_INV : TH_TEXT_DIM;
    draw_text(w->x + 38, w->y + 4, w->title, tc, 0);
    draw_icon_sized(w->x + 6, w->y + 3, w->icon, 18);
    draw_traffic_lights(w);
    if (w->paint) w->paint(w);
}

/* ---------- top status bar ---------- */
static void draw_statusbar(void) {
    draw_rect(0, 0, g_fb.width, BAR_H, TH_PANEL);
    draw_hline(0, BAR_H - 1, g_fb.width, TH_PANEL_LINE);

    draw_text(10, 5, "Yart", TH_TEXT, 0);
    draw_text(10 + 4 * FONT_W + 2, 5, "OS", g_config.accent, 0);

    /* workspace pips */
    int wx = 10 + 8 * FONT_W;
    for (int i = 0; i < WORKSPACES; i++) {
        int x = wx + 14 + i * 16;
        color_t c = (i == current_ws) ? g_config.accent : TH_TEXT_MUTED;
        draw_rect(x, 8, 10, 10, c);
    }

    window_t *f = window_focused();
    if (f && (f->flags & WIN_VIS) && !(f->flags & WIN_MIN)) {
        int tw = text_width(f->title);
        draw_text((g_fb.width - tw) / 2, 5, f->title, TH_TEXT_DIM, 0);
    }

    rtc_time_t t; rtc_read(&t);
    char clk[40];
    if (g_config.time_format24) {
        snprintf(clk, sizeof clk, "%02u:%02u  %04u-%02u-%02u",
                 t.hour, t.minute, t.year, t.month, t.day);
    } else {
        int h12 = t.hour % 12; if (h12 == 0) h12 = 12;
        const char *ap = t.hour < 12 ? "AM" : "PM";
        snprintf(clk, sizeof clk, "%02d:%02u %s  %04u-%02u-%02u",
                 h12, t.minute, ap, t.year, t.month, t.day);
    }
    int w = text_width(clk);
    int clock_x = g_fb.width - w - 12;
    draw_text(clock_x, 5, clk, TH_TEXT, 0);

    /* applets to the left of the clock */
    int ax = clock_x - 12;
    /* audio - mini speaker */
    {
        const char *a = pci_audio_name();
        const char *txt = a ? a : "no-audio";
        int tw = text_width(txt);
        ax -= tw;
        draw_text(ax, 5, txt, g_audio_present ? TH_TEXT_DIM : TH_TEXT_MUTED, 0);
        ax -= 12;
    }
    /* network */
    {
        const char *n = pci_nic_name();
        const char *txt = n ? n : "no-net";
        int tw = text_width(txt);
        ax -= tw;
        draw_text(ax, 5, txt, g_nic_present ? g_config.accent : TH_TEXT_MUTED, 0);
    }

    /* power / session menu trigger */
    if (g_session.logged_in) {
        const char *pwr = "[P]";
        int pw = text_width(pwr);
        draw_text(g_fb.width - pw - 10, 5, pwr, TH_TEXT_MUTED, 0);
    } else if (g_session.user_count > 0 && !g_session.setup_wizard_visible) {
        const char *guest = "Sign In";
        int gw = text_width(guest);
        draw_text(g_fb.width - gw - 10, 5, guest, TH_TEXT_DIM, 0);
    }
}

/* ---------- dock ---------- */
typedef struct {
    int x, y, w, h;
    enum { SLOT_PINNED, SLOT_TASK, SLOT_DRAWER } kind;
    const yart_app_t *app;   /* for SLOT_PINNED */
    window_t         *win;   /* for SLOT_TASK   */
} dock_slot_t;

static dock_slot_t slots[64];
static int nslots;

static int dock_y(void) { return g_fb.height - DOCK_H; }

static void layout_dock(void) {
    nslots = 0;
    int gap = g_config.dock_spacing;
    int icon_box = g_config.dock_icon_size + 16;
    if (icon_box < 36) icon_box = 36;

    /* count pinned + tasks (only current workspace) */
    int pinned_count = 0;
    for (int i = 0; i < g_config.dock_pinned_count; i++) {
        if (app_find(g_config.dock_pinned[i])) pinned_count++;
    }
    int task_count = 0;
    for (window_t *w = windows; w; w = w->next)
        if ((w->flags & WIN_VIS) && ws_of(w) == current_ws) task_count++;

    int total = pinned_count + task_count + 1; /* +1 drawer */
    int total_w = total * icon_box + (total - 1) * gap;
    int x0 = (g_fb.width - total_w) / 2;
    int yy = dock_y() + (DOCK_H - icon_box) / 2;
    int xx = x0;

    for (int i = 0; i < g_config.dock_pinned_count; i++) {
        const yart_app_t *a = app_find(g_config.dock_pinned[i]);
        if (!a) continue;
        slots[nslots++] = (dock_slot_t){ xx, yy, icon_box, icon_box, SLOT_PINNED, a, 0 };
        xx += icon_box + gap;
    }
    for (window_t *w = windows; w; w = w->next) {
        if (!(w->flags & WIN_VIS) || ws_of(w) != current_ws) continue;
        slots[nslots++] = (dock_slot_t){ xx, yy, icon_box, icon_box, SLOT_TASK, 0, w };
        xx += icon_box + gap;
    }
    slots[nslots++] = (dock_slot_t){ xx, yy, icon_box, icon_box, SLOT_DRAWER, 0, 0 };
}

static void draw_dock(void) {
    int dy = dock_y();
    draw_rect(0, dy, g_fb.width, DOCK_H, TH_PANEL);
    draw_hline(0, dy, g_fb.width, TH_PANEL_LINE);
    int cx, cy; cursor_get_pos(&cx, &cy);
    layout_dock();

    for (int i = 0; i < nslots; i++) {
        dock_slot_t *s = &slots[i];
        bool hover = (cx >= s->x && cx < s->x + s->w &&
                      cy >= s->y && cy < s->y + s->h);
        if (hover) {
            draw_rounded_rect(s->x - 2, s->y - 2, s->w + 4, s->h + 4, 6, TH_PANEL_HI);
            draw_rounded_rect_outline(s->x - 2, s->y - 2, s->w + 4, s->h + 4, 6, TH_ACCENT_DIM);
        }
        if (s->kind == SLOT_DRAWER) {
            draw_icon(s->x + (s->w - 32)/2, s->y + (s->h - 32)/2, ICON_DRAWER);
        } else if (s->kind == SLOT_TASK && s->win) {
            draw_icon(s->x + (s->w - 32)/2, s->y + (s->h - 32)/2, s->win->icon);
            if (s->win->flags & WIN_FOCUS) {
                draw_hline(s->x + 8, s->y + s->h + 2, s->w - 16, g_config.accent);
                draw_hline(s->x + 8, s->y + s->h + 3, s->w - 16, TH_ACCENT_DIM);
            } else {
                draw_hline(s->x + 16, s->y + s->h + 2, s->w - 32, TH_TEXT_MUTED);
            }
        } else if (s->kind == SLOT_PINNED && s->app) {
            draw_icon(s->x + (s->w - 32)/2, s->y + (s->h - 32)/2, s->app->icon);
            if (hover) {
                int tw = text_width(s->app->name);
                draw_text(s->x + (s->w - tw)/2, s->y + s->h + 2, s->app->name,
                          TH_TEXT_DIM, 0);
            }
        }
    }
}

/* ---------- app drawer (with search) ---------- */
static bool drawer_open;
static char drawer_query[40];
static int  drawer_query_len;
static int  drawer_hover = -1;

/* dock right-click "pin" ack-helper */
static const yart_app_t *pin_target_app;
static window_t          *task_target_win;

static void cm_pin(void *u)    { (void)u; if (pin_target_app)  config_pin  (pin_target_app->name); }
static void cm_unpin(void *u)  { (void)u; if (pin_target_app)  config_unpin(pin_target_app->name); }
static void cm_close(void *u)  { (void)u; if (task_target_win) window_close_requested(task_target_win); }
static void cm_pin_t(void *u)  { (void)u; if (task_target_win) {
    /* find app whose icon matches; fallback: use title */
    for (int i = 0; i < yart_app_catalog_count; i++)
        if (yart_app_catalog[i].icon == task_target_win->icon) {
            config_pin(yart_app_catalog[i].name); return;
        }
}}

bool drawer_open_request;
static void cm_drawer(void *u)  { (void)u; drawer_open_request = true; }

/* desktop context menu */
extern void open_calc(void);
extern void open_sysmon(void);
static void cm_terminal(void *u){ (void)u; open_terminal(); }
static void cm_files(void *u)   { (void)u; open_files("/home/yart"); }
static void cm_editor(void *u)  { (void)u; open_editor(0); }
static void cm_calc(void *u)    { (void)u; open_calc(); }
static void cm_sysmon(void *u)  { (void)u; open_sysmon(); }

static void open_desktop_menu(int x, int y) {
    static menu_item_t items[] = {
        { "New Terminal Here", cm_terminal, 0, false, false },
        { "Open Files",        cm_files,    0, false, false },
        { "New Editor",        cm_editor,   0, false, false },
        { "",                  0,           0, true,  false },
        { "Calculator",        cm_calc,     0, false, false },
        { "System Monitor",    cm_sysmon,   0, false, false },
        { "",                  0,           0, true,  false },
        { "Show All Apps",     cm_drawer,   0, false, false },
    };
    menu_open(x, y, items, sizeof items / sizeof items[0]);
}

static void open_pinned_menu(int x, int y, const yart_app_t *a) {
    pin_target_app = a;
    static menu_item_t items_pin[2];
    bool pinned = config_is_pinned(a->name);
    strncpy(items_pin[0].label, pinned ? "Unpin from Dock" : "Pin to Dock",
            MENU_LABEL_LEN - 1);
    items_pin[0].on_click = pinned ? cm_unpin : cm_pin;
    items_pin[0].ud = 0;
    items_pin[0].separator = false;
    items_pin[0].disabled  = false;
    strncpy(items_pin[1].label, "Launch", MENU_LABEL_LEN - 1);
    items_pin[1].on_click = 0;
    items_pin[1].ud = 0;
    items_pin[1].separator = false;
    items_pin[1].disabled  = true;
    menu_open(x, y, items_pin, 2);
}

static void open_task_menu(int x, int y, window_t *w) {
    task_target_win = w;
    static menu_item_t items_task[3];
    strncpy(items_task[0].label, "Pin App to Dock", MENU_LABEL_LEN - 1);
    items_task[0].on_click = cm_pin_t;
    items_task[0].ud = 0; items_task[0].separator = false; items_task[0].disabled = false;
    items_task[1].label[0] = 0;
    items_task[1].on_click = 0; items_task[1].ud = 0;
    items_task[1].separator = true; items_task[1].disabled = false;
    strncpy(items_task[2].label, "Close Window", MENU_LABEL_LEN - 1);
    items_task[2].on_click = cm_close;
    items_task[2].ud = 0; items_task[2].separator = false; items_task[2].disabled = false;
    menu_open(x, y, items_task, 3);
}

static void draw_drawer(void) {
    if (!drawer_open) return;
    /* fast halve-each-channel dim */
    const u32 MASK = 0x007F7F7FU;
    for (u32 y = 0; y < g_fb.height; y++) {
        u32 *row = &g_fb.pixels[y * g_fb.pitch_px];
        for (u32 x = 0; x < g_fb.width; x++) {
            row[x] = 0xFF000000U | ((row[x] >> 1) & MASK);
        }
    }
    int cols = 4;
    int cell_w = 130, cell_h = 110;
    int rows = (yart_app_catalog_count + cols - 1) / cols;
    int pw = cols * cell_w + 40;
    int ph = rows * cell_h + 130;
    int px = (g_fb.width - pw) / 2;
    int py = (g_fb.height - ph) / 2;
    draw_rounded_rect(px, py, pw, ph, 10, TH_PANEL);
    draw_rounded_rect_outline(px, py, pw, ph, 10, TH_ACCENT_DIM);
    draw_text(px + 20, py + 16, "Applications", TH_TEXT, 0);
    draw_text(px + pw - text_width("Esc to close") - 20, py + 16,
              "Esc to close", TH_TEXT_MUTED, 0);

    /* search box */
    int sb_x = px + 20, sb_y = py + 40;
    int sb_w = pw - 40, sb_h = 28;
    draw_rounded_rect(sb_x, sb_y, sb_w, sb_h, 4, TH_EDITOR_BG);
    draw_rounded_rect_outline(sb_x, sb_y, sb_w, sb_h, 4, TH_ACCENT_DIM);
    if (drawer_query_len == 0) {
        draw_text(sb_x + 10, sb_y + 6, "Type to search",
                  TH_TEXT_MUTED, 0xFF000000);
    } else {
        draw_text(sb_x + 10, sb_y + 6, drawer_query, TH_TEXT, 0xFF000000);
    }
    /* caret */
    if ((pit_ticks() / 30) & 1) {
        int cx = sb_x + 10 + drawer_query_len * FONT_W;
        draw_vline(cx, sb_y + 4, sb_h - 8, g_config.accent);
    }

    int cxm, cym; cursor_get_pos(&cxm, &cym);
    drawer_hover = -1;
    int gx = px + 20, gy = py + 80;
    int visible = 0;
    for (int i = 0; i < yart_app_catalog_count; i++) {
        const yart_app_t *a = &yart_app_catalog[i];
        /* filter by query */
        if (drawer_query_len > 0) {
            /* simple case-insensitive substring */
            const char *hs = a->name; const char *needle = drawer_query;
            bool match = false;
            for (const char *h = hs; *h && !match; h++) {
                int k;
                for (k = 0; needle[k]; k++) {
                    char a_ = h[k], b_ = needle[k];
                    if (a_ >= 'A' && a_ <= 'Z') a_ += 32;
                    if (b_ >= 'A' && b_ <= 'Z') b_ += 32;
                    if (a_ != b_) break;
                }
                if (needle[k] == 0) match = true;
            }
            if (!match) continue;
        }
        int col = visible % cols, row = visible / cols;
        visible++;
        int x = gx + col * cell_w;
        int y = gy + row * cell_h;
        bool hover = (cxm >= x && cxm < x + cell_w && cym >= y && cym < y + cell_h);
        if (hover) {
            drawer_hover = i;
            draw_rounded_rect(x, y, cell_w, cell_h, 8, TH_PANEL_HI);
            draw_rounded_rect_outline(x, y, cell_w, cell_h, 8, g_config.accent);
        }
        draw_icon_sized(x + (cell_w - 48)/2, y + 14, a->icon, 48);
        int tw = text_width(a->name);
        draw_text(x + (cell_w - tw)/2, y + cell_h - 22, a->name, TH_TEXT, 0);
    }
    if (visible == 0) {
        const char *msg = "No matches.";
        int tw = text_width(msg);
        draw_text(px + (pw - tw)/2, gy + 40, msg, TH_TEXT_MUTED, 0);
    }
}

/* ---------- input ---------- */
static window_t *dragging;
static hit_t     drag_hit;
static int       drag_dx, drag_dy;
static int       drag_ox, drag_oy, drag_ow, drag_oh;
static bool      prev_left, prev_right;

static window_t *window_at(int x, int y) {
    for (window_t *w = windows; w; w = w->next) {
        if (!(w->flags & WIN_VIS) || (w->flags & WIN_MIN)) continue;
        if (ws_of(w) != current_ws) continue;
        hit_t h = hit_test(w, x, y);
        if (h != HIT_NONE) return w;
    }
    return 0;
}

void desktop_handle_mouse(int dx, int dy, u8 buttons) {
    int cx, cy; cursor_get_pos(&cx, &cy);
    cursor_set_pos(cx + dx, cy + dy);
    cursor_get_pos(&cx, &cy);
    g_dirty = true;

    bool left  = buttons & 1;
    bool right = buttons & 2;

    session_input_activity();

    /* route to login/system overlays first */
    if (g_session.setup_wizard_visible || g_session.login_screen_visible || g_session.system_menu_visible) {
        if (login_overlay_handle_mouse(cx, cy, left && !prev_left)) {
            prev_left = left; prev_right = right; return;
        }
    }

    if (menu_handle_click(cx, cy, left && !prev_left)) {
        prev_left = left; prev_right = right; return;
    }
    if (drawer_open) {
        if (left && !prev_left) {
            if (drawer_hover >= 0) {
                drawer_open = false;
                drawer_query_len = 0; drawer_query[0] = 0;
                yart_app_catalog[drawer_hover].launch();
            } else {
                drawer_open = false;
                drawer_query_len = 0; drawer_query[0] = 0;
            }
        }
        prev_left = left; prev_right = right; return;
    }

    if (right && !prev_right) {
        /* dock right-click? */
        if (cy >= dock_y()) {
            for (int i = 0; i < nslots; i++) {
                dock_slot_t *s = &slots[i];
                if (cx < s->x || cx >= s->x + s->w ||
                    cy < s->y || cy >= s->y + s->h) continue;
                if (s->kind == SLOT_PINNED && s->app) {
                    open_pinned_menu(cx, cy, s->app);
                    prev_left = left; prev_right = right; return;
                }
                if (s->kind == SLOT_TASK && s->win) {
                    open_task_menu(cx, cy, s->win);
                    prev_left = left; prev_right = right; return;
                }
            }
        } else if (cy >= BAR_H) {
            window_t *w = window_at(cx, cy);
            if (!w) {
                open_desktop_menu(cx, cy);
                prev_left = left; prev_right = right; return;
            }
        }
    }
    prev_right = right;

    if (left && !prev_left) {
        if (cy >= dock_y()) {
            for (int i = 0; i < nslots; i++) {
                dock_slot_t *s = &slots[i];
                if (cx < s->x || cx >= s->x + s->w ||
                    cy < s->y || cy >= s->y + s->h) continue;
                if (s->kind == SLOT_DRAWER) drawer_open = true;
                else if (s->kind == SLOT_TASK && s->win) {
                    if (s->win == windows && (s->win->flags & WIN_FOCUS))
                        window_minimize(s->win);
                    else
                        window_restore(s->win);
                } else if (s->kind == SLOT_PINNED && s->app) {
                    s->app->launch();
                }
                prev_left = left; return;
            }
        }
        if (cy < BAR_H) {
            if (g_session.logged_in && cx >= (int)g_fb.width - 40) {
                g_session.system_menu_visible = true;
                prev_left = left; return;
            }
            prev_left = left; return;
        }

        window_t *w = window_at(cx, cy);
        if (w) {
            window_focus(w);
            hit_t h = hit_test(w, cx, cy);
            if (h == HIT_BODY) {
                g_files_click_x = g_term_click_x = g_editor_click_x = cx;
                g_files_click_y = g_term_click_y = g_editor_click_y = cy;
                g_files_click_pending = g_term_click_pending = g_editor_click_pending = true;
            }
            switch (h) {
            case HIT_BTN_CLOSE: window_close_requested(w); break;
            case HIT_BTN_MIN:   window_minimize(w);        break;
            case HIT_BTN_MAX:   window_maximize(w);        break;
            case HIT_TITLE:
                if (!(w->flags & WIN_NOMOVE) && !(w->flags & WIN_MAX)) {
                    dragging = w; drag_hit = HIT_TITLE;
                    drag_dx = cx - w->x; drag_dy = cy - w->y;
                }
                break;
            case HIT_RESIZE_R: case HIT_RESIZE_B: case HIT_RESIZE_BR:
            case HIT_RESIZE_L: case HIT_RESIZE_T: case HIT_RESIZE_BL:
            case HIT_RESIZE_TR: case HIT_RESIZE_TL:
                if (!(w->flags & (WIN_NORES | WIN_MAX))) {
                    dragging = w; drag_hit = h;
                    drag_dx = cx; drag_dy = cy;
                    drag_ox = w->x; drag_oy = w->y;
                    drag_ow = w->w; drag_oh = w->h;
                }
                break;
            default: break;
            }
        }
    }

    if (left && dragging) {
        window_t *w = dragging;
        switch (drag_hit) {
        case HIT_TITLE:
            w->x = cx - drag_dx;
            w->y = cy - drag_dy;
            if (w->y < BAR_H) w->y = BAR_H;
            break;
        case HIT_RESIZE_R: w->w = MAX(w->min_w, drag_ow + (cx - drag_dx)); break;
        case HIT_RESIZE_B: w->h = MAX(w->min_h, drag_oh + (cy - drag_dy)); break;
        case HIT_RESIZE_BR:
            w->w = MAX(w->min_w, drag_ow + (cx - drag_dx));
            w->h = MAX(w->min_h, drag_oh + (cy - drag_dy));
            break;
        case HIT_RESIZE_L: {
            int nw = drag_ow - (cx - drag_dx);
            if (nw >= w->min_w) { w->x = drag_ox + (cx - drag_dx); w->w = nw; }
            break;
        }
        case HIT_RESIZE_T: {
            int nh = drag_oh - (cy - drag_dy);
            if (nh >= w->min_h && drag_oy + (cy - drag_dy) >= BAR_H) {
                w->y = drag_oy + (cy - drag_dy); w->h = nh;
            }
            break;
        }
        case HIT_RESIZE_BL: {
            int nw = drag_ow - (cx - drag_dx);
            if (nw >= w->min_w) { w->x = drag_ox + (cx - drag_dx); w->w = nw; }
            w->h = MAX(w->min_h, drag_oh + (cy - drag_dy));
            break;
        }
        case HIT_RESIZE_TR: {
            int nh = drag_oh - (cy - drag_dy);
            if (nh >= w->min_h && drag_oy + (cy - drag_dy) >= BAR_H) {
                w->y = drag_oy + (cy - drag_dy); w->h = nh;
            }
            w->w = MAX(w->min_w, drag_ow + (cx - drag_dx));
            break;
        }
        case HIT_RESIZE_TL: {
            int nw = drag_ow - (cx - drag_dx);
            int nh = drag_oh - (cy - drag_dy);
            if (nw >= w->min_w) { w->x = drag_ox + (cx - drag_dx); w->w = nw; }
            if (nh >= w->min_h && drag_oy + (cy - drag_dy) >= BAR_H) {
                w->y = drag_oy + (cy - drag_dy); w->h = nh;
            }
            break;
        }
        default: break;
        }
    }

    if (!left && dragging) {
        window_t *w = dragging;
        if (drag_hit == HIT_TITLE) {
            if (cy <= BAR_H + 4)            window_maximize(w);
            else if (cx <= 4) {
                w->saved_x = w->x; w->saved_y = w->y;
                w->saved_w = w->w; w->saved_h = w->h;
                w->x = 0; w->y = BAR_H;
                w->w = g_fb.width / 2;
                w->h = g_fb.height - BAR_H - DOCK_H;
            } else if (cx >= (int)g_fb.width - 4) {
                w->saved_x = w->x; w->saved_y = w->y;
                w->saved_w = w->w; w->saved_h = w->h;
                w->x = g_fb.width / 2; w->y = BAR_H;
                w->w = g_fb.width / 2;
                w->h = g_fb.height - BAR_H - DOCK_H;
            }
        }
        dragging = 0;
    } else if (!left) {
        dragging = 0;
    }
    prev_left = left;
}

void desktop_handle_key(int sc, char ch, u32 mods) {
    g_dirty = true;
    session_input_activity();

    /* route to overlays first */
    if (g_session.setup_wizard_visible || g_session.login_screen_visible || g_session.system_menu_visible) {
        if (login_overlay_handle_key(sc, ch, mods)) return;
    }

    /* drawer: search */
    if (drawer_open) {
        if (sc == 0x01 || ch == 27) {
            drawer_open = false;
            drawer_query_len = 0; drawer_query[0] = 0;
            return;
        }
        if (ch == '\b' || ch == 8) {
            if (drawer_query_len) drawer_query[--drawer_query_len] = 0;
            return;
        }
        if (ch == '\n') {
            /* launch first match */
            for (int i = 0; i < yart_app_catalog_count; i++) {
                const yart_app_t *a = &yart_app_catalog[i];
                if (drawer_query_len == 0) {
                    drawer_open = false;
                    drawer_query_len = 0; drawer_query[0] = 0;
                    a->launch(); return;
                }
                /* substring match (case-insensitive) */
                const char *hs = a->name;
                bool match = false;
                for (const char *h = hs; *h && !match; h++) {
                    int k;
                    for (k = 0; drawer_query[k]; k++) {
                        char a_ = h[k], b_ = drawer_query[k];
                        if (a_ >= 'A' && a_ <= 'Z') a_ += 32;
                        if (b_ >= 'A' && b_ <= 'Z') b_ += 32;
                        if (a_ != b_) break;
                    }
                    if (drawer_query[k] == 0) match = true;
                }
                if (match) {
                    drawer_open = false;
                    drawer_query_len = 0; drawer_query[0] = 0;
                    a->launch(); return;
                }
            }
            return;
        }
        if (ch >= ' ' && ch < 127 && drawer_query_len < (int)sizeof drawer_query - 1) {
            drawer_query[drawer_query_len++] = ch;
            drawer_query[drawer_query_len] = 0;
        }
        return;
    }

    /* Ctrl+Alt+Left/Right => workspace switch */
    if ((mods & KEY_CTRL) && (mods & KEY_ALT)) {
        if (sc == 0x4B) {            /* left */
            current_ws = (current_ws + WORKSPACES - 1) % WORKSPACES;
            return;
        }
        if (sc == 0x4D) {            /* right */
            current_ws = (current_ws + 1) % WORKSPACES;
            return;
        }
    }
    /* Super (F1) opens drawer */
    if (sc == 0x3B) { drawer_open = true; return; }
    window_t *w = window_focused();
    if (w && w->on_key) w->on_key(w, sc, ch, mods);
}

/* ---------- night-light tint ---------- */
static void apply_night_light(void) {
    int s = g_config.display_night_light;
    if (s <= 0) return;
    if (s > 100) s = 100;
    /* shift blue down, red up: per row, multiply blue by (100-s)/100. */
    int blue_keep = 100 - (s * 60) / 100;
    int red_add   = (s * 20) / 100;
    for (u32 y = 0; y < g_fb.height; y++) {
        u32 *row = &g_fb.pixels[y * g_fb.pitch_px];
        for (u32 x = 0; x < g_fb.width; x++) {
            u32 c = row[x];
            u32 r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
            b = (b * blue_keep) / 100;
            r = r + (255 - r) * red_add / 100;
            if (r > 255) r = 255;
            row[x] = 0xFF000000U | (r << 16) | (g << 8) | b;
        }
    }
}

/* ---------- public lifecycle ---------- */
static bool g_apps_opened = false;

void desktop_init(void) {
    try_load_wallpaper();
    session_init();
    login_overlay_init();
    cursor_set_pos(g_fb.width / 2, g_fb.height / 2);
}

static bool any_animated(void) {
    if (drawer_open)   return true;
    if (menu_is_open()) return true;
    for (window_t *w = windows; w; w = w->next)
        if ((w->flags & WIN_ANIM) && (w->flags & WIN_VIS) && !(w->flags & WIN_MIN) && ws_of(w) == current_ws)
            return true;
    extern bool toasts_active(void);
    if (toasts_active()) return true;
    return false;
}

void desktop_render(void) {
    draw_wallpaper();
    draw_statusbar();
    window_t *stack[32]; int n = 0;
    for (window_t *w = windows; w && n < 32; w = w->next) stack[n++] = w;
    for (int i = n - 1; i >= 0; i--) draw_window(stack[i]);
    draw_dock();
    draw_drawer();
    menu_render();
    toasts_render(pit_ticks() * 10);
    apply_night_light();

    /* overlays */
    if (!g_session.logged_in) {
        if (g_session.setup_wizard_visible) draw_setup_wizard();
        else draw_login_screen();
    }
    if (g_session.system_menu_visible) draw_system_menu();

    cursor_draw();
    fb_present();
}

void desktop_tick(u64 ms) {
    if (drawer_open_request) { drawer_open = true; drawer_open_request = false; g_dirty = true; }
    mouse_event_t me;
    while (mouse_poll(&me)) {
        desktop_handle_mouse(me.dx, me.dy, me.buttons);
        g_dirty = true;
    }
    int ev;
    while ((ev = kbd_poll_event()) != 0) {
        if (ev & KEY_RELEASE) continue;
        u32 mods = ev & (KEY_SHIFT|KEY_CTRL|KEY_ALT);
        desktop_handle_key((ev >> 8) & 0xFF, (char)(ev & 0xFF), mods);
        g_dirty = true;
    }
    int interval = 1000 / (g_config.display_fps > 0 ? g_config.display_fps : 30);
    if (interval < 16) interval = 16;
    if (ms - g_last_frame_ms < (u64)interval) return;
    if (!g_dirty && !any_animated()) return;
    g_last_frame_ms = ms;
    g_dirty = false;
    desktop_render();

    login_overlay_tick();

    /* open default apps on first successful login */
    if (g_session.logged_in && !g_apps_opened) {
        g_apps_opened = true;
        const char *home = "/home/yart";
        if (g_session.current_user >= 0 && g_session.users[g_session.current_user].home[0])
            home = g_session.users[g_session.current_user].home;
        open_files(home);
        open_terminal();
        toast("Welcome to Yart OS");
        toast("F1 = drawer  Ctrl+Alt+arrows = workspace");
    }
}
