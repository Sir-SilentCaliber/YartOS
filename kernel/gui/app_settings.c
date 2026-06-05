/* Yart OS - Settings panel (rewrite for Stage 10).
 *
 * Tabs:
 *   Appearance | Dock | Top Bar | Wallpaper | Fonts |
 *   Mouse/Kbd  | Display | Time | Power
 *
 * Every control mutates g_config and immediately persists with
 * config_save("/etc/yart.conf"). The desktop notices the change next
 * frame because wm_dirty() is called.
 */
#include <yart/gui.h>
#include <yart/theme.h>
#include <yart/string.h>
#include <yart/mm.h>
#include <yart/config.h>
#include <yart/session.h>
#include <yart/icons.h>
#include <yart/menu.h>

extern int  g_editor_click_x, g_editor_click_y;
extern bool g_editor_click_pending;
extern bool fb_set_font(const char *name);

typedef enum {
    TAB_APPEARANCE, TAB_DOCK, TAB_TOPBAR, TAB_WALL,
    TAB_FONTS, TAB_MOUSE, TAB_DISPLAY, TAB_TIME, TAB_POWER, TAB_USERS,
    TAB_COUNT
} settings_tab_t;

static const char *tab_names[TAB_COUNT] = {
    "Appearance", "Dock", "Top Bar", "Wallpaper",
    "Fonts", "Input", "Display", "Time", "Power", "Users"
};

typedef struct {
    settings_tab_t tab;
} settings_state_t;

/* ---- helpers: simple immediate-mode controls ---- */
static bool clicked_rect(int x, int y, int w, int h) {
    if (!g_editor_click_pending) return false;
    int cx = g_editor_click_x, cy = g_editor_click_y;
    if (cx >= x && cx < x + w && cy >= y && cy < y + h) {
        g_editor_click_pending = false;
        return true;
    }
    return false;
}

static void persist(void) {
    config_save("/etc/yart.conf");
    wm_dirty();
}

/* slider that returns true if value changed */
static bool slider(int x, int y, int w, int *val, int lo, int hi, const char *label) {
    draw_text(x, y, label, TH_TEXT, 0);
    int ty = y + FONT_H + 4;
    draw_rounded_rect(x, ty + 4, w, 6, 3, TH_PANEL_HI);
    int range = hi - lo;
    int pos = range > 0 ? ((*val - lo) * w / range) : 0;
    draw_rect(x, ty + 4, pos, 6, g_config.accent);
    /* draggable knob */
    int kx = x + pos - 5;
    draw_rect(kx, ty, 10, 14, g_config.accent);
    /* click-on-track sets value */
    if (clicked_rect(x, ty - 4, w, 22)) {
        int p = g_editor_click_x - x;
        if (p < 0) p = 0;
        if (p > w) p = w;
        int nv = lo + p * range / w;
        if (nv != *val) { *val = nv; return true; }
    }
    char num[16];
    snprintf(num, sizeof num, "%d", *val);
    draw_text(x + w + 12, y + FONT_H, num, TH_TEXT_DIM, 0);
    return false;
}

/* toggle that returns true if changed */
static bool toggle(int x, int y, bool *val, const char *label) {
    draw_text(x, y, label, TH_TEXT, 0);
    int bx = x + 200;
    color_t bg = *val ? g_config.accent : TH_PANEL_HI;
    draw_rounded_rect(bx, y, 40, 18, 9, bg);
    int kx = *val ? bx + 22 : bx + 2;
    draw_rounded_rect(kx, y + 2, 16, 14, 7, TH_TEXT);
    if (clicked_rect(bx, y - 2, 40, 22)) { *val = !*val; return true; }
    return false;
}

