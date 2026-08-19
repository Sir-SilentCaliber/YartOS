/* Minimal baseline JPEG decoder (ISO/IEC 10918-1, baseline DCT, 8-bit).
 *
 * Self-contained (no libc beyond memcpy/memset) so the SAME code runs in the
 * YartOS userland AND in a host unit test that compares its output against
 * Python PIL's decoder.  Supports:
 *   - 1-component (grayscale) and 3-component (YCbCr) images
 *   - 4:4:4, 4:2:2, 4:2:0 chroma subsampling
 *   - standard Huffman tables (from DHT) + standard quantization (from DQT)
 *   - restart markers are NOT handled (rare in MJPEG); stuffed 0xFF00 is.
 *
 * This is the decode core for the Motion-JPEG video player (roadmap #4) and,
 * because webcams stream MJPEG, the future camera app (roadmap #5).
 */
#pragma once

/* Decode a JPEG image into a 32-bit ARGB pixel buffer.
 *   jpeg: bytes, len: length
 *   out:  dst buffer (u32 pixels); if NULL, only the dimensions are returned
 *         (w_out/h_out) and no pixels are written (a cheap "probe").
 *   pitch: dst pitch in pixels
 *   max_w/max_h: clamp (0 = no clamp)
 * Returns 0 on success (fills *w_out, *h_out), -1 on error.
 * `out` must hold at least w*h pixels. */
int jpeg_decode(const unsigned char *jpeg, unsigned int len,
                unsigned int *out, unsigned int pitch,
                int max_w, int max_h,
                int *w_out, int *h_out);
