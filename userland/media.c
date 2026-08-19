/* /bin/media — a Motion-JPEG video player.
 *
 * Roadmap #4 "video support (decode + play)": decodes an embedded MJPEG clip
 * (concatenated baseline-JPEG frames) with userland/jpeg.c and plays it in a
 * loop in a window, upscaled to the surface.  MJPEG is the standard webcams
 * stream, so this same decode path backs the future camera app (roadmap #5).
 */
#include "sys.h"
#include "gfx.h"
#include "jpeg.h"

/* the embedded clip: [4-byte BE size][JPEG]... */
extern const char _binary_clip_mjpeg_start[];
extern const char _binary_clip_mjpeg_end[];

static int  s_wid;
static wm_surf_info_t s_info;
static surface_t s_surf;

static int video_main(void) {
    s_wid = (int)wm_create(640, 480, &s_info);
    if (s_wid < 0) return 1;
    s_surf.px = (u32 *)(unsigned long)s_info.app_va;
    s_surf.w = s_info.w; s_surf.h = s_info.h; s_surf.pitch = s_info.w;
    wm_title(s_wid, "Video Player");

    const unsigned char *clip = (const unsigned char *)_binary_clip_mjpeg_start;
    long clen = _binary_clip_mjpeg_end - _binary_clip_mjpeg_start;

    /* frame buffer at clip size (we decode into a fixed 160x120 surface and
     * upscale; the decoder is generic so any MJPEG frame size works) */
    surface_t frame = {0};

    long pos = 0;
    for (;;) {
        /* poll for input (Esc / close) */
        int ev;
        while ((ev = poll_key()) != 0) {
            int ascii = ev & 255, make = !(ev & (1 << 16));
            if (!make) continue;
            if (ascii == 27) return 0;          /* Esc exits */
        }

        /* read one frame size */
        if (pos + 4 > clen) pos = 0;            /* loop the clip */
        unsigned int fsz = ((unsigned int)(unsigned char)clip[pos] << 24) |
                           ((unsigned int)(unsigned char)clip[pos+1] << 16) |
                           ((unsigned int)(unsigned char)clip[pos+2] << 8) |
                            (unsigned int)(unsigned char)clip[pos+3];
        pos += 4;
        if (fsz == 0 || pos + (long)fsz > clen) { pos = 0; continue; }

        /* allocate the frame buffer at the clip's size on the first frame */
        int fw, fh;
        if (!frame.px) {
            if (jpeg_decode(clip + pos, fsz, NULL, 0, 0, 0, &fw, &fh) != 0) {
                pos += fsz; continue;
            }
            frame.px = (u32 *)mmap(((long)fw * fh * 4 + 4095) & ~4095L);
            frame.w = fw; frame.h = fh; frame.pitch = fw;
        }
        if (jpeg_decode(clip + pos, fsz, (unsigned int *)frame.px, frame.w, 0, 0, &fw, &fh) != 0) {
            pos += fsz; continue;               /* skip a bad frame */
        }
        pos += fsz;

        /* blit (upscale) into the window surface */
        sf_fill(&s_surf, RGB(0x10, 0x10, 0x14));
        sf_blit_scaled(&s_surf, 0, 0, s_surf.w, s_surf.h, &frame, 0, 0, frame.w, frame.h);
        wm_flip(s_wid);

        sleep(66);                              /* ~15 fps */
    }
}

int main_entry(int argc, char **argv, char **envp) {
    (void)argc; (void)argv; (void)envp;
    return video_main();
}
