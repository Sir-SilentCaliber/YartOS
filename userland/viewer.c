/* /bin/viewer — YartOS media / image viewer (roadmap #5).
 *
 * Opens a .jpg (still) or .mjpeg (video) and shows it, decoded with
 * userland/jpeg.c.  Launch with a path argument:
 *     /bin/viewer /home/yart/Pictures/cam_0.jpg
 * or no argument: picks the newest capture in ~/Pictures.
 *
 * Controls:  Space = play/pause (MJPEG)   Left/Right = prev/next frame
 *            Esc = close
 */
#include "sys.h"
#include "gfx.h"
#include "jpeg.h"
#include "fsutil.h"

#define MAXF (16 * 1024 * 1024 / 4)   /* 16 MiB clip/frame budget (u32 words) */

static int          s_wid;
static wm_surf_info_t s_info;
static surface_t    s_surf;

static char         s_path[160];
static int          s_is_video;

/* still image: decoded ARGB + dims */
static u32        *s_img;      /* u32 pixels */
static int          s_iw, s_ih;

/* video: frame offsets in a loaded file */
static char        *s_vid;      /* raw file bytes */
static long         s_vid_len;
static long         s_off[2048];   /* start of each frame payload */
static long         s_fsz[2048];
static int          s_fcount;
static int          s_fcur;
static int          s_playing;

static void load_still(const char *path) {
    char *raw = (char *)mmap(MAXF * 4);
    if (!raw) return;
    long n = fs_read_file(path, raw, MAXF * 4);
    if (n <= 0) { munmap((long)raw); return; }
    int w = 0, h = 0;
    if (jpeg_decode((unsigned char *)raw, (unsigned)n, NULL, 0, 0, 0, &w, &h) != 0) {
        munmap((long)raw); return;
    }
    u32 *px = (u32 *)mmap(((long)w * h * 4 + 4095) & ~4095L);
    if (!px) { munmap((long)raw); return; }
    if (jpeg_decode((unsigned char *)raw, (unsigned)n, px, w, 0, 0, &w, &h) != 0) {
        munmap((long)px); munmap((long)raw); return;
    }
    munmap((long)raw);
    s_img = px; s_iw = w; s_ih = h;
}

static void load_video(const char *path) {
    char *raw = (char *)mmap(MAXF * 4);
    if (!raw) return;
    long n = fs_read_file(path, raw, MAXF * 4);
    if (n <= 0) { munmap((long)raw); return; }
    s_vid = raw; s_vid_len = n;
    long pos = 0;
    s_fcount = 0;
    while (pos + 4 <= n && s_fcount < 2048) {
        unsigned int fsz = ((unsigned int)(unsigned char)raw[pos] << 24) |
                           ((unsigned int)(unsigned char)raw[pos + 1] << 16) |
                           ((unsigned int)(unsigned char)raw[pos + 2] << 8) |
                            (unsigned int)(unsigned char)raw[pos + 3];
        pos += 4;
        if (fsz == 0 || pos + (long)fsz > n) break;
        s_off[s_fcount] = pos;
        s_fsz[s_fcount] = (long)fsz;
        s_fcount++;
        pos += fsz;
    }
    if (s_fcount == 0) { s_vid = NULL; munmap((long)raw); return; }
    s_fcur = 0;
}

/* decode frame index f into a freshly mapped buffer (caller reuses s_img) */
static void decode_frame(int f) {
    if (f < 0 || f >= s_fcount) return;
    const unsigned char *p = (const unsigned char *)(s_vid + s_off[f]);
    int w = 0, h = 0;
    if (jpeg_decode(p, (unsigned)s_fsz[f], NULL, 0, 0, 0, &w, &h) != 0) return;
    u32 *px = (u32 *)mmap(((long)w * h * 4 + 4095) & ~4095L);
    if (!px) return;
    if (jpeg_decode(p, (unsigned)s_fsz[f], px, w, 0, 0, &w, &h) == 0) {
        if (s_img) munmap((long)s_img);
        s_img = px; s_iw = w; s_ih = h;
    } else {
        munmap((long)px);
    }
}

