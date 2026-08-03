/*
 * Yart OS - tiny ini-style config parsed from /etc/yart.conf.
 *
 * Schema we recognise (everything else is ignored):
 *
 *   hostname=yart
 *   theme=slate-amber
 *   accent=#E8A87C
 *   border=#E8A87C
 *   corner_radius=6
 *
 *   dock.auto_hide=0
 *   dock.position=bottom        # bottom|left|right
 *   dock.icon_size=32
 *   dock.spacing=12
 *   dock.pinned=Files,Term,Editor,Calc,Mon
 *
 *   topbar.height=26
 *   topbar.alpha=255
 *
 *   wallpaper.mode=image        # gradient|image
 *   wallpaper.path=/YartOS/kora/wallpaper.bmp
 *   wallpaper.index=0
 *
 *   font.system=default
 *   font.terminal=default
 *
 *   mouse.accel=10
 *   keyboard.repeat_delay=400
 *   keyboard.repeat_rate=33
 *
 *   display.fps=30
 *   display.night_light=0       # 0..100 strength
 *
 *   time.format24=1
 *   time.tz_offset=0
 *
 *   power.dim_after=300
 *   power.sleep_after=900
 */
#pragma once
#include <yart/types.h>

#define CONFIG_MAX_PINNED 12
#define CONFIG_STR_LEN    64

typedef struct {
    char     hostname[CONFIG_STR_LEN];
    char     theme[CONFIG_STR_LEN];
    u32      accent;            /* 0xAARRGGBB */
    u32      border;
    int      corner_radius;

    /* dock */
    bool     dock_auto_hide;
    char     dock_position[8];      /* "bottom" | "left" | "right"  */
    int      dock_icon_size;
    int      dock_spacing;
    char     dock_pinned[CONFIG_MAX_PINNED][CONFIG_STR_LEN];
    int      dock_pinned_count;

    /* top bar */
    int      topbar_height;
    int      topbar_alpha;

    /* wallpaper */
    char     wallpaper_mode[12];    /* "gradient" | "image" */
    char     wallpaper_path[128];
    int      wallpaper_index;      /* index into wallpaper pack */

    /* fonts */
    char     font_system[CONFIG_STR_LEN];
    char     font_terminal[CONFIG_STR_LEN];

    /* input */
    int      mouse_accel;
    int      keyboard_repeat_delay;
    int      keyboard_repeat_rate;

    /* display */
    int      display_fps;
    int      display_night_light;

    /* time */
    bool     time_format24;
    int      time_tz_offset;

    /* power */
    int      power_dim_after;
    int      power_sleep_after;
} yart_config_t;

extern yart_config_t g_config;

void config_load_defaults(void);
void config_load(const char *path);     /* parse + populate g_config */
int  config_save(const char *path);     /* serialize g_config back   */

/* pinned-app helpers */
bool config_is_pinned(const char *name);
void config_pin(const char *name);
void config_unpin(const char *name);