/* segmented choice: returns index that changed (or -1) */
static int seg_choice(int x, int y, int w, const char **opts, int n,
                      const char *selected) {
    int seg = w / n;
    int hit = -1;
    for (int i = 0; i < n; i++) {
        bool active = (strcmp(opts[i], selected) == 0);
        color_t bg = active ? g_config.accent : TH_PANEL_HI;
        color_t fg = active ? TH_TEXT_INV : TH_TEXT;
        draw_rounded_rect(x + i * seg, y, seg - 2, 22, 4, bg);
        draw_rounded_rect_outline(x + i * seg, y, seg - 2, 22, 4, TH_WIN_BORDER);
        int tw = text_width(opts[i]);
        draw_text(x + i * seg + (seg - tw)/2, y + 4, opts[i], fg, 0);
        if (clicked_rect(x + i * seg, y, seg - 2, 22)) hit = i;
    }
    return hit;
}

/* color swatch row: 8 preset accents */
static u32 preset_colors[] = {
    0xFFE8A87C, /* warm amber (default) */
    0xFF8FBC8F, /* sage */
    0xFFCC7777, /* dusty rose */
    0xFFC299E0, /* lavender */
    0xFF7ABBC9, /* teal */
    0xFFE0C088, /* honey */
    0xFFE6E8EE, /* white */
    0xFF7A3CFF, /* neon purple */
};
static bool color_picker(int x, int y, u32 *target, const char *label) {
    draw_text(x, y, label, TH_TEXT, 0);
    int ty = y + FONT_H + 6;
    int sw = 28, gap = 6;
    bool changed = false;
    for (int i = 0; i < (int)(sizeof preset_colors / sizeof preset_colors[0]); i++) {
        int sx = x + i * (sw + gap);
        draw_rounded_rect(sx, ty, sw, sw, 4, preset_colors[i]);
        if (preset_colors[i] == *target)
            draw_rounded_rect_outline(sx - 2, ty - 2, sw + 4, sw + 4, 5, TH_TEXT);
        if (clicked_rect(sx, ty, sw, sw)) { *target = preset_colors[i]; changed = true; }
    }
    return changed;
}

/* ---- per-tab content ---- */
static void tab_appearance(window_t *w, int x, int y, int W) {
    if (color_picker(x, y, &g_config.accent, "Accent color")) persist();
    y += 70;
    if (color_picker(x, y, &g_config.border, "Focused border color")) persist();
    y += 70;
    if (slider(x, y, W - 200, &g_config.corner_radius, 0, 16, "Corner radius")) persist();
}

static void tab_dock(window_t *w, int x, int y, int W) {
    static const char *positions[] = { "bottom", "left", "right" };
    draw_text(x, y, "Position", TH_TEXT, 0); y += FONT_H + 4;
    int sc = seg_choice(x, y, W - 60, positions, 3, g_config.dock_position);
    if (sc >= 0) { strncpy(g_config.dock_position, positions[sc], 7); persist(); }
    y += 36;
    if (slider(x, y, W - 200, &g_config.dock_icon_size, 24, 48, "Icon size (px)")) persist();
    y += 50;
    if (slider(x, y, W - 200, &g_config.dock_spacing, 4, 32, "Spacing (px)")) persist();
    y += 50;
    if (toggle(x, y, &g_config.dock_auto_hide, "Auto-hide dock")) persist();
    y += 30;
    /* pinned list */
    draw_text(x, y, "Pinned apps:", TH_TEXT_DIM, 0); y += FONT_H + 4;
    char buf[200] = {0};
    for (int i = 0; i < g_config.dock_pinned_count; i++) {
        int l = strlen(buf);
        snprintf(buf + l, sizeof buf - l, "%s%s",
                 i ? ", " : "", g_config.dock_pinned[i]);
    }
    draw_text(x, y, buf, g_config.accent, 0);
    y += FONT_H + 6;
    draw_text(x, y, "(right-click a dock icon to pin/unpin)", TH_TEXT_MUTED, 0);
}

static void tab_topbar(window_t *w, int x, int y, int W) {
    if (slider(x, y, W - 200, &g_config.topbar_height, 20, 40, "Height (px)")) persist();
    y += 50;
    if (slider(x, y, W - 200, &g_config.topbar_alpha, 128, 255, "Opacity")) persist();
}

