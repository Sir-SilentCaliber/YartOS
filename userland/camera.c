/* /bin/camera — YartOS camera app (roadmap #5: media viewer + camera).
 *
 * HONEST: QEMU has no camera hardware, so the "sensor" is a procedurally
 * generated animated test pattern (SMPTE-style color bars + a moving target
 * box + a frame counter).  This is the SAME honesty pattern as the battery
 * and wifi indicators.  What is REAL is the encode pipeline: frames are
 * JPEG-encoded with userland/jpeg_enc.c, and recording is a Motion-JPEG
 * stream (the exact format real webcams deliver) — so when a USB webcam
 * driver lands, only sensor_frame() needs replacing.
 *
 * Controls:  Space = capture photo   R = start/stop recording   Esc = quit
 * Photos  -> /home/yart/Pictures/cam_N.jpg
 * Video   -> /home/yart/Videos/cam_N.mjpeg
 */
#include "sys.h"
#include "gfx.h"
#include "jpeg_enc.h"
#include "fsutil.h"

/* sensor frame is a multiple of 16 (the encoder's requirement) */
#define SENSOR_W 320
#define SENSOR_H 240

static int         s_wid;
static wm_surf_info_t s_info;
static surface_t   s_surf;   /* window canvas (640x480) */
static u32         s_px[SENSOR_W * SENSOR_H];   /* sensor ARGB */

static long s_frame;          /* animation frame counter */
static int  s_recording;      /* 0/1 */
static int  s_rec_fd;         /* open MJPEG file while recording */
static int  s_rec_frames;     /* frames written this recording */
static char s_status[64];

/* SMPTE-ish color bars (7 bars + 1) */
static const u32 s_bar[8] = {
    RGB(0xC0,0xC0,0xC0), RGB(0xC0,0xC0,0x00), RGB(0x00,0xC0,0xC0),
    RGB(0x00,0xC0,0x00), RGB(0xC0,0x00,0xC0), RGB(0xC0,0x00,0x00),
    RGB(0x00,0x00,0xC0), RGB(0x20,0x20,0x30),
};

/* render one sensor frame (animated test pattern) into s_px */
static void sensor_frame(long t) {
    /* scrolling color bars (vertical) */
    int shift = (int)(t / 3) % (SENSOR_W / 4);
    for (int y = 0; y < SENSOR_H; y++) {
        for (int x = 0; x < SENSOR_W; x++) {
            int bar = ((x + shift) * 8) / SENSOR_W;
            u32 c = s_bar[bar & 7];
            /* gentle vertical brightness ramp so it reads as a real image */
            int lum = (y * 24) / SENSOR_H - 12;
            int r = (int)((c >> 16) & 255) + lum;
            int g = (int)((c >> 8) & 255) + lum;
            int b = (int)(c & 255) + lum;
            if (r < 0) r = 0;
            if (r > 255) r = 255;
            if (g < 0) g = 0;
            if (g > 255) g = 255;
            if (b < 0) b = 0;
            if (b > 255) b = 255;
            s_px[y * SENSOR_W + x] = RGB((u8)r, (u8)g, (u8)b);
        }
    }
    /* moving target box (a "subject") */
    int bw = 56, bh = 56;
    int bx = ((int)(t / 4) * 5) % (SENSOR_W - bw);
    int by = SENSOR_H / 2 - bh / 2 + (int)((t / 20) % 3) * 12 - 12;
    for (int y = by; y < by + bh; y++) {
        for (int x = bx; x < bx + bw; x++) {
            if (x < 0 || y < 0 || x >= SENSOR_W || y >= SENSOR_H) continue;
            u32 c = RGB(0xFF, 0xFF, 0xFF);
            if (x < bx + 3 || x >= bx + bw - 3 || y < by + 3 || y >= by + bh - 3)
                c = RGB(0x20, 0x20, 0x20);
            s_px[y * SENSOR_W + x] = c;
        }
    }
}

static void blit_viewfinder(void) {
    /* upscale the sensor into the window, letterboxed on a dark bezel */
    sf_fill(&s_surf, RGB(0x14, 0x14, 0x18));
    surface_t src = { s_px, SENSOR_W, SENSOR_H, SENSOR_W };
    int dw = s_surf.w, dh = (s_surf.w * SENSOR_H) / SENSOR_W;
    if (dh > s_surf.h - 48) { dh = s_surf.h - 48; dw = (dh * SENSOR_W) / SENSOR_H; }
    int dx = (s_surf.w - dw) / 2, dy = 6;
    sf_blit_scaled(&s_surf, dx, dy, dw, dh, &src, 0, 0, SENSOR_W, SENSOR_H);
    sf_rect_outline(&s_surf, dx - 1, dy - 1, dw + 2, dh + 2, RGB(0x40, 0x44, 0x50));

    /* control bar */
    int by = s_surf.h - 44;
    sf_fill_rect(&s_surf, 0, by, s_surf.w, 44, RGB(0x1E, 0x1E, 0x24));
    sf_hline(&s_surf, 0, by, s_surf.w, RGB(0x34, 0x38, 0x42));
    sf_text(&s_surf, 12, by + 14, "Space: Photo   R: Record   Esc: Close",
            RGB(0xC0, 0xC4, 0xCC));
    u32 rec_col = s_recording ? RGB(0xFF, 0x55, 0x55) : RGB(0x60, 0xC0, 0x60);
    sf_text(&s_surf, s_surf.w - sf_text_width(s_status) - 12, by + 14,
            s_status, rec_col);
    /* blinking REC dot while recording */
    if (s_recording && ((s_frame / 5) & 1)) {
        sf_fill_rect(&s_surf, 12, by + 8, 10, 10, RGB(0xFF, 0x40, 0x40));
    }
}

