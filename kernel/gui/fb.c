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
    for (u32 y = 0; y < g_fb.height; y++) {
        u64 *src = (u64 *)(g_fb.pixels + y * g_fb.pitch_px);
        u64 *dst = (u64 *)(g_fb.fb     + y * g_fb.pitch_px);
        u32 n = g_fb.pitch_px / 2;
        for (u32 i = 0; i < n; i++) dst[i] = src[i];
        if (g_fb.pitch_px & 1)
            ((u32 *)dst)[g_fb.pitch_px - 1] = ((u32 *)src)[g_fb.pitch_px - 1];
    }
}
