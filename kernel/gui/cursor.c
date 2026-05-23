/* Yart OS - mouse cursor drawn from a real ARGB asset. */
#include <yart/gui.h>

extern const u32 yart_cursor_pixels[];
extern const int yart_cursor_w;
extern const int yart_cursor_h;

static int cx = 200, cy = 200;

void cursor_set_pos(int x, int y) {
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if ((u32)x >= g_fb.width)  x = g_fb.width  - 1;
    if ((u32)y >= g_fb.height) y = g_fb.height - 1;
    cx = x; cy = y;
}

void cursor_get_pos(int *x, int *y) { *x = cx; *y = cy; }

void cursor_draw(void) {
    for (int j = 0; j < yart_cursor_h; j++)
        for (int i = 0; i < yart_cursor_w; i++) {
            u32 c = yart_cursor_pixels[j * yart_cursor_w + i];
            if (c & 0xFF000000) draw_pixel(cx + i, cy + j, c);
        }
}