static void draw_frame(void) {
    sf_fill(&s_surf, RGB(0x10, 0x10, 0x14));
    if (!s_img) {
        sf_text(&s_surf, 20, 20, "no image loaded", RGB(0xC0, 0xC4, 0xCC));
        return;
    }
    surface_t src = { s_img, s_iw, s_ih, s_iw };
    /* fit, preserving aspect */
    int dw = s_surf.w, dh = (s_surf.w * s_ih) / s_iw;
    if (dh > s_surf.h - 36) { dh = s_surf.h - 36; dw = (dh * s_iw) / s_ih; }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    int dx = (s_surf.w - dw) / 2, dy = (s_surf.h - 36 - dh) / 2;
    if (dy < 2) dy = 2;
    sf_blit_scaled(&s_surf, dx, dy, dw, dh, &src, 0, 0, s_iw, s_ih);

    char status[128];
    if (s_is_video) {
        fs_copystr(status, s_playing ? "playing" : "paused", sizeof status);
        char num[16]; fs_itoa(s_fcur + 1, num);
        int k = (int)strlen(status);
        status[k++] = ' '; status[k++] = '[';
        for (int i = 0; num[i]; i++) status[k++] = num[i];
        status[k++] = '/';
        char tot[16]; fs_itoa(s_fcount, tot);
        for (int i = 0; tot[i]; i++) status[k++] = tot[i];
        status[k++] = ']'; status[k] = 0;
    } else {
        fs_copystr(status, s_path, sizeof status);
    }
    int by = s_surf.h - 30;
    sf_fill_rect(&s_surf, 0, by, s_surf.w, 30, RGB(0x1A, 0x1A, 0x20));
    sf_hline(&s_surf, 0, by, s_surf.w, RGB(0x34, 0x38, 0x42));
    sf_text(&s_surf, 10, by + 8, status, RGB(0xC0, 0xC4, 0xCC));
    sf_text(&s_surf, s_surf.w - sf_text_width("Space play  <-  -> step  Esc") - 10,
            by + 8, "Space play  <-  -> step  Esc", RGB(0x80, 0x86, 0x92));
}

/* pick newest capture in ~/Pictures (or ~/Videos) if no arg */
static void default_path(void) {
    const char *dirs[2] = { "/home/yart/Pictures", "/home/yart/Videos" };
    const char *exts[2] = { ".jpg", ".mjpeg" };
    int best = -1;
    char bestpath[160] = {0};
    for (int d = 0; d < 2; d++) {
        int fd = open(dirs[d], O_RDONLY);
        if (fd < 0) continue;
        yart_dirent_t de[32];
        long n;
        while ((n = getdents(fd, de, 32)) > 0) {
            for (long i = 0; i < n; i++) {
                if (de[i].type != 1) continue;
                if (!fs_endswith(de[i].name, exts[d])) continue;
                int num = 0;
                const char *p = de[i].name;
                if (d == 0) p += 4; else p += 4;   /* "cam_" */
                while (*p >= '0' && *p <= '9') { num = num * 10 + (*p - '0'); p++; }
                if (num > best) {
                    best = num;
                    int k = 0;
                    for (const char *q = dirs[d]; *q; q++) bestpath[k++] = *q;
                    bestpath[k++] = '/';
                    for (const char *q = de[i].name; *q; q++) bestpath[k++] = *q;
                    bestpath[k] = 0;
                }
            }
        }
        close(fd);
    }
    if (best >= 0) fs_copystr(s_path, bestpath, sizeof s_path);
}

static int viewer_main(int argc, char **argv) {
    s_wid = (int)wm_create(640, 480, &s_info);
    if (s_wid < 0) return 1;
    s_surf.px = (u32 *)(unsigned long)s_info.app_va;
    s_surf.w = (int)s_info.w; s_surf.h = (int)s_info.h; s_surf.pitch = (int)s_info.w;
    wm_title(s_wid, "Viewer");

    if (argc > 1) fs_copystr(s_path, argv[1], sizeof s_path);
    else default_path();
    if (s_path[0]) {
        s_is_video = fs_endswith(s_path, ".mjpeg");
        if (s_is_video) load_video(s_path); else load_still(s_path);
        if (s_is_video && s_vid) { decode_frame(0); s_playing = 1; }
    }

    long last = time_ms();
    for (;;) {
        int ev;
        while ((ev = poll_key()) != 0) {
            int ascii = ev & 255, make = !(ev & (1 << 16));
            if (!make) continue;
            if (ascii == 27) return 0;
            if (ascii == ' ') { if (s_is_video) s_playing = !s_playing; }
            if (s_is_video) {
                if (ascii == 0xE3) { s_fcur--; if (s_fcur < 0) s_fcur = 0; decode_frame(s_fcur); s_playing = 0; }   /* YK_LEFT  */
                if (ascii == 0xE4) { s_fcur++; if (s_fcur >= s_fcount) s_fcur = s_fcount - 1; decode_frame(s_fcur); s_playing = 0; } /* YK_RIGHT */
            }
        }
        long now = time_ms();
        if (s_is_video && s_playing && now - last >= 66) {
            last = now;
            s_fcur++;
            if (s_fcur >= s_fcount) s_fcur = 0;
            decode_frame(s_fcur);
        }
        draw_frame();
        wm_flip(s_wid);
        sleep(16);
    }
}

int main_entry(int argc, char **argv, char **envp) {
    (void)envp;
    return viewer_main(argc, argv);
}
