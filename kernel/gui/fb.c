/* Yart OS - kernel-side framebuffer (minimal).
 *
 * All drawing / compositing is done in ring 3.  The kernel only:
 *   - maps the Limine-supplied hardware framebuffer,
 *   - allocates a back buffer (phys-contiguous, later mmap'd into wm),
 *   - provides fb_present() to copy the back buffer to the scanout.
 */
#include <yart/gui.h>
#include <yart/mm.h>
#include <yart/string.h>
#include <yart/console.h>

fb_ctx_t g_fb;

void fb_init(struct limine_framebuffer *lfb) {
    g_fb.fb       = (u32 *)lfb->address;
    g_fb.width    = lfb->width;
    g_fb.height   = lfb->height;
    g_fb.bpp      = lfb->bpp;
    g_fb.pitch_px = lfb->pitch / 4;
    g_fb.rgb      = (lfb->red_mask_shift > lfb->blue_mask_shift);

    size_t bytes = (size_t)g_fb.pitch_px * g_fb.height * 4;
    size_t pages = PAGE_ALIGN_UP(bytes) / PAGE_SIZE;
    paddr_t p = pmm_alloc_pages(pages);
    g_fb.pixels = (u32 *)phys_to_virt(p);
    fb_clear(0xFF10141A);
    kprintf("fb: %ux%u@%u  pitch=%u  rgb=%d\n",
            g_fb.width, g_fb.height, g_fb.bpp, g_fb.pitch_px, (int)g_fb.rgb);
}

void fb_clear(color_t c) {
    /* c is ARGB in kernel byte order.  The scanout might be BGRA, but for a
     * kernel-side clear (only used by watchdog splash) paint black. */
    u32 v = c;
    for (u32 y = 0; y < g_fb.height; y++) {
        u32 *row = g_fb.pixels + y * g_fb.pitch_px;
        for (u32 x = 0; x < g_fb.width; x++) row[x] = v;
    }
}

void fb_present(void) {
    fb_rect_t all = { 0, 0, g_fb.width, g_fb.height };
    fb_present_rects(&all, 1);
}

void fb_present_rects(const fb_rect_t *rects, u32 count) {
    for (u32 i = 0; i < count; i++) {
        u32 x = rects[i].x, y = rects[i].y;
        u32 w = rects[i].w, h = rects[i].h;
        if (x >= g_fb.width || y >= g_fb.height) continue;
        if (w == 0 || h == 0) continue;
        if (x + w > g_fb.width)  w = g_fb.width  - x;
        if (y + h > g_fb.height) h = g_fb.height - y;
        for (u32 yy = 0; yy < h; yy++) {
            u32 *src = g_fb.pixels + (y + yy) * g_fb.pitch_px + x;
            u32 *dst = g_fb.fb     + (y + yy) * g_fb.pitch_px + x;
            u32 n = w, k = 0;
            if ((((unsigned long)src | (unsigned long)dst) & 7) == 0) {
                u64 *s64 = (u64 *)src, *d64 = (u64 *)dst;
                for (; k + 1 < n; k += 2) d64[k / 2] = s64[k / 2];
            }
            for (; k < n; k++) dst[k] = src[k];
        }
    }
}