static void do_photo(void) {
    fs_mkdir_p("/home/yart/Pictures");
    char path[96];
    fs_next_free("/home/yart/Pictures", "cam_", ".jpg", path, sizeof path);
    unsigned char *jpg = (unsigned char *)mmap(((SENSOR_W * SENSOR_H * 2) + 4095) & ~4095L);
    if (!jpg) { fs_copystr(s_status, "photo: out of memory", sizeof s_status); return; }
    int len = jpeg_encode(s_px, SENSOR_W, SENSOR_H, SENSOR_W, 85, jpg,
                          SENSOR_W * SENSOR_H * 2);
    if (len > 0 && fs_write_file(path, jpg, len) == 0) {
        fs_copystr(s_status, "saved photo", sizeof s_status);
        notify("Camera: photo saved");
    } else {
        fs_copystr(s_status, "photo: encode failed", sizeof s_status);
    }
    munmap((long)jpg);
}

static void do_record_toggle(void) {
    if (!s_recording) {
        fs_mkdir_p("/home/yart/Videos");
        char path[96];
        fs_next_free("/home/yart/Videos", "cam_", ".mjpeg", path, sizeof path);
        s_rec_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
        if (s_rec_fd < 0) {
            fs_copystr(s_status, "record: open failed", sizeof s_status);
            return;
        }
        s_recording = 1;
        s_rec_frames = 0;
        fs_copystr(s_status, "recording...", sizeof s_status);
    } else {
        if (s_rec_fd >= 0) { fsync(s_rec_fd); close(s_rec_fd); s_rec_fd = -1; }
        s_recording = 0;
        fs_copystr(s_status, "recording saved", sizeof s_status);
        notify("Camera: recording saved");
    }
}

static void record_frame(void) {
    unsigned char jpg[SENSOR_W * SENSOR_H * 2];
    int len = jpeg_encode(s_px, SENSOR_W, SENSOR_H, SENSOR_W, 80, jpg,
                          sizeof jpg);
    if (len <= 0) return;
    unsigned char hdr[4];
    hdr[0] = (unsigned char)((len >> 24) & 255);
    hdr[1] = (unsigned char)((len >> 16) & 255);
    hdr[2] = (unsigned char)((len >> 8) & 255);
    hdr[3] = (unsigned char)(len & 255);
    write(s_rec_fd, hdr, 4);
    write(s_rec_fd, jpg, (size_t)len);
    s_rec_frames++;
    if (s_rec_frames >= 150) {          /* auto-stop at ~15s @ 10fps */
        fsync(s_rec_fd); close(s_rec_fd); s_rec_fd = -1;
        s_recording = 0;
        fs_copystr(s_status, "recording saved", sizeof s_status);
        notify("Camera: recording saved");
    }
}

static int camera_main(void) {
    s_wid = (int)wm_create(640, 480, &s_info);
    if (s_wid < 0) return 1;
    s_surf.px = (u32 *)(unsigned long)s_info.app_va;
    s_surf.w = (int)s_info.w; s_surf.h = (int)s_info.h; s_surf.pitch = (int)s_info.w;
    wm_title(s_wid, "Camera");
    fs_copystr(s_status, "ready", sizeof s_status);

    long t0 = time_ms();
    long last = t0;
    for (;;) {
        int ev;
        while ((ev = poll_key()) != 0) {
            int ascii = ev & 255, make = !(ev & (1 << 16));
            if (!make) continue;
            if (ascii == 27) {               /* Esc */
                if (s_recording) do_record_toggle();
                return 0;
            }
            if (ascii == ' ') { do_photo(); continue; }
            if (ascii == 'r' || ascii == 'R') { do_record_toggle(); continue; }
        }

        long now = time_ms();
        s_frame = (now - t0) / 16;           /* ~60fps animation tick */

        sensor_frame(s_frame);
        if (s_recording && now - last >= 100) { record_frame(); last = now; }

        blit_viewfinder();
        wm_flip(s_wid);
        sleep(16);
    }
}

int main_entry(int argc, char **argv, char **envp) {
    (void)argc; (void)argv; (void)envp;
    return camera_main();
}
