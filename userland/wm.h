/* YartOS compositor — shared internal header (Skift-style module split).
 *
 * wm.c was a single 1500-line monolith.  It is now split into focused
 * modules, each owning one screen concern, all sharing this header:
 *
 *   wm.c          orchestrator: main loop, input routing, cursor, backdrop,
 *                 config loading, process/app launching.
 *   wm_damage.c   dirty-rectangle list (Skift _dirty / _damage(r)).
 *   wm_windows.c  window state machine, chrome paint, animations, z-order.
 *   wm_dock.c     dock + desktop icons (static base scene + frosted glass).
 *   wm_panel.c    taskbar (black bar, search pill, clock, status cluster).
 *   wm_launcher.c app launcher panel + dock dropdown + search.
 *   wm_overlays.c context menu, quick settings, calendar, OSD, switcher,
 *                 overview.
 */
#ifndef YART_WM_H
#define YART_WM_H

#include "sys.h"
#include "gfx.h"
#include "kora.h"
#include "cursors.h"
#include "theme/theme.h"

/* ---- key flags ---- */
#define WM_KEY_CTRL    (1<<18)
#define WM_KEY_ALT     (1<<19)
#define WM_KEY_RELEASE (1<<16)
#define WM_KEY_EXT     (1<<20)

/* ---- limits + geometry ---- */
#define MAX_WIN 16
#define MAX_APPS 64
#define MAX_DOCK 16
#define MAX_DESKTOP 32
#define MAX_MENU 8
#define MAX_PIDS 24
#define MAX_DIRTY 32
#define MAX_WORKSPACES 9

extern int G_scale;             /* HiDPI UI scale (1 or 2) */
#define PANEL_H (40 * G_scale)
#define TB_H    (34 * G_scale)
#define DOCK_REST   (40 * G_scale)
#define DOCK_PITCH  (56 * G_scale)
#define DOCK_MARGIN (8  * G_scale)
#define OV_CW 300
#define OV_CH 190
#define OV_GAP 20

/* ---- types ---- */
typedef struct {
    int workspace;                 /* -1 = shown on all workspaces */
    int snap;                      /* 0=none 1=left 2=right 3=top/max */
    bool active, seen, dirty, maximized, minimized;
    bool closing;
    bool minimizing;
    bool hidden;                  /* moved off-screen by Show Desktop  */
    int hide_sx, hide_sy;         /* position to restore it to          */
    u32 id, owner;
    int x, y, w, h, saved_x, saved_y, saved_w, saved_h, z;
    long anim_start;
    int anim_from_x, anim_from_y, anim_from_w, anim_from_h;
    int min_to_x, min_to_y;
    int draw_dx, draw_dy;
    char title[32];
    char path[72];
    u64 va;
    int misses;
    GfxRect prev_draw;
    float scale;
    float open_prog;
    u8 alpha;
} win_t;

typedef struct { char name[40]; char path[72]; int icon; bool removable; } app_t;
typedef struct { char name[40]; char path[72]; int icon; bool core; } dock_t;
typedef struct { char name[40]; char path[72]; int icon; int kind; int gx, gy; } desk_t;
typedef struct { const char *label; int action; int arg; int icon; } menuitem_t;
typedef struct { int x0, y0, x1, y1, icon; const char *name; } tray_t;

enum { ACT_NONE, ACT_CLOSE_OVERLAY, ACT_TOGGLE_GRID, ACT_TOGGLE_OVERVIEW, ACT_TOGGLE_QUICK,
       ACT_TOGGLE_CAL, ACT_SHOW_DESKTOP, ACT_LAUNCH_APP, ACT_TOGGLE_MAX, ACT_CLOSE_WIN,
       ACT_PIN_APP, ACT_UNPIN_DOCK, ACT_ADD_DESKTOP, ACT_REMOVE_DESKTOP,
       ACT_OPEN_DESKTOP, ACT_OPEN_TERMINAL, ACT_CHANGE_WALLPAPER, ACT_DOCK_LAUNCH,
       ACT_WIN_FOCUS, ACT_CAL_PREV, ACT_CAL_NEXT, ACT_OPEN_TRASH, ACT_OPEN_HOME,
       ACT_MIN_WIN, ACT_RESTORE_WIN, ACT_SNAP_LEFT, ACT_SNAP_RIGHT, ACT_OPEN_SETTINGS };

