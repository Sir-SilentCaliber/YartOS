#pragma once
#include <yart/types.h>
#include <yart/limine.h>
#include <yart/theme.h>
#include <yart/icons.h>

typedef u32 color_t;

typedef struct {
    u32  *pixels;
    u32  *fb;
    u32   width, height;
    u32   pitch_px;
    u32   bpp;
    bool  rgb;
} fb_ctx_t;

extern fb_ctx_t g_fb;

/* low-level */
void fb_init(struct limine_framebuffer *lfb);
void fb_present(void);
void fb_clear(color_t c);

/* primitives (operate on backbuffer) */
void draw_pixel(int x, int y, color_t c);
void draw_hline(int x, int y, int w, color_t c);
void draw_vline(int x, int y, int h, color_t c);
void draw_rect(int x, int y, int w, int h, color_t c);
void draw_rect_outline(int x, int y, int w, int h, color_t c);
void draw_rect_gradient_v(int x, int y, int w, int h, color_t top, color_t bot);
void draw_rounded_rect(int x, int y, int w, int h, int r, color_t c);
void draw_rounded_rect_outline(int x, int y, int w, int h, int r, color_t c);

void draw_char(int x, int y, char ch, color_t fg, color_t bg);
void draw_text(int x, int y, const char *s, color_t fg, color_t bg);
void draw_text_n(int x, int y, const char *s, int n, color_t fg, color_t bg);
int  text_width(const char *s);

/* font */
/* live pointer to the current 8x16 font (swappable via fb_set_font). */
extern const u8 (*yart_font8x16)[16];

/* font registry */
typedef struct { const char *name; const u8 (*data)[16]; } yart_font_t;
extern const yart_font_t yart_fonts[];
extern const int yart_fonts_count;
bool fb_set_font(const char *name);
#define FONT_W 8
#define FONT_H 16

/* ---------------- window manager ---------------- */
struct yart_window;
typedef struct yart_window window_t;

#define WIN_VIS    (1u << 0)
#define WIN_FOCUS  (1u << 1)
#define WIN_MIN    (1u << 2)
#define WIN_MAX    (1u << 3)
#define WIN_NORES  (1u << 4)   /* not resizable */
#define WIN_NOMOVE (1u << 5)
#define WIN_MODAL  (1u << 6)
#define WIN_ANIM   (1u << 7)   /* paint every frame (animated content) */

void wm_dirty(void);            /* mark compositor as needing a redraw */



/* shared chrome metrics */
#define WIN_TITLE_H 24
#define EDGE_GRAB    6
struct yart_window {
    int x, y, w, h;
    int saved_x, saved_y, saved_w, saved_h;   /* for restore from max */
    int min_w, min_h;
    char title[64];
    icon_id_t icon;
    u32  flags;
    color_t bg;
    void  *ud;                                /* per-app state */
    void (*paint)(window_t *win);
    void (*on_key)(window_t *win, int sc, int ch, u32 mods);
    void (*on_scroll)(window_t *win, int dy); /* mouse wheel / PgUp-PgDn  */
    void (*on_close)(window_t *win);
    window_t *next;
};

void desktop_init(void);
void desktop_tick(u64 ms);
void desktop_render(void);

window_t *window_create(const char *title, icon_id_t icon,
                        int x, int y, int w, int h,
                        void (*paint)(window_t *));
void      window_destroy(window_t *w);
void      window_focus(window_t *w);
window_t *window_focused(void);
void      window_minimize(window_t *w);
void      window_maximize(window_t *w);
void      window_restore(window_t *w);
void      window_close_requested(window_t *w);

/* mouse cursor */
void cursor_set_pos(int x, int y);
void cursor_get_pos(int *x, int *y);
void cursor_draw(void);

/* input bridge from desktop loop */
void desktop_handle_key(int scancode, int ascii, u32 mods);
void desktop_handle_mouse(int dx, int dy, u8 buttons, int wheel);
