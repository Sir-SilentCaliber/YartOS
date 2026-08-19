/* Yart userland - photo cursor themes (parsed from the embedded blob). */
#ifndef YART_USER_CURSORS_H
#define YART_USER_CURSORS_H

#include "sys.h"
#include "cursor_assets.h"   /* generated: CURSOR_THEME_COUNT/KIND_* */

typedef struct {
    const u32 *px;      /* ARGB, pitch = w      */
    int w, h;
    int hotx, hoty;
    bool present;
} cursor_img_t;

typedef struct {
    cursor_img_t img[CURSOR_KIND_COUNT];
} cursor_theme_t;

/* Parse the embedded cursors.bin.  Returns the theme count (0 on error). */
int cursors_init(void);
/* Theme by index (0..count-1) or NULL. */
cursor_theme_t *cursors_theme(int t);
const char *cursors_theme_name(int t);
int cursors_theme_count(void);
/* Find a theme by name ("photo-white" etc.), -1 if unknown. */
int cursors_theme_by_name(const char *name);

/* ---- draw-size (pre-scaled) cursors ----
 * The raw cursors are packed at up to 48px and were bilinear-downscaled to
 * CURSOR_SCALE_NUM/CURSOR_SCALE_DEN with FLOAT math on EVERY frame in the
 * compositor - ~50k float ops/frame just for the cursor, which made the
 * pointer visibly lag/jump under emulation.  cursors_draw_img() pre-scales
 * each cursor ONCE (lazily, cached) into a straight-ARGB bitmap; the
 * compositor then blits it with a cheap integer alpha blend each frame. */
#define CURSOR_SCALE_NUM 4
#define CURSOR_SCALE_DEN 5
/* Returns a draw-ready (pre-scaled) cursor image, or NULL if not present. */
cursor_img_t *cursors_draw_img(int theme, int kind);

#endif