/* ---- small helpers ---- */
static inline float anim_ease_out(float t){ if(t<0)t=0; if(t>1)t=1; return 1-(1-t)*(1-t)*(1-t); }
static inline float anim_ease_in_out(float t){ if(t<0)t=0; if(t>1)t=1; return t<0.5f?4*t*t*t:1-(-2*t+2)*(-2*t+2)*(-2*t+2)/2; }
static inline float anim_ease_out_back(float t){ if(t<0)t=0; if(t>1)t=1; float c1=1.70158f,c3=c1+1; t-=1; return 1+c3*t*t*t+c1*t*t; }
static inline int lerpi(int a,int b,float f){ return a+(int)((b-a)*f); }
static inline int mini(int a, int b){ return a<b?a:b; }
static inline int maxi(int a, int b){ return a>b?a:b; }
static inline int absi(int a){ return a<0?-a:a; }
static inline bool ptin(int x,int y,int x0,int y0,int x1,int y1){ return x>=x0&&x<x1&&y>=y0&&y<y1; }
static inline void copy_str(char *d,const char*s,int cap){ int i=0; while(s[i]&&i<cap-1){d[i]=s[i];i++;} d[i]=0; }

/* ---- shared framebuffer / backdrop ---- */
extern surface_t G_fb, G_wp;
extern surface_t G_backdrop;
extern bool g_backdrop_dirty;

/* ---- input state ---- */
extern int G_cx, G_cy, G_pressed_x, G_pressed_y;
extern unsigned char G_mb;
extern long G_start_ms;
extern char G_clk[16];
extern unsigned long G_last_sec;
extern char G_date[32];

/* ---- workspaces ---- */
extern int G_workspace;
extern int G_ws_count;

/* ---- windows ---- */
extern win_t G_win[MAX_WIN];
extern int G_z_top;
extern int G_focus_win;

/* ---- app registry ---- */
extern app_t G_app[MAX_APPS];
extern int G_apps;

/* ---- dock + desktop ---- */
extern dock_t G_dock[MAX_DOCK];
extern int G_dock_n;
extern bool G_core_state[MAX_DOCK];
extern desk_t G_desk[MAX_DESKTOP];
extern int G_desk_n;
extern surface_t G_icon_cache[MAX_DOCK];
extern int G_slot_sz[MAX_DOCK], G_slot_lift[MAX_DOCK], G_slot_target_sz[MAX_DOCK], G_slot_target_lift[MAX_DOCK], G_slot_bounce[MAX_DOCK];
extern int G_dock_x, G_dock_y, G_dock_w, G_dock_h;
extern surface_t G_dock_blur;
extern int G_blur_x, G_blur_y, G_blur_w, G_blur_h;
extern int G_dock_hover;
extern bool G_dock_tooltip, G_dock_tooltip_prev;

/* ---- panel ---- */
extern tray_t G_tray[8];
extern int G_tray_n;
extern int G_act_x0, G_act_x1, G_clock_x0, G_clock_x1, G_sys_x0, G_sys_x1;
extern int G_ws_x0, G_ws_x1;

/* ---- overlays ---- */
extern bool G_grid, G_overview, G_quick, G_calendar, G_dockmenu, G_switcher;
extern bool G_locked;
extern bool G_super_held;
extern bool G_lock_prompt;
extern char G_lock_pw[64];
extern int  G_lock_pw_len;
extern bool G_lock_bad;
extern bool G_dock_visible;
extern int G_switcher_idx;
extern int G_dockmenu_x, G_dockmenu_y, G_dockmenu_w, G_dockmenu_h;
extern int G_quick_x, G_quick_y, G_quick_w, G_quick_h, G_cal_x, G_cal_y, G_cal_w, G_cal_h;
extern int G_cal_month, G_cal_year;
extern char G_search[40];
extern int G_search_len;

/* ---- selection / drag ---- */
extern int G_sel_app, G_sel_desk;
extern bool G_double, G_multi_sel, G_desktop_drag, G_title_drag, G_marquee, G_icon_drag;
extern int G_drag_win, G_drag_dx, G_drag_dy;
extern int G_resize_win, G_resize_edges, G_drag_icon, G_icon_dx, G_icon_dy;
extern int G_mx0, G_my0, G_mx1, G_my1;
extern long G_last_click_desk, G_last_title_click;
extern int G_last_click_idx;

/* ---- context menu ---- */
extern bool G_menu;
extern long G_menu_t0;
extern int G_menu_x, G_menu_y, G_menu_w, G_menu_h, G_menu_type, G_menu_idx, G_menu_arg;
extern menuitem_t G_menu_items[MAX_MENU];
extern int G_menu_n;
extern char G_menu_path[72];

/* ---- quick settings state ---- */
extern bool G_audio, G_wifi, G_wired;
extern int G_vol;
extern int G_net_up;

