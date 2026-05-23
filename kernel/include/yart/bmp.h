/*
 * Yart OS - minimal 24/32-bit BMP decoder.
 *
 * Supports uncompressed BI_RGB (compression=0) Windows BMPs.
 * Output buffer is owned by the caller (use kmalloc).
 */
#pragma once
#include <yart/types.h>

typedef struct {
    u32 *pixels;     /* ARGB; alpha 0xFF unless 32-bit source had alpha */
    int  w, h;
} bmp_image_t;

/* Decode a BMP from raw bytes; returns 0 on success, fills *out.
 * Caller frees out->pixels with kfree() when done. */
int bmp_decode(const void *data, size_t len, bmp_image_t *out);

/* Load a BMP from the VFS path; returns 0 on success. */
int bmp_load(const char *path, bmp_image_t *out);

/* Render an image to the framebuffer at (x,y), nearest-neighbour scaled
 * to (dst_w, dst_h).  If dst_w/h <= 0, use the source size 1:1. */
void bmp_blit(const bmp_image_t *img, int x, int y, int dst_w, int dst_h);