static void tab_wall(window_t *w, int x, int y, int W) {
    static const char *modes[] = { "gradient", "image" };
    draw_text(x, y, "Wallpaper mode", TH_TEXT, 0); y += FONT_H + 4;
    int sc = seg_choice(x, y, W - 60, modes, 2, g_config.wallpaper_mode);
    if (sc >= 0) { strncpy(g_config.wallpaper_mode, modes[sc], 11); persist(); }
    y += 36;
    draw_text(x, y, "Image path:", TH_TEXT_DIM, 0); y += FONT_H + 4;
    draw_text(x, y, g_config.wallpaper_path, g_config.accent, 0);
    y += FONT_H + 16;
    draw_text(x, y, "(BMP 24/32-bit, uncompressed)", TH_TEXT_MUTED, 0);
}

static void tab_fonts(window_t *w, int x, int y, int W) {
    extern const yart_font_t yart_fonts[];
    extern const int yart_fonts_count;

    draw_text(x, y, "System font", TH_TEXT, 0); y += FONT_H + 4;
    /* draw choices */
    const char *names[8];
    int n = yart_fonts_count > 8 ? 8 : yart_fonts_count;
    for (int i = 0; i < n; i++) names[i] = yart_fonts[i].name;
    int sc = seg_choice(x, y, W - 60, names, n, g_config.font_system);
    if (sc >= 0) {
        strncpy(g_config.font_system, names[sc], CONFIG_STR_LEN-1);
        fb_set_font(g_config.font_system);
        persist();
    }
    y += 36;
    draw_text(x, y, "Terminal font", TH_TEXT, 0); y += FONT_H + 4;
    int sc2 = seg_choice(x, y, W - 60, names, n, g_config.font_terminal);
    if (sc2 >= 0) {
        strncpy(g_config.font_terminal, names[sc2], CONFIG_STR_LEN-1);
        persist();
    }
    y += 40;
    draw_text(x, y, "Preview: The quick brown fox jumps over the lazy dog 0123456789",
              TH_TEXT, 0);
}

static void tab_mouse(window_t *w, int x, int y, int W) {
    if (slider(x, y, W - 200, &g_config.mouse_accel, 1, 20, "Mouse acceleration")) persist();
    y += 50;
    if (slider(x, y, W - 200, &g_config.keyboard_repeat_delay, 100, 1000, "Key repeat delay (ms)")) persist();
    y += 50;
    if (slider(x, y, W - 200, &g_config.keyboard_repeat_rate, 10, 100, "Key repeat rate (cps)")) persist();
}

static void tab_display(window_t *w, int x, int y, int W) {
    if (slider(x, y, W - 200, &g_config.display_fps, 10, 60, "Target FPS")) persist();
    y += 50;
    if (slider(x, y, W - 200, &g_config.display_night_light, 0, 100, "Night light (%)")) persist();
    y += 60;
    char buf[64];
    snprintf(buf, sizeof buf, "Resolution: %ux%u  Depth: %u",
             g_fb.width, g_fb.height, g_fb.bpp);
    draw_text(x, y, buf, TH_TEXT_DIM, 0);
}

static void tab_time(window_t *w, int x, int y, int W) {
    if (toggle(x, y, &g_config.time_format24, "Use 24-hour clock")) persist();
    y += 36;
    if (slider(x, y, W - 200, &g_config.time_tz_offset, -12, 14, "Timezone offset (h)")) persist();
}

static void tab_power(window_t *w, int x, int y, int W) {
    if (slider(x, y, W - 200, &g_config.power_dim_after, 30, 1800, "Dim after (s)")) persist();
    y += 50;
    if (slider(x, y, W - 200, &g_config.power_sleep_after, 60, 3600, "Sleep after (s)")) persist();
}