/* ---- input language + clipboard + network list (panel extras) ---- */
extern bool G_clip_open;
extern char G_clipboard[512];
extern int  G_clipboard_len;
extern bool G_netlist_open;
extern char G_netlist[1024];
extern int  G_netlist_len;
extern int  G_net_x0, G_net_x1, G_clip_x0, G_clip_x1, G_lang_x0, G_lang_x1;
extern int  G_clip_x, G_clip_y, G_clip_w, G_clip_h;
extern int  G_nl_x, G_nl_y, G_nl_w, G_nl_h;

/* ---- tooltip / OSD / wallpaper / notifications ---- */
extern long G_tooltip_t0;
extern int G_tooltip_item;
extern long G_osd_t0;
extern char G_osd[80];
extern int G_wp_index;
extern char G_notifs[16][128];
extern int  G_notif_n;

/* ---- processes ---- */
typedef struct { long pid; char path[72]; } proc_t;
typedef struct { long pid; char path[72]; long stamp; } pending_t;
extern proc_t G_pids[MAX_PIDS];
extern int G_pids_n;
extern pending_t G_pending[MAX_PIDS];
extern int G_pending_n;

/* ---- damage (wm_damage.c) ---- */
extern GfxRect G_dirty[MAX_DIRTY];
extern int G_ndirty;
void damage_whole(void);
void damage_add(GfxRect r);
void damage_overlay_small(void);
GfxRect osd_rect(void);

/* ---- windows (wm_windows.c) ---- */
void win_draw_rect(win_t *w, GfxRect *out);
void win_shadow(int tx,int ty,int aw,int ah);
void draw_close_glyph(int cx,int cy,u32 c);
void draw_window(win_t *w);
void win_update(win_t *w, long now);
win_t *win_find(u32 id);
win_t *win_at(int x,int y);
void bring_front(win_t *w);
void close_win(win_t *w);
void toggle_max(win_t *w);
void scan_windows(void);
void minimize_win(win_t *w);
void restore_win(win_t *w);
void windows_update_and_damage(long now);
int count_windows_on_ws(void);
void win_snap(win_t *w, int side);

/* ---- dock + desktop (wm_dock.c) ---- */
void rebuild_dock_cache(void);
void add_dock_app(const char *path,const char *name,int icon,bool core);
void remove_dock(int idx);
int dock_find_path(const char *path);
void default_dock(void);
void dock_apply_hidden(void);
void dock_save_hidden(const char *path,bool hidden);
int dock_slot_x(int i);
int dock_hit(int x,int y);
void dock_update(long now);
void draw_dock(surface_t *s,long now);
void add_desktop_xy(const char *name,const char *path,int icon,int kind,int gx,int gy);
void add_desktop(const char *name,const char *path,int icon,int kind);
void desk_rect(int i,int*x0,int*y0,int*x1,int*y1);
int desk_hit(int x,int y);
bool desk_selected(int i);
void draw_desktop_icons(surface_t *s);
void draw_desktop_live(surface_t *s);

/* ---- panel (wm_panel.c) ---- */
void format_wallclock(long wt, char *buf);
void format_date(long wt,char *buf);
void draw_panel(surface_t *s);
int tray_hit(int x,int y);

/* ---- launcher (wm_launcher.c) ---- */
bool search_match(const char *name);
void draw_app_row(surface_t *s,int x,int y,int w,int h,app_t *a);
int app_grid_hit(int x,int y,int *idx);
int dockmenu_hit(int x,int y,int *idx);
void draw_dockmenu(surface_t *s);
void draw_app_grid(surface_t *s);

/* ---- overlays (wm_overlays.c) ---- */
void menu_open(int x,int y,int type,int idx);
void menu_close(void);
int menu_hit(int x,int y);
void menu_dispatch(int i);
void menu_open_win(int x,int y, win_t *w);
void draw_menu(surface_t *s);
void draw_quick(surface_t *s);
void draw_calendar(surface_t *s);
void draw_clipboard(surface_t *s);
void draw_netlist(surface_t *s);
void draw_osd(surface_t *s,long now);
void draw_switcher(surface_t *s);
void draw_overview(surface_t *s);
int overview_card_at(int x,int y);
void draw_lock(surface_t *s, long now);

/* ---- orchestrator (wm.c) ---- */
void osd(const char *m);
int icon_for_path(const char *p);
void add_app(const char *name,const char *path,int icon);
int app_index(const char *path);
void load_all(void);
void save_config(void);
void save_dock_only(void);
void pid_forget_dead(void);
void pid_record(long pid,const char *path);
bool pid_for_path(const char *path);
void launch_app(const char *path);
void show_desktop_toggle(void);

#endif
