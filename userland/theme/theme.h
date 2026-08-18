/* YartOS Theme Engine - semantic colors for the whole desktop.
 *
 * Every GUI component reads its colors from the active theme instead of
 * hardcoding RGB values. A theme is a set of named u32 ARGB colors; it
 * can be loaded from /home/yart/theme.ini (key=value) at startup and
 * reloaded at runtime (e.g. a hotkey) so the desktop is customizable
 * the way Hyprland/Sway configs are, without a CSS engine.
 *
 * Adding a new color: add a T_xxx entry to the enum, a default in
 * theme_reset_defaults(), and (optionally) a parse alias in theme_load().
 */
#ifndef YART_THEME_H
#define YART_THEME_H
#include "sys.h"

typedef enum {
    /* surfaces */
    T_PANEL_BG = 0, T_DOCK_BG, T_DOCK_SHADOW, T_DESKTOP_SEL,
    T_WIN_BG, T_WIN_TITLE, T_WIN_BORDER, T_WIN_SHADOW,
    T_OVERLAY_BG, T_OVERLAY_SURFACE, T_MENU_BG, T_MENU_HOVER,
    T_TOOLTIP_BG, T_GRID_BG, T_SEARCH_BG,
    /* chrome buttons */
    T_BTN_CLOSE, T_BTN_CLOSE_GLYPH,
    T_BTN_MIN, T_BTN_MIN_GLYPH,
    T_BTN_MAX, T_BTN_MAX_GLYPH,
    T_BTN_TOGGLE_ON, T_BTN_TOGGLE_OFF,
    /* text */
    T_TEXT, T_TEXT_DIM, T_TEXT_FAINT, T_TEXT_ON_ACCENT,
    /* accents / state */
    T_ACCENT, T_ACCENT_DIM, T_DANGER, T_FOLDER,
    /* cursor */
    T_CURSOR_OUTLINE,
    T__COUNT
} theme_color_id;

typedef struct {
    u32 c[T__COUNT];
} theme_t;

/* Reset g_theme to the built-in default (Yart Dark). */
void theme_reset_defaults(void);

/* Load overrides from an ini file. Missing keys keep their default.
 * Returns 0 on success, negative on error (file missing is OK). */
int  theme_load(const char *path);

/* Save the current theme to a file (for a settings app later). */
int  theme_save(const char *path);

/* Get a color by id. */
static inline u32 theme(theme_color_id id){
    extern theme_t g_theme;
    return (id >= 0 && id < T__COUNT) ? g_theme.c[id] : 0xFFFFFFFFu;
}

/* Parse "#rrggbb" / "#aarrggbb" / "r,g,b" into ARGB. Returns 0 on error. */
u32  theme_parse_color(const char *s);

#endif