static void tab_users(window_t *w, int x, int y, int W) {
    (void)w;
    draw_text(x, y, "Registered users:", TH_ACCENT, 0); y += FONT_H + 8;
    if (g_session.user_count == 0) {
        draw_text(x, y, "No users configured.", TH_TEXT_DIM, 0);
        return;
    }
    for (int i = 0; i < g_session.user_count; i++) {
        char buf[128];
        snprintf(buf, sizeof buf, "  %s%s  home: %s",
                 g_session.users[i].username,
                 g_session.users[i].is_admin ? " (admin)" : "",
                 g_session.users[i].home);
        draw_text(x, y, buf, TH_TEXT, 0); y += FONT_H + 4;
    }
    y += 8;
    draw_text(x, y, "Users are stored in /etc/users.conf.", TH_TEXT_MUTED, 0);
}

/* ---- main paint ---- */

static void settings_paint(window_t *w) {
    settings_state_t *st = w->ud;
    int x = w->x + 8, y = w->y + WIN_TITLE_H + 8;
    int W = w->w - 16, H = w->h - WIN_TITLE_H - 16;
    draw_rounded_rect(x, y, W, H, 4, TH_WIN_BG);
    draw_rounded_rect_outline(x, y, W, H, 4, TH_WIN_BORDER);

    /* tab strip on the left */
    int strip_w = 130;
    draw_rect(x + 1, y + 1, strip_w, H - 2, TH_WIN_BG_ALT);
    draw_vline(x + strip_w, y + 1, H - 2, TH_WIN_BORDER);
    int ty = y + 10;
    for (int i = 0; i < TAB_COUNT; i++) {
        bool active = (st->tab == (settings_tab_t)i);
        if (active) draw_rect(x + 4, ty - 2, strip_w - 8, 24, TH_ACCENT_BG);
        draw_text(x + 14, ty + 2, tab_names[i], active ? g_config.accent : TH_TEXT, 0);
        if (clicked_rect(x + 4, ty - 2, strip_w - 8, 24)) st->tab = i;
        ty += 28;
    }

    /* content */
    int cx_ = x + strip_w + 20;
    int cy_ = y + 20;
    int cW = W - strip_w - 40;

    char title[32];
    snprintf(title, sizeof title, "%s", tab_names[st->tab]);
    draw_text(cx_, cy_, title, g_config.accent, 0); cy_ += FONT_H + 14;

    switch (st->tab) {
    case TAB_APPEARANCE: tab_appearance(w, cx_, cy_, cW); break;
    case TAB_DOCK:       tab_dock      (w, cx_, cy_, cW); break;
    case TAB_TOPBAR:     tab_topbar    (w, cx_, cy_, cW); break;
    case TAB_WALL:       tab_wall      (w, cx_, cy_, cW); break;
    case TAB_FONTS:      tab_fonts     (w, cx_, cy_, cW); break;
    case TAB_MOUSE:      tab_mouse     (w, cx_, cy_, cW); break;
    case TAB_DISPLAY:    tab_display   (w, cx_, cy_, cW); break;
    case TAB_TIME:       tab_time      (w, cx_, cy_, cW); break;
    case TAB_POWER:      tab_power     (w, cx_, cy_, cW); break;
    case TAB_USERS:      tab_users     (w, cx_, cy_, cW); break;
    default: break;
    }

    /* footer hint */
    draw_text(x + strip_w + 20, y + H - 22,
              "Changes are saved to /etc/yart.conf automatically.",
              TH_TEXT_MUTED, 0);

    /* clear any unconsumed click */
    g_editor_click_pending = false;
}

void open_settings(void) {
    settings_state_t *st = kzalloc(sizeof *st);
    int W = 720, H = 460;
    int x = (g_fb.width - W) / 2;
    int y = (g_fb.height - H) / 2;
    window_t *win = window_create("Settings", ICON_CONFIG, x, y, W, H, settings_paint);
    if (!win) { kfree(st); return; }
    win->ud = st;
    win->min_w = 600; win->min_h = 380;
    win->flags |= WIN_ANIM;     /* live preview */
}
