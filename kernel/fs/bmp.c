/* Yart OS - 24/32-bit BMP decoder. */
#include <yart/bmp.h>
#include <yart/fs.h>
#include <yart/mm.h>
#include <yart/string.h>
#include <yart/console.h>
#include <yart/gui.h>

#pragma pack(push, 1)
typedef struct {
    u16 magic;
    u32 file_size;
    u16 r1, r2;
    u32 pixel_offset;
    u32 dib_size;
    i32 width;
    i32 height;
    u16 planes;
    u16 bpp;
    u32 compression;
    u32 image_size;
    i32 xppm, yppm;
    u32 colors;
    u32 important;
} bmp_hdr_t;
#pragma pack(pop)

int bmp_decode(const void *data, size_t len, bmp_image_t *out) {
    if (len < sizeof(bmp_hdr_t)) return -1;
    const bmp_hdr_t *h = data;
    if (h->magic != 0x4D42) {                            /* "BM" */
        kprintf("bmp: bad magic 0x%x\n", h->magic);
        return -1;
    }
    if (h->compression != 0) {
        kprintf("bmp: unsupported compression %lu\n", h->compression);
        return -1;
    }
    int w = h->width;
    int h_abs = h->height < 0 ? -h->height : h->height;
    bool top_down = (h->height < 0);
    int bpp = h->bpp;
    if (bpp != 24 && bpp != 32) {
        kprintf("bmp: unsupported bpp %d\n", bpp);
        return -1;
    }
    int row_stride = ((bpp / 8) * w + 3) & ~3;
    if (h->pixel_offset + row_stride * h_abs > len) {
        kprintf("bmp: truncated\n");
        return -1;
    }

    u32 *pixels = kmalloc((size_t)w * h_abs * 4);
    if (!pixels) return -1;

    const u8 *src = (const u8 *)data + h->pixel_offset;
    for (int j = 0; j < h_abs; j++) {
        int sy = top_down ? j : (h_abs - 1 - j);
        const u8 *row = src + sy * row_stride;
        u32 *dst = pixels + j * w;
        for (int i = 0; i < w; i++) {
            u8 b = row[i * (bpp/8) + 0];
            u8 g = row[i * (bpp/8) + 1];
            u8 r = row[i * (bpp/8) + 2];
            u8 a = (bpp == 32) ? row[i * 4 + 3] : 0xFF;
            if (bpp == 32 && a == 0) a = 0xFF; /* many BMPs leave alpha 0 */
            dst[i] = ((u32)a << 24) | ((u32)r << 16) | ((u32)g << 8) | b;
        }
    }
    out->pixels = pixels;
    out->w = w;
    out->h = h_abs;
    return 0;
}

int bmp_load(const char *path, bmp_image_t *out) {
    vnode_t *v = vfs_lookup(path);
    if (!v || v->type != VN_FILE || !v->data) return -1;
    return bmp_decode(v->data, v->size, out);
}

void bmp_blit(const bmp_image_t *img, int x, int y, int dst_w, int dst_h) {
    if (!img || !img->pixels) return;
    if (dst_w <= 0) dst_w = img->w;
    if (dst_h <= 0) dst_h = img->h;
    for (int j = 0; j < dst_h; j++) {
        int sy = (int)((long)j * img->h / dst_h);
        const u32 *src = &img->pixels[sy * img->w];
        for (int i = 0; i < dst_w; i++) {
            int sx = (int)((long)i * img->w / dst_w);
            u32 c = src[sx];
            /* alpha blend onto framebuffer */
            u32 a = (c >> 24) & 0xFF;
            if (a == 0) continue;
            if (a == 0xFF) draw_pixel(x + i, y + j, c);
            else {
                /* simple over-blend against current backbuffer */
                extern fb_ctx_t g_fb;
                int px = x + i, py = y + j;
                if ((u32)px >= g_fb.width || (u32)py >= g_fb.height) continue;
                u32 *dst = &g_fb.pixels[py * g_fb.pitch_px + px];
                u32 d = *dst;
                u32 sr = (c >> 16) & 0xFF, sg = (c >> 8) & 0xFF, sb = c & 0xFF;
                u32 dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
                u32 ia = 255 - a;
                u32 nr = (sr * a + dr * ia) >> 8;
                u32 ng = (sg * a + dg * ia) >> 8;
                u32 nb = (sb * a + db * ia) >> 8;
                *dst = 0xFF000000U | (nr << 16) | (ng << 8) | nb;
            }
        }
    }
}
