#pragma once
/* Yart GUI (kernel side) - framebuffer only.
 *
 * All window management, compositing, themes, chrome, input routing and
 * apps live in RING 3.  The kernel only owns the raw hardware framebuffer
 * (provided by Limine) and a single blit helper that the ring-3
 * compositor uses to present its rendered buffer to the scanout via
 * SYS_FB_FLIP.
 */
#include <yart/types.h>
#include <yart/limine.h>

typedef u32 color_t;

typedef struct {
    u32  *pixels;      /* back buffer (kernel alloc, maps into wm) */
    u32  *fb;          /* real scanout framebuffer (kernel only)   */
    u32   width, height;
    u32   pitch_px;
    u32   bpp;
    bool  rgb;
} fb_ctx_t;

extern fb_ctx_t g_fb;

void fb_init(struct limine_framebuffer *lfb);
void fb_present(void);

/* Primitive helpers kept in the kernel for boot-time recovery painting
 * (e.g. watchdog reset splash).  Compositor uses its own drawing code. */
void fb_clear(color_t c);
