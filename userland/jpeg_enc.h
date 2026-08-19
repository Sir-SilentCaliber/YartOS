/* Minimal baseline JPEG ENCODER (ISO/IEC 10918-1, baseline, 8-bit).
 *
 * Complements jpeg.c (the decoder).  Encodes an RGB (ARGB u32) framebuffer to
 * a baseline JPEG with 4:2:0 chroma subsampling, standard quantization tables
 * (quality-scaled), and OPTIMAL Huffman tables computed per image (like a real
 * encoder; smaller files, self-contained, no hardcoded code tables).
 *
 * Freestanding-safe: no malloc, no libm (hardcoded 8x8 cosine table).
 * This powers screenshots, screen recording, and camera photo/video.
 */
#pragma once

/* Encode RGB pixels (u32 ARGB, `pitch` pixels per row) to a JPEG in `out`
 * (capacity `cap` bytes).  quality 1..100 (higher = better/larger).
 * Returns the encoded length, or -1 on error (buffer too small). */
int jpeg_encode(const unsigned int *pixels, int w, int h, int pitch,
                int quality, unsigned char *out, unsigned int cap);
