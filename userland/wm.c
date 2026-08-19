/* YartOS ring-3 compositor — orchestrator module.
 * Screen concerns are split across wm_damage.c, wm_windows.c, wm_dock.c,
 * wm_panel.c, wm_launcher.c, wm_overlays.c (see wm.h).  This file owns the
 * main loop, input routing, cursor, backdrop compositing, config loading,
 * process management and app launching. */
#include "wm.h"
#include "jpeg_enc.h"

/* ---- screenshot + screen recording (roadmap #6) ----
 * The compositor owns the framebuffer (G_fb), so it captures a rectangle by
 * copying it into a contiguous buffer, JPEG-encoding it (jpeg_enc.c), and
 * writing to /home/yart/Screenshots/.  Screen recording appends the same
 * encoded frames to an MJPEG file. */
static int shot_counter = 0;

static void capture_rect(int x, int y, int w, int h, const char *path) {
    if (w <= 0 || h <= 0) return;
    /* the encoder needs dims that are multiples of 16 (4:2:0) */
    if ((w & 15) || (h & 15)) return;
    /* clamp to the framebuffer */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > G_fb.w) w = G_fb.w - x;
    if (y + h > G_fb.h) h = G_fb.h - y;
    if (w <= 0 || h <= 0) return;

    u32 *buf = (u32 *)mmap((long)w * h * 4 + 4096);
    if (!buf) return;
    for (int row = 0; row < h; row++)
        memcpy(buf + (long)row * w, G_fb.px + (long)(y + row) * G_fb.pitch + x, (size_t)w * 4);

    unsigned char *jpg = (unsigned char *)mmap((long)w * h * 2 + 4096);
    int len = jpeg_encode(buf, w, h, w, 85, jpg, (unsigned int)w * h * 2);

    if (len > 0) {
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
        if (fd >= 0) { write(fd, jpg, (size_t)len); fsync(fd); close(fd); }
    }
    munmap((long)buf);
    munmap((long)jpg);
}

static void screenshot_full(void) {
    mkdir("/home/yart/Screenshots");
    char path[96];
    /* Screenshots/shot_N.jpg */
    int k = 0; const char *p = "/home/yart/Screenshots/shot_"; while (*p) path[k++] = *p++;
    char num[16]; int j = 0, v = shot_counter++; if (!v) num[j++] = '0';
    while (v) { num[j++] = (char)('0' + v % 10); v /= 10; }
    while (j) path[k++] = num[--j];
    const char *e = ".jpg"; while (*e) path[k++] = *e++;
    path[k] = 0;
    capture_rect(0, 0, G_fb.w, G_fb.h, path);
    osd("Screenshot saved");
}

static void screenshot_window(void) {
    if (G_focus_win < 0 || G_focus_win >= MAX_WIN) return;
    win_t *w = &G_win[G_focus_win];
    if (!w->active) return;
    int cx, cy, cw, ch; win_client_rect(w, &cx, &cy, &cw, &ch);
    mkdir("/home/yart/Screenshots");
    char path[96];
    int k = 0; const char *p = "/home/yart/Screenshots/win_"; while (*p) path[k++] = *p++;
    char num[16]; int j = 0, v = shot_counter++; if (!v) num[j++] = '0';
    while (v) { num[j++] = (char)('0' + v % 10); v /= 10; }
    while (j) path[k++] = num[--j];
    const char *e = ".jpg"; while (*e) path[k++] = *e++;
    path[k] = 0;
    capture_rect(cx, cy, cw, ch, path);
    osd("Window captured");
}

/* region selection: drag a rectangle, then capture it on release */
static bool G_shot_region = false;
static int G_shot_x0, G_shot_y0;

/* ---- screen recording (MJPEG) ---- */
static bool G_recording = false;
static int  G_rec_fd = -1;
static int  G_rec_frame = 0;
static long G_rec_last = 0;

static void record_frame(void) {
    /* capture a downscaled (2x) frame to keep the file size + TCG cost sane */
    int w = G_fb.w / 2, h = G_fb.h / 2;
    u32 *buf = (u32 *)mmap((long)w * h * 4 + 4096);
    if (!buf) return;
    for (int row = 0; row < h; row++) {
        u32 *src = G_fb.px + (long)(row * 2) * G_fb.pitch;
        u32 *dst = buf + (long)row * w;
        for (int col = 0; col < w; col++)
            dst[col] = src[col * 2];   /* nearest-neighbour 2x downscale */
    }
    unsigned char *jpg = (unsigned char *)mmap((long)w * h * 2 + 4096);
    int len = jpeg_encode(buf, w, h, w, 80, jpg, (unsigned int)w * h * 2);
    if (len > 0 && G_rec_fd >= 0) {
        unsigned char sz[4] = { (unsigned char)(len >> 24), (unsigned char)(len >> 16),
                                (unsigned char)(len >> 8),  (unsigned char)len };
        write(G_rec_fd, sz, 4);
        write(G_rec_fd, jpg, (size_t)len);
    }
    munmap((long)buf);
    munmap((long)jpg);
}

static void record_toggle(void) {
    if (G_recording) {
        G_recording = false;
        if (G_rec_fd >= 0) { fsync(G_rec_fd); close(G_rec_fd); G_rec_fd = -1; }
        osd("Recording saved");
    } else {
        mkdir("/home/yart/Screenshots");
        char path[96]; int k = 0; const char *p = "/home/yart/Screenshots/rec_";
        while (*p) path[k++] = *p++;
        char num[16]; int j = 0, v = shot_counter++; if (!v) num[j++] = '0';
        while (v) { num[j++] = (char)('0' + v % 10); v /= 10; }
        while (j) path[k++] = num[--j];
        const char *e = ".mjpeg"; while (*e) path[k++] = *e++;
        path[k] = 0;
        G_rec_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
        if (G_rec_fd < 0) { osd("Recording failed"); return; }
        G_recording = true;
        G_rec_frame = 0;
        G_rec_last = time_ms();
        osd("Recording started (Super+R to stop)");
    }
}

/* ---- userland TSC clock (for precise frame pacing) ----
 * `time_ms()` is a syscall (expensive per call), so the frame loop must not
 * busy-poll it.  We calibrate the TSC rate ONCE here (rdtsc is unprivileged)
 * against two time_ms() samples 50 ms apart, then busy-wait with an inline
 * rdtsc - zero syscalls in the pacing tail.  This is the same vDSO trick a
 * real OS uses to hand the clock to userspace. */
static inline u64 wm_rdtsc(void){ u32 lo,hi; __asm__ volatile("rdtsc":"=a"(lo),"=d"(hi)); return ((u64)hi<<32)|lo; }
static u64 g_tsc_per_ms;         /* TSC counts per ms (0 = uncalibrated) */
static u64 g_tsc_base;           /* TSC value at a known time_ms()        */
static long g_ms_base;           /* the known time_ms() value             */

/* absolute TSC value for a given time_ms() deadline */
static inline u64 tsc_deadline(long ms){
    return g_tsc_base + (u64)(ms - g_ms_base) * g_tsc_per_ms;
}

/* ---- global state definitions (see wm.h for the shared contract) ---- */
surface_t G_fb, G_wp;
surface_t G_backdrop;
bool    g_backdrop_dirty = true;
int     G_cx, G_cy, G_pressed_x, G_pressed_y;
unsigned char G_mb;
long    G_start_ms;
char    G_clk[16] = "00:00:00";
unsigned long G_last_sec = (unsigned long)-1;
char    G_date[32] = "";
int     G_workspace = 0;
int     G_ws_count = 1;
app_t   G_app[MAX_APPS];
int     G_apps;
bool    G_grid, G_overview, G_quick, G_calendar, G_dockmenu, G_switcher;
bool    G_locked = true;   /* login at boot: the lock screen is the login screen (real auth, like an enterprise OS) */
bool    G_super_held = false;
/* Session-end flag: Ctrl+Alt+Backspace sets it; wm_run()'s loop checks it and
 * returns, so init can exit and the kernel can reclaim the framebuffer. */
static bool G_session_end = false;
/* Lock screen auth state (real password check via SYS_AUTH_VERIFY). */
bool    G_lock_prompt = false;
char    G_lock_pw[64];
int     G_lock_pw_len = 0;
bool    G_lock_bad = false;
/* Dock visibility (settings.conf dock=0/1). */
bool    G_dock_visible = true;
/* Active cursor theme (settings.conf cursor=...). */
static int G_cur_theme = -1;
int     G_switcher_idx;
int     G_dockmenu_x, G_dockmenu_y, G_dockmenu_w, G_dockmenu_h;
int     G_quick_x, G_quick_y, G_quick_w, G_quick_h, G_cal_x, G_cal_y, G_cal_w, G_cal_h;
int     G_cal_month = 8, G_cal_year = 2026;
char    G_search[40];
int     G_search_len;
int     G_sel_app = -1, G_sel_desk = -1;
bool    G_double;
bool    G_multi_sel, G_desktop_drag, G_title_drag, G_marquee, G_icon_drag;
int     G_drag_win = -1, G_drag_dx, G_drag_dy;
int     G_resize_win = -1, G_resize_edges, G_drag_icon = -1, G_icon_dx, G_icon_dy;
int     G_mx0, G_my0, G_mx1, G_my1;
long    G_last_click_desk;
long    G_last_title_click;
int     G_last_click_idx = -1;
bool    G_menu;
long    G_menu_t0;
int     G_menu_x, G_menu_y, G_menu_w, G_menu_h, G_menu_type, G_menu_idx, G_menu_arg;
menuitem_t G_menu_items[MAX_MENU];
int     G_menu_n;
char    G_menu_path[72];
bool    G_audio = true, G_wifi = false, G_wired = true;   /* QEMU: wired e1000, no 802.11 hw */
bool    G_clip_open = false;
char    G_clipboard[512]; int G_clipboard_len = 0;
bool    G_netlist_open = false;
char    G_netlist[1024]; int G_netlist_len = 0;
int     G_net_x0, G_net_x1, G_clip_x0, G_clip_x1, G_lang_x0, G_lang_x1;
int     G_clip_x, G_clip_y, G_clip_w, G_clip_h;
int     G_nl_x, G_nl_y, G_nl_w, G_nl_h;
int     G_vol = 70;
int     G_net_up;
int     G_scale = 1;   /* HiDPI UI scale (1 or 2), persisted in settings.conf */
long    G_tooltip_t0;
int     G_tooltip_item = -1;
long    G_osd_t0;
char    G_osd[80];
int     G_wp_index;
char    G_notifs[16][128];
int     G_notif_n = 0;
proc_t G_pids[MAX_PIDS];
int     G_pids_n;
pending_t G_pending[MAX_PIDS];
int     G_pending_n;

void osd(const char *m){ int i=0; while(m[i]&&i<(int)sizeof(G_osd)-1){G_osd[i]=m[i];i++;} G_osd[i]=0; G_osd_t0=time_ms(); }

/* Ask the compositor to end the graphical session. */
void wm_session_end(void){ G_session_end = true; }

int icon_for_path(const char *p){
    if(strcmp(p,"/bin/nyra")==0) return ICON_DOCK_TERMINAL;
    if(strcmp(p,"/bin/files")==0) return ICON_DOCK_FILES;
    if(strcmp(p,"/bin/settings")==0) return ICON_DOCK_SETTINGS;
    if(strcmp(p,"/bin/browser")==0) return ICON_DOCK_BROWSER;
    if(strcmp(p,"/bin/editor")==0) return ICON_DOCK_EDITOR;
    if(strcmp(p,"/bin/camera")==0) return ICON_DEV_CAMERA_PHOTO;
    if(strcmp(p,"/bin/viewer")==0) return ICON_MIME_IMAGE_X_GENERIC;
    return ICON_MIME_APPLICATION_X_EXECUTABLE;
}

/* map a .desktop Icon= name to an icon enum */
int icon_for_name(const char *n){
    if(strcmp(n,"calculator")==0) return ICON_DOCK_CALC;
    if(strcmp(n,"camera")==0) return ICON_DEV_CAMERA_PHOTO;
    if(strcmp(n,"viewer")==0 || strcmp(n,"image")==0) return ICON_MIME_IMAGE_X_GENERIC;
    if(strcmp(n,"terminal")==0) return ICON_DOCK_TERMINAL;
    if(strcmp(n,"files")==0) return ICON_DOCK_FILES;
    if(strcmp(n,"settings")==0) return ICON_DOCK_SETTINGS;
    if(strcmp(n,"editor")==0) return ICON_DOCK_EDITOR;
    if(strcmp(n,"browser")==0) return ICON_DOCK_BROWSER;
    if(strcmp(n,"video")==0) return ICON_DEV_CAMERA_VIDEO;
    return ICON_MIME_APPLICATION_X_EXECUTABLE;
}

/* ---------- app, dock and desktop persistence ---------- */
void add_app(const char *name,const char *path,int icon){
    if(G_apps>=MAX_APPS) return;
    for(int i=0;i<G_apps;i++) if(strcmp(G_app[i].path,path)==0) return;
    copy_str(G_app[G_apps].name,name,sizeof(G_app[0].name));
    copy_str(G_app[G_apps].path,path,sizeof(G_app[0].path));
    G_app[G_apps].icon=icon; G_app[G_apps].removable=(path[0]=='/'); G_app[G_apps].dynamic=false; G_apps++;
}
int app_index(const char *path){ for(int i=0;i<G_apps;i++) if(strcmp(G_app[i].path,path)==0) return i; return -1; }

/* add/refresh an app discovered from a the desktop-entry dir file.
 * Dynamic apps are re-derived every scan, so `apk del` (which removes the
 * .desktop file) makes the app vanish from the launcher on the next scan. */
static void add_desktop_app(const char *name,const char *path,int icon){
    for(int i=0;i<G_apps;i++){
        if(G_app[i].dynamic && strcmp(G_app[i].path,path)==0){
            copy_str(G_app[i].name,name,sizeof(G_app[0].name));
            G_app[i].icon=icon; return;
        }
    }
    if(G_apps>=MAX_APPS) return;
    for(int i=0;i<G_apps;i++) if(strcmp(G_app[i].path,path)==0) return;
    copy_str(G_app[G_apps].name,name,sizeof(G_app[0].name));
    copy_str(G_app[G_apps].path,path,sizeof(G_app[0].path));
    G_app[G_apps].icon=icon; G_app[G_apps].removable=true; G_app[G_apps].dynamic=true; G_apps++;
}

/* drop dynamic apps whose .desktop entry no longer exists */
static void prune_desktop_apps(void){
    for(int i=0;i<G_apps;){
        if(!G_app[i].dynamic){ i++; continue; }
        char dp[180]; copy_str(dp,"/usr/share/applications/",sizeof dp);
        int k=(int)strlen(dp);
        const char *base=G_app[i].path;
        for(const char *p=base;*p;p++) if(*p=='/') base=p+1;
        for(const char *p=base;*p&&k<170;p++) dp[k++]=*p;
        const char *suf=".desktop"; while(*suf&&k<179) dp[k++]=*suf++;
        dp[k]=0;
        int fd=open(dp,0);
        if(fd>=0){ close(fd); i++; continue; }
        for(int j=i;j<G_apps-1;j++) G_app[j]=G_app[j+1];
        G_apps--;
    }
}

/* Scan the desktop-entry dir and register every launchable app.
 * This is the same mechanism GNOME/KDE use: `apk add <pkg>` drops a .desktop
 * entry, the compositor notices, and the app appears in the Super launcher. */
void scan_desktop_apps(void){
    prune_desktop_apps();
    int fd=open("/usr/share/applications",0);
    if(fd<0) return;
    yart_dirent_t de[32];
    long n;
    while((n=getdents(fd,de,32))>0){
        for(long i=0;i<n;i++){
            if(de[i].type!=1) continue;
            const char *nm=de[i].name;
            int L=(int)strlen(nm);
            if(L<=8 || strcmp(nm+L-8,".desktop")!=0) continue;
            char full[180]; copy_str(full,"/usr/share/applications/",sizeof full);
            int k=(int)strlen(full);
            for(int j=0;nm[j]&&k<170;j++) full[k++]=nm[j];
            full[k]=0;
            char buf[512];
            int f2=open(full,0);
            if(f2<0) continue;
            long r=read(f2,buf,sizeof buf-1); close(f2);
            if(r<=0) continue;
            buf[r]=0;
            char name[40]={0}, exec[72]={0}, iconn[32]={0};
            for(int p=0;buf[p];){
                int e=p; while(buf[e]&&buf[e]!='\n') e++;
                char saved=buf[e]; buf[e]=0;
                if(strncmp(buf+p,"Name=",5)==0) copy_str(name,buf+p+5,sizeof name);
                else if(strncmp(buf+p,"Exec=",5)==0) copy_str(exec,buf+p+5,sizeof exec);
                else if(strncmp(buf+p,"Icon=",5)==0) copy_str(iconn,buf+p+5,sizeof iconn);
                buf[e]=saved; p=saved?e+1:e;
            }
            if(!name[0]||!exec[0]) continue;
            add_desktop_app(name,exec,icon_for_name(iconn));
        }
    }
    close(fd);
}

void save_config(void){
    int fd=open("/home/yart/desktop.conf",O_WRONLY|O_CREAT|O_TRUNC); if(fd<0) return;
    char line[160];
    for(int i=0;i<G_dock_n;i++){ if(G_dock[i].core) continue; int k=0; const char *p="dock="; while(*p)line[k++]=*p++; for(int j=0;G_dock[i].name[j]&&k<150;j++)line[k++]=G_dock[i].name[j]; line[k++]='|'; for(int j=0;G_dock[i].path[j]&&k<156;j++)line[k++]=G_dock[i].path[j]; line[k++]='\n'; write(fd,line,k); }
    for(int i=0;i<G_desk_n;i++){ int k=0; const char *p="desk="; while(*p)line[k++]=*p++; for(int j=0;G_desk[i].name[j]&&k<120;j++)line[k++]=G_desk[i].name[j]; line[k++]='|'; for(int j=0;G_desk[i].path[j]&&k<120;j++)line[k++]=G_desk[i].path[j]; line[k++]='|'; char num[8]; int nx=G_desk[i].gx; int d=10000; while(d){ num[0]='0'+(nx/d); if(k<150){line[k++]=num[0];} nx%=d; d/=10; } line[k++]='|'; int ny=G_desk[i].gy; d=10000; while(d){ num[0]='0'+(ny/d); if(k<150){line[k++]=num[0];} ny%=d; d/=10; } line[k++]='\n'; write(fd,line,k); }
    fsync(fd); close(fd);
}
static void load_apps_config(void){
    add_app("Console","/bin/nyra",ICON_DOCK_TERMINAL);
    add_app("Files","/bin/files",ICON_DOCK_FILES);
    add_app("Settings","/bin/settings",ICON_DOCK_SETTINGS);
    add_app("Text","/bin/editor",ICON_DOCK_EDITOR);
    add_app("Video","/bin/media",ICON_ACT_MEDIA_PLAYBACK_START);
    add_app("Camera","/bin/camera",ICON_DEV_CAMERA_PHOTO);
    add_app("Viewer","/bin/viewer",ICON_MIME_IMAGE_X_GENERIC);
}
static void load_desktop_config(void){
    G_desk_n=0;
    add_desktop("Home","/home/yart",ICON_PLACE_HOME,1);
    add_desktop("Documents","/home/yart/Documents",ICON_PLACE_DOCS,2);
    add_desktop("Downloads","/home/yart/Downloads",ICON_PLACE_DL,3);
    add_desktop("Trash","trash",ICON_DOCK_TRASH,4);
    int fd=open("/home/yart/desktop.conf",0);
    if(fd<0) return;
    char buf[2048]; long n=read(fd,buf,sizeof(buf)-1); close(fd); if(n<=0) return; buf[n]=0;
    char *p=buf;
    while(*p){
        char *e=p; while(*e&&*e!='\n') e++; char old=*e; *e=0;
        if(strncmp(p,"dock=",5)==0){ char *q=p+5; char *bar=0; for(char*r=q;*r;r++) if(*r=='|'){bar=r;break;} if(bar){*bar=0; add_dock_app(bar+1,q,icon_for_path(bar+1),true);} }
        else if(strncmp(p,"desk=",5)==0){ char *q=p+5; char *b1=0,*b2=0,*b3=0; for(char*r=q;*r;r++){ if(*r=='|'&&!b1)b1=r; else if(*r=='|'&&b1&&!b2)b2=r; else if(*r=='|'&&b2){b3=r;break;} } if(b1){*b1=0; char *path=b1+1; int px=-1,py=-1; if(b2){*b2=0; if(b3){*b3=0; px=0; for(char*d=b2+1;*d;d++)px=px*10+(*d-'0'); py=0; for(char*d=b3+1;*d;d++)py=py*10+(*d-'0'); } add_desktop_xy(q,path,icon_for_path(path),100,px,py); } else add_desktop(q,path,icon_for_path(path),100); } }
        p=old?e+1:e;
    }
}
void load_all(void){
    load_apps_config(); scan_desktop_apps(); default_dock(); dock_apply_hidden();
    int fd=open("/home/yart/dock.conf",0);
    if(fd>=0){ char buf[1024]; long n=read(fd,buf,sizeof(buf)-1); close(fd); if(n>0){ buf[n]=0; char *p=buf; while(*p){ char *e=p; while(*e&&*e!='\n')e++; char old=*e; *e=0; if(strncmp(p,"pin=",4)==0) add_dock_app(p+4,p+4,icon_for_path(p+4),true); p=old?e+1:e; } } }
    load_desktop_config();
}
void save_dock_only(void){
    int fd=open("/home/yart/dock.conf",O_WRONLY|O_CREAT|O_TRUNC); if(fd<0)return; char line[128];
    for(int i=0;i<G_dock_n;i++){ if(G_dock[i].core||G_dock[i].path[0]!='/') continue; int k=0; const char*q="pin="; while(*q)line[k++]=*q++; for(int j=0;G_dock[i].path[j]&&k<120;j++)line[k++]=G_dock[i].path[j]; line[k++]='\n'; write(fd,line,k); }
    close(fd);
}

/* ---------- settings.conf (REAL, persistent settings) ----------
 * The Settings app writes /home/yart/settings.conf; the compositor reads it
 * at boot and re-reads it every couple of seconds, applying accent / cursor
 * theme / wallpaper / dock visibility / volume.  No visual redesign — the
 * values only feed the existing theme/cursor/wallpaper/dock systems. */
extern theme_t g_theme;
static int  G_cursor_last_idx = -1;
static long G_last_settings_check = 0;

static int atoi_small(const char *s){ int v=0; while(*s>='0'&&*s<='9'){ v=v*10+(*s-'0'); s++; } return v; }

static void settings_apply(const char *key, const char *val){
    if(strcmp(key,"accent")==0){
        u32 c = theme_parse_color(val);
        if(c && g_theme.c[T_ACCENT] != c){
            g_theme.c[T_ACCENT] = c;
            g_theme.c[T_ACCENT_DIM] = (c & 0xFF000000u) | ((((c>>16)&0xFF)*7/10)<<16) |
                                      ((((c>>8)&0xFF)*7/10)<<8) | ((c&0xFF)*7/10);
            g_theme.c[T_BTN_TOGGLE_ON] = (c & 0xFFFFFFu) | 0xDC000000u;
            g_backdrop_dirty=true; damage_whole();
        }
    } else if(strcmp(key,"cursor")==0){
        int t = cursors_theme_by_name(val);
        if(t >= 0 && t != G_cursor_last_idx){
            G_cur_theme = t;
            G_cursor_last_idx = t;
        }
    } else if(strcmp(key,"wallpaper")==0){
        int idx = atoi_small(val);
        if(idx >= 0 && idx < wallpaper_count() && idx != G_wp_index){
            G_wp_index = idx;
            wallpaper_load_index(idx);
            wallpaper_bind(&G_wp);
            g_backdrop_dirty=true; damage_whole();
        }
    } else if(strcmp(key,"dock")==0){
        bool vis = val[0] != '0';
        if(vis != G_dock_visible){
            G_dock_visible = vis;
            g_backdrop_dirty=true; damage_whole();
        }
    } else if(strcmp(key,"volume")==0){
        int v = atoi_small(val);
        if(v < 0) v = 0;
        if(v > 100) v = 100;
        if(v != G_vol){ G_vol = v; G_audio = v > 0; audio_vol(v); }
    } else if(strcmp(key,"scale")==0){
        int sc = atoi_small(val);
        if(sc < 1) sc = 1;
        if(sc > 2) sc = 2;
        if(sc != G_scale){
            G_scale = sc;
            sf_set_scale(sc);            /* HiDPI text scale for the compositor */
            rebuild_dock_cache();        /* dock icons re-render at the new size */
            g_backdrop_dirty = true;
            damage_whole();
        }
    }
}

static void settings_load(void){
    int fd=open("/home/yart/settings.conf",0);
    if(fd<0) return;
    static char buf[512];
    long n=read(fd,buf,sizeof(buf)-1);
    close(fd);
    if(n<=0) return;
    buf[n]=0;
    char *p=buf;
    while(*p){
        char *e=p; while(*e&&*e!='\n') e++;
        char old=*e; *e=0;
        char *eq=0; for(char*q=p;*q;q++) if(*q=='='){eq=q;break;}
        if(eq){ *eq=0; settings_apply(p, eq+1); }
        p = old?e+1:e;
    }
}

static void settings_poll(long now){
    /* re-read cheaply every ~2s so the Settings app's writes take effect */
    if(now - G_last_settings_check < 2000) return;
    G_last_settings_check = now;
    settings_load();
}

/* ---------- processes ---------- */
void pid_forget_dead(void){
    for(int i=0;i<G_pids_n;){
        int st=0; long r=waitpid_nohang(G_pids[i].pid,&st);
        if(r==G_pids[i].pid||r==-1){ for(int j=i;j<G_pids_n-1;j++)G_pids[j]=G_pids[j+1]; G_pids_n--; } else i++;
    }
}
void pid_record(long pid,const char*path){ if(pid<=0)return; for(int i=0;i<G_pids_n;i++) if(G_pids[i].pid==pid)return; if(G_pids_n<MAX_PIDS){G_pids[G_pids_n].pid=pid; copy_str(G_pids[G_pids_n].path,path,sizeof(G_pids[0].path)); G_pids_n++;} if(G_pending_n<MAX_PIDS){G_pending[G_pending_n].pid=pid; copy_str(G_pending[G_pending_n].path,path,sizeof(G_pending[0].path)); G_pending[G_pending_n].stamp=time_ms(); G_pending_n++;} }
bool pid_for_path(const char*path){ for(int i=0;i<G_pids_n;i++) if(strcmp(G_pids[i].path,path)==0) return true; return false; }

void launch_app(const char *path){
    if(!path||!path[0]) return;
    if(strcmp(path,"trash")==0){
        /* REAL trash: open the Files app pointed at the trash directory. */
        const char *real="/bin/files";
        long pid=fork();
        if(pid==0){
            char *argv[]={(char*)real,(char*)"/home/yart/.trash",0};
            char *envp[]={"HOME=/home/yart","TERM=nyra",0};
            long r=exec(real,argv,envp); (void)r;
            klog("wm: exec failed\n"); exit(1);
        } else if(pid>0){ pid_record(pid,real); }
        return;
    }
    if(path[0]!='/'){ osd("App not available"); return; }
    /* Like GNOME: launching an app always starts a new process/window. */
    long pid=fork();
    if(pid==0){
        char *argv[]={(char*)path,0};
        char *envp[]={"HOME=/home/yart","TERM=nyra",0};
        long r=exec(path,argv,envp); (void)r;
        klog("wm: exec failed\n"); exit(1);
    } else if(pid>0){ pid_record(pid,path); int i=dock_find_path(path); if(i>=0)G_slot_bounce[i]=20; /* focus will be set when window appears in scan_windows */ }
}

/* ---------- input / overlays helpers ---------- */
static void close_all_overlays(void){ G_grid=G_overview=G_quick=G_calendar=G_dockmenu=false; menu_close(); }
static void handle_menu_action(int hit){ if(hit>=0) menu_dispatch(hit); else if(hit==-1) menu_close(); }
static void launch_dock(int i){
    if(i==0){ G_dockmenu=!G_dockmenu; G_grid=G_overview=G_quick=G_calendar=false; G_menu=false;
              G_dockmenu_w=260; G_dockmenu_h=420; G_dockmenu_x=G_dock_x-30; G_dockmenu_y=G_dock_y-G_dockmenu_h-8;
              if(G_dockmenu_y<PANEL_H+4)G_dockmenu_y=PANEL_H+4;
              G_search_len=0; G_search[0]=0; return; }
    if(!strcmp(G_dock[i].path,"trash")){ launch_app("trash"); return; }
    if(G_dock[i].path[0]=='/'){
        for(int j=0;j<MAX_WIN;j++){ win_t*w=&G_win[j];
            if(w->active && (w->workspace==G_workspace||w->workspace<0) && !strcmp(w->path,G_dock[i].path)){
                if(w->hidden){            /* bring a show-desktop'd window back */
                    w->hidden=false; w->dirty=true;
                    g_backdrop_dirty=true; damage_whole();
                }
                if(w->minimized){ restore_win(w); return; }
                /* click dock icon of focused app -> launch a NEW window (GNOME) */
                if(G_focus_win==j){ launch_app(G_dock[i].path); return; }
                bring_front(w); wm_focus(w->owner); G_focus_win=j; return;
            }
        }
        launch_app(G_dock[i].path);
    }
}
static int desktop_area(int y){ return y>=PANEL_H && y < (G_dock_visible ? G_dock_y-4 : G_fb.h-4); }
/* Show Desktop is a TOGGLE: first press hides every window (a WM-side
 * compositing flag — the app keeps rendering, we just stop painting it),
 * pressing again brings them all back.  The old code called
 * wm_move(id,-10000,-10000), but the kernel clamps positions to x>=-100,
 * so windows just piled up in the top-left corner and could never be
 * restored — a real bug. */
void show_desktop_toggle(void){
    close_all_overlays();
    bool any_hidden=false;
    for(int i=0;i<MAX_WIN;i++) if(G_win[i].active && G_win[i].hidden) any_hidden=true;
    if(any_hidden){
        for(int i=0;i<MAX_WIN;i++) if(G_win[i].active) G_win[i].hidden=false;
        osd("Windows restored");
    } else {
        for(int i=0;i<MAX_WIN;i++) if(G_win[i].active) G_win[i].hidden=true;
        osd("Show Desktop");
    }
    g_backdrop_dirty=true; damage_whole();
}
/* Extract the idx-th AP SSID from the wifi status text ("  SSID [...]"). */
static bool netlist_ssid_at(int idx, char *out){
    if(idx<0) return false;
    int i=0, n=0;
    while(i<G_netlist_len){
        while(i<G_netlist_len && (G_netlist[i]=='\r'||G_netlist[i]=='\n')) i++;
        if(i+1<G_netlist_len && G_netlist[i]==' ' && G_netlist[i+1]==' '){
            int ss=i+2, se=ss;
            while(se<G_netlist_len && G_netlist[se]!=' ' && G_netlist[se]!='[' && G_netlist[se]!='\n') se++;
            if(n==idx){
                int k=0; for(int q=ss;q<se&&k<39;q++) out[k++]=G_netlist[q]; out[k]=0;
                return k>0;
            }
            n++; i=se;
        }
        while(i<G_netlist_len && G_netlist[i]!='\n') i++;
        if(i<G_netlist_len) i++;
    }
    return false;
}

static void handle_press(int x,int y,int button){
    if(G_locked) return;      /* ignore clicks while locked */
    G_pressed_x=x; G_pressed_y=y;
    if(G_shot_region && button==1){
        /* region-select mode: start the selection rectangle */
        G_shot_x0 = G_mx0 = G_mx1 = x;
        G_shot_y0 = G_my0 = G_my1 = y;
        G_marquee = true;
        return;
    }
    if(button==3){
        if(G_menu){menu_close();return;}
        if(G_grid){ int ai; if(app_grid_hit(x,y,&ai)){ if(ai>=0)menu_open(x,y,2,ai); return; } }
        /* right-click a window titlebar -> Skift window menu (Restore /
         * Maximize / Minimize / Snap Left / Snap Right / Close).  A
         * right-click in the window CONTENT area (not the titlebar) must NOT
         * fall through to the desktop menu - it previously did, so
         * right-clicking inside the Console showed "Personalize/Settings"
         * (the desktop menu).  Content-area right-clicks are now a no-op
         * (the app owns its own context menu if any). */
        win_t *rw=win_at(x,y);
        if(rw){ int rty=rw->y-TB_H; if(rty<PANEL_H)rty=PANEL_H;
            if(ptin(x,y,rw->x,rty,rw->x+rw->w,rty+TB_H)){ menu_open_win(x,y,rw); return; }
            return;   /* inside window content: swallow, no desktop menu */
        }
        int i=dock_hit(x,y); if(i>=0){menu_open(x,y,1,i);return;}
        int di=desk_hit(x,y); if(desktop_area(y)&&di>=0){G_sel_desk=di; menu_open(x,y,3,di);return;}
        if(desktop_area(y)){G_sel_desk=-1; menu_open(x,y,3,-1);return;}
        if(y<PANEL_H) menu_open(x,y,4,0);
        return;
    }
    if(button!=1)return;
    int mh=menu_hit(x,y); if(G_menu){ handle_menu_action(mh); return; }
    if(G_quick){ if(!ptin(x,y,G_quick_x,G_quick_y,G_quick_x+G_quick_w,G_quick_y+G_quick_h))G_quick=false; else {
        int bx=G_quick_x+16,by=G_quick_y+16;
        if(ptin(x,y,bx,by,bx+140,by+44)){ /* Wi-Fi tile: on -> real scan + pick a network from the list */
            if(G_wifi){ wifi_disconnect(); G_wifi=false; }
            else { G_quick=false; G_netlist_open=true; wifi_scan(); G_netlist_len=(int)wifi_status(G_netlist,sizeof G_netlist); if(G_netlist_len<0)G_netlist_len=0; }
        }
        else if(ptin(x,y,bx+152,by,bx+292,by+44))G_wired=!G_wired;
        else { /* volume slider drag/click */
            int slx=bx+26,sly=by+64+22,slw=G_quick_w-58;
            if(ptin(x,y,slx-4,sly-6,slx+slw+4,sly+10)){
                int v=(x-slx)*100/slw; if(v<0)v=0; if(v>100)v=100; G_vol=v; G_audio=(v>0); audio_vol(v);
            }
        }
        return; } }
    if(G_calendar){ if(!ptin(x,y,G_cal_x,G_cal_y,G_cal_x+G_cal_w,G_cal_y+G_cal_h))G_calendar=false; else { if(ptin(x,y,G_cal_x+G_cal_w-48,G_cal_y+12,G_cal_x+G_cal_w-34,G_cal_y+30))G_cal_month++; else if(ptin(x,y,G_cal_x+G_cal_w-26,G_cal_y+12,G_cal_x+G_cal_w-10,G_cal_y+30))G_cal_month--; if(G_cal_month>12){G_cal_month=1;G_cal_year++;} if(G_cal_month<1){G_cal_month=12;G_cal_year--;} return; } }
    if(G_clip_open){ if(!ptin(x,y,G_clip_x,G_clip_y,G_clip_x+G_clip_w,G_clip_y+G_clip_h))G_clip_open=false; return; }
    if(G_netlist_open){
        if(!ptin(x,y,G_nl_x,G_nl_y,G_nl_x+G_nl_w,G_nl_y+G_nl_h)){ G_netlist_open=false; return; }
        /* click a network row -> try to connect (WPA2 needs a password) */
        int row=(y-(G_nl_y+44))/24;
        char ssid[40]; if(netlist_ssid_at(row,ssid)){
            if(wifi_connect(ssid,"")==0){ G_wifi=true; G_netlist_open=false; osd("Connected to network"); }
            else { char b[80]; int k=0; const char*q="WPA2: use Console - wifi connect "; while(*q&&k<79)b[k++]=*q++; for(int i=0;ssid[i]&&k<79;i++)b[k++]=ssid[i]; b[k]=0; osd(b); }
        }
        return;
    }
    if(G_dockmenu){ int ai; if(dockmenu_hit(x,y,&ai)){ if(ai>=0){launch_app(G_app[ai].path);G_dockmenu=false;} return; } G_dockmenu=false; return; }
    if(G_grid){ int ai; if(app_grid_hit(x,y,&ai)){ if(ai>=0)G_sel_app=ai; else if(ai==-2){G_grid=false;} return; } G_grid=false; }
    if(G_overview){ int wi=overview_card_at(x,y); if(wi>=0){bring_front(&G_win[wi]); wm_focus(G_win[wi].owner); G_focus_win=wi; G_overview=false; return;} G_overview=false; return; }
    if(y<PANEL_H){
        if(ptin(x,y,G_act_x0,0,G_act_x1,PANEL_H)){ G_grid=!G_grid; if(G_grid){G_search_len=0; G_search[0]=0;} G_overview=false; return; }
        if(ptin(x,y,G_ws_x0,0,G_ws_x1,PANEL_H)){ int ws=(x-G_ws_x0)/18; if(ws>=0&&ws<G_ws_count&&ws!=G_workspace){G_workspace=ws;char b[32];int k=0;const char*q="Workspace ";while(q[k]){b[k]=q[k];k++;} b[k++]='0'+1+ws; b[k]=0; osd(b); g_backdrop_dirty=true; damage_whole(); wm_focus(0);} return; }
        if(ptin(x,y,G_clock_x0,0,G_clock_x1,PANEL_H)){ G_calendar=!G_calendar; G_quick=false; return; }
        /* input-language button (English layout - honest: no other layout exists yet) */
        if(ptin(x,y,G_lang_x0,0,G_lang_x1,PANEL_H)){ osd("Input: English (US) — only layout available"); G_quick=false; G_calendar=false; return; }
        /* clipboard button */
        if(ptin(x,y,G_clip_x0,0,G_clip_x1,PANEL_H)){ G_clip_open=!G_clip_open; G_netlist_open=false; if(G_clip_open){ G_clipboard_len=(int)clipboard_get(G_clipboard,sizeof G_clipboard); if(G_clipboard_len<0)G_clipboard_len=0; } G_quick=false; G_calendar=false; return; }
        /* wifi ">" chevron -> real scan + network list */
        if(!(G_wifi && G_net_up) && ptin(x,y,G_net_x0,0,G_net_x1,PANEL_H)){ G_netlist_open=!G_netlist_open; G_clip_open=false; if(G_netlist_open){ wifi_scan(); G_netlist_len=(int)wifi_status(G_netlist,sizeof G_netlist); if(G_netlist_len<0)G_netlist_len=0; } G_quick=false; G_calendar=false; return; }
        if(ptin(x,y,G_sys_x0,0,G_sys_x1,PANEL_H)){ G_quick=!G_quick; G_calendar=false; return; }
        int th=tray_hit(x,y); if(th>=0){ G_quick=!G_quick; G_calendar=false; return; }
        return;
    }
    win_t *w=win_at(x,y);
    if(w){
        int cx0, cy0, cw0, ch0; win_client_rect(w, &cx0, &cy0, &cw0, &ch0);
        int ty=cy0-TB_H; if(ty<PANEL_H)ty=PANEL_H;
        bring_front(w); wm_focus(w->owner); G_focus_win=(int)(w-G_win);
        if(ptin(x,y,cx0,ty,cx0+cw0,ty+TB_H)){
            /* [min] [max] [close] buttons on the right (Skift style) */
            int bx=cx0+cw0-TB_H+4, by=ty+4, bs=TB_H-8;
            if(ptin(x,y,bx,by,bx+bs,by+bs)){ close_win(w); return; }         /* close */
            bx-=bs+2;
            if(ptin(x,y,bx,by,bx+bs,by+bs)){ toggle_max(w); return; }        /* maximize */
            bx-=bs+2;
            if(ptin(x,y,bx,by,bx+bs,by+bs)){ minimize_win(w); return; }      /* minimize */
            if(!w->maximized){
                long now=time_ms();
                if(now-G_last_title_click<320){ toggle_max(w); G_last_title_click=0; return; }
                G_last_title_click=now;
                /* anchor = offset of the grab point inside the titlebar, so the
                 * window tracks the cursor exactly (no jump). */
                G_title_drag=true;G_drag_win=(int)(w-G_win);
                G_drag_dx=x-cx0; G_drag_dy=y-ty;
            } return;
        }
        if(!w->maximized){
            int onl=x>=cx0-4&&x<=cx0+6, onr=x<=cx0+cw0+4&&x>=cx0+cw0-6;
            int onb=y<=ty+TB_H+ch0+4&&y>=ty+TB_H+ch0-6;
            int ont=y>=ty-4&&y<=ty+4;
            if(onl||onr||onb||ont){ G_resize_win=(int)(w-G_win); G_resize_edges=(onl?1:0)|(onr?2:0)|(ont?4:0)|(onb?8:0); return; }
        }
        return; }
    int di=desk_hit(x,y); if(di>=0){ G_sel_desk=di; G_multi_sel=false; long now=time_ms(); G_double=(now-G_last_click_desk<450&&G_last_click_idx==di); G_last_click_desk=now;G_last_click_idx=di; if(G_double){ if(G_desk[di].kind==4)launch_app("trash"); else if(G_desk[di].kind<=3)launch_app("/bin/files"); else launch_app(G_desk[di].path); G_double=false;} else { int x0,y0,x1,y1; desk_rect(di,&x0,&y0,&x1,&y1); G_drag_icon=di; G_icon_dx=x-x0; G_icon_dy=y-y0; G_icon_drag=true; } return; }
    if(desktop_area(y)){ G_sel_desk=-1; G_desktop_drag=true; G_marquee=false; G_mx0=G_mx1=x; G_my0=G_my1=y; wm_focus(0); return; }
    int dk=dock_hit(x,y); if(dk>=0){ launch_dock(dk); return; }
    wm_focus(0); G_focus_win=-1;
}
void win_snap(win_t*w,int side){
    if(!w)return;
    if(side==3){ toggle_max(w); return; }
    if(w->maximized){ /* restore from maximize to saved geometry first */
        wm_move(w->id,w->saved_x,w->saved_y); w->w=w->saved_w; w->h=w->saved_h; w->maximized=false;
    }
    if(w->snap!=side){
        if(!w->snap){ w->saved_x=w->x; w->saved_y=w->y; w->saved_w=w->w; w->saved_h=w->h; }
        int gap=6, top=PANEL_H+4;
        if(side==1){ w->x=gap; w->y=top; w->w=G_fb.w/2-gap; w->h=G_fb.h-top-gap; }
        else if(side==2){ w->x=G_fb.w/2; w->y=top; w->w=G_fb.w/2-gap; w->h=G_fb.h-top-gap; }
        wm_move(w->id,w->x,w->y); wm_resize(w->id,w->w,w->h); w->snap=side;
    } else { /* unsnap */
        w->x=w->saved_x; w->y=w->saved_y; w->w=w->saved_w; w->h=w->saved_h;
        wm_move(w->id,w->x,w->y); wm_resize(w->id,w->w,w->h); w->snap=0;
    }
    g_backdrop_dirty=true; damage_whole();
}
static win_t*focused_win(void){ if(G_focus_win<0)return 0; win_t*w=&G_win[G_focus_win]; if(w->active)return w; return 0; }
static void handle_drag(int x,int y){
    if(G_shot_region && G_marquee){
        G_mx1 = x; G_my1 = y;
        damage_whole();
        return;
    }
    if(G_quick){ int bx=G_quick_x+16, slx=bx+26, slw=G_quick_w-58;
        if(G_mb&1 && x>=slx-4 && x<=slx+slw+4){ int v=(x-slx)*100/slw; if(v<0)v=0; if(v>100)v=100; G_vol=v; G_audio=v>0; audio_vol(v); damage_add((GfxRect){G_quick_x-4,G_quick_y-4,G_quick_w+8,G_quick_h+8}); return; } }
    if(G_resize_win>=0&&G_resize_win<MAX_WIN&&G_win[G_resize_win].active){ win_t*w=&G_win[G_resize_win];
        int cx0, cy0, cw0, ch0; win_client_rect(w, &cx0, &cy0, &cw0, &ch0);
        int nx=cx0, ny=cy0, nw=cw0, nh=ch0;
        if(G_resize_edges&1){ /* left  : right edge stays, left follows cursor */ int n=cx0+cw0-x; if(n>=240){ nx=x; nw=n; } }
        if(G_resize_edges&2){ /* right : left stays, right follows cursor  */ nw=x-cx0; if(nw<240)nw=240; }
        if(G_resize_edges&4){ /* top   : bottom stays, titlebar top follows */ int nt=y+TB_H; int nh2=(cy0+ch0)-nt; if(nh2>=120&&nt>=PANEL_H+TB_H){ ny=nt; nh=nh2; } }
        if(G_resize_edges&8){ /* bottom: top stays, bottom follows cursor   */ nh=y-cy0; if(nh<120)nh=120; }
        if(nx<0){nx=0;} if(nx+nw>G_fb.w){nw=G_fb.w-nx;}
        /* kernel surface caps (WM_SURF_MAX_*): never ask the kernel to resize
         * beyond them or wm_resize() fails and the rect disagrees next scan. */
        if(nw>640)nw=640;
        if(nh>480)nh=480;
        wm_move(w->id,nx,ny); wm_resize(w->id,nw,nh); w->x=nx;w->y=ny;w->w=nw;w->h=nh;w->dirty=true; }
    else if(G_title_drag&&G_drag_win>=0&&G_drag_win<MAX_WIN&&G_win[G_drag_win].active){
        /* drag slop: ignore tiny movements so clicks don't jiggle the window */
        if(absi(x-G_pressed_x)+absi(y-G_pressed_y) < 5) return;
        win_t*w=&G_win[G_drag_win];
        /* G_drag_dx/dy = grab offset within the client/titlebar, so the window
         * tracks the cursor exactly.  Client top = titlebar top + TB_H. */
        int nx=x-G_drag_dx;
        int ny=(y-G_drag_dy)+TB_H;
        if(nx<0)nx=0;
        if(nx+w->w>G_fb.w)nx=G_fb.w-w->w;
        if(ny<PANEL_H+TB_H)ny=PANEL_H+TB_H;
        wm_move(w->id,nx,ny); w->x=nx;w->y=ny;w->dirty=true; }
    if(G_icon_drag&&G_drag_icon>=0){ int nx=x-G_icon_dx,ny=y-G_icon_dy; if(nx<8)nx=8; if(ny<PANEL_H+8)ny=PANEL_H+8; if(nx>G_fb.w-96)nx=G_fb.w-96; if(ny>G_dock_y-96)ny=G_dock_y-96; G_desk[G_drag_icon].gx=nx; G_desk[G_drag_icon].gy=ny; g_backdrop_dirty=true; damage_whole(); }
    else if(G_desktop_drag&&desktop_area(G_pressed_y)){ if(absi(x-G_pressed_x)>4||absi(y-G_pressed_y)>4)G_marquee=true;
        GfxRect oldr={(G_mx0<G_mx1?G_mx0:G_mx1)-1,(G_my0<G_my1?G_my0:G_my1)-1,absi(G_mx1-G_mx0)+2,absi(G_my1-G_my0)+2};
        G_mx1=x;G_my1=y; if(G_marquee)G_multi_sel=true;
        GfxRect newr={(G_mx0<G_mx1?G_mx0:G_mx1)-1,(G_my0<G_my1?G_my0:G_my1)-1,absi(G_mx1-G_mx0)+2,absi(G_my1-G_my0)+2};
        damage_add(oldr); damage_add(newr);
    }
}
static void handle_release(int x,int y,int button){
    if(button!=1)return;
    if(G_shot_region && G_marquee){
        G_shot_region = false; G_marquee = false;
        int x0 = G_mx0 < G_mx1 ? G_mx0 : G_mx1;
        int y0 = G_my0 < G_my1 ? G_my0 : G_my1;
        int x1 = G_mx0 > G_mx1 ? G_mx0 : G_mx1;
        int y1 = G_my0 > G_my1 ? G_my0 : G_my1;
        /* round to multiples of 16 (encoder's 4:2:0 requirement) */
        int w = (x1 - x0) & ~15, h = (y1 - y0) & ~15;
        if (w >= 16 && h >= 16) {
            mkdir("/home/yart/Screenshots");
            char path[96]; int k = 0; const char *p = "/home/yart/Screenshots/region_";
            while (*p) path[k++] = *p++;
            char num[16]; int j = 0, v = shot_counter++; if (!v) num[j++] = '0';
            while (v) { num[j++] = (char)('0' + v % 10); v /= 10; }
            while (j) path[k++] = num[--j];
            const char *e = ".jpg"; while (*e) path[k++] = *e++;
            path[k] = 0;
            capture_rect(x0 & ~15, y0 & ~15, w, h, path);
            osd("Region captured");
        } else {
            osd("Region too small");
        }
        damage_whole();
        return;
    }
    if(G_grid&&G_sel_app>=0){ int ai; if(app_grid_hit(x,y,&ai)&&ai==G_sel_app){launch_app(G_app[ai].path);G_grid=false;} G_sel_app=-1; }
    if(G_title_drag){
        G_title_drag=false; G_drag_win=-1;
        /* edge snapping */
        win_t*w=focused_win();
        if(w && !w->maximized){
            if(x<=4) win_snap(w,1);
            else if(x>=G_fb.w-5) win_snap(w,2);
            else if(y<=PANEL_H+2) win_snap(w,3);
            else if(w->snap){ /* dragged away from snap -> restore */
                w->x=x-w->w/2; w->y=y-15; if(w->x<0)w->x=0; if(w->y<PANEL_H)w->y=PANEL_H;
                wm_move(w->id,w->x,w->y); w->snap=0; g_backdrop_dirty=true; damage_whole();
            }
        }
    }
    if(G_resize_win>=0){G_resize_win=-1;}
    if(G_icon_drag){
        G_icon_drag=false;
        if(G_drag_icon>=0){
            /* snap to 104x100 grid */
            int gx=(G_desk[G_drag_icon].gx+52*G_scale)/(104*G_scale)*(104*G_scale)+18;
            int gy=((G_desk[G_drag_icon].gy-PANEL_H-18*G_scale+50*G_scale)/(100*G_scale))*(100*G_scale)+PANEL_H+18*G_scale;
            if(gx<18){gx=18;} if(gy<PANEL_H+18*G_scale){gy=PANEL_H+18*G_scale;}
            if(gx>G_fb.w-104*G_scale){gx=G_fb.w-104*G_scale;} if(gy>G_dock_y-100*G_scale){gy=G_dock_y-100*G_scale;}
            /* avoid overlap with other icons */
            for(int attempt=0;attempt<60;attempt++){
                int clash=0;
                for(int j=0;j<G_desk_n;j++) if(j!=G_drag_icon){
                    int dx=gx-G_desk[j].gx, dy=gy-G_desk[j].gy;
                    if(dx>-80&&dx<80&&dy>-80&&dy<80){clash=1;break;}
                }
                if(!clash)break;
                gx+=104*G_scale; if(gx>G_fb.w-104*G_scale){gx=18;gy+=100*G_scale;}
            }
            G_desk[G_drag_icon].gx=gx; G_desk[G_drag_icon].gy=gy;
            save_config();
        }
        G_drag_icon=-1;
    }
    if(G_desktop_drag){ G_desktop_drag=false; G_marquee=false; G_multi_sel=false; damage_whole(); }
}
/* Advance the window switcher to the next window (or open it on first press).
 * Shared by Alt+Tab and Super+Tab. */
static void switcher_advance(void){
    if(!G_switcher) G_switcher_idx = -1;        /* first press: start fresh */
    G_switcher = true;
    int start = G_switcher_idx;
    for(int step=0; step<MAX_WIN; step++){
        G_switcher_idx = (G_switcher_idx+1) % MAX_WIN;
        win_t *c = &G_win[G_switcher_idx];
        if(c->active && !c->minimized && !c->closing && (c->workspace==G_workspace || c->workspace<0)) break;
    }
    if(start == G_switcher_idx && !(G_win[G_switcher_idx].active && !G_win[G_switcher_idx].minimized && !G_win[G_switcher_idx].closing && (G_win[G_switcher_idx].workspace==G_workspace || G_win[G_switcher_idx].workspace<0))){
        G_switcher = false;
    }
    damage_whole();
}

static void handle_key(int ev){
    int ascii=ev&255, make=!(ev&WM_KEY_RELEASE), sc=(ev>>8)&255;
    /* Track Super as a modifier (it arrives as an E0-prefixed 5B/5C). */
    if((ev&WM_KEY_EXT) && (sc==0x5B || sc==0x5C)) G_super_held = make;
    if(!make)return;
    /* Locked: real password auth.  Enter opens the password field, the
     * password is verified against the account hash (SYS_AUTH_VERIFY —
     * no elevation), Esc cancels. */
    if(G_locked){
        if(!G_lock_prompt){
            if(ascii==13 || ascii==10){
                G_lock_prompt=true; G_lock_pw_len=0; G_lock_pw[0]=0; G_lock_bad=false;
                damage_whole();
            }
            return;
        }
        if(ascii==27){
            G_lock_prompt=false; G_lock_pw_len=0; G_lock_pw[0]=0; G_lock_bad=false;
            damage_whole(); return;
        }
        if(ascii==13 || ascii==10){
            G_lock_pw[G_lock_pw_len]=0;
            if(auth_verify(G_lock_pw)==0){
                G_locked=false; G_lock_prompt=false; G_lock_bad=false;
                G_lock_pw_len=0; G_lock_pw[0]=0; G_super_held=false;
            } else {
                G_lock_bad=true; G_lock_pw_len=0; G_lock_pw[0]=0;
            }
            damage_whole(); return;
        }
        if(ascii==8 || ascii==127){
            if(G_lock_pw_len>0){ G_lock_pw_len--; G_lock_pw[G_lock_pw_len]=0; }
            damage_whole(); return;
        }
        if(ascii>=32 && ascii<127 && G_lock_pw_len < (int)sizeof(G_lock_pw)-1){
            G_lock_pw[G_lock_pw_len++]=(char)ascii; G_lock_pw[G_lock_pw_len]=0;
            damage_whole();
        }
        return;
    }
    /* Super+L -> lock screen (Skift parity). */
    if(G_super_held && sc==0x26){ G_locked=true; G_super_held=false; damage_whole(); return; }
    /* Ctrl+W -> close the focused window (standard enterprise-OS shortcut). */
    if((ev&WM_KEY_CTRL) && sc==0x11){ win_t *fw=focused_win(); if(fw) close_win(fw); return; }
    /* Ctrl+Alt+Backspace -> end the graphical session (classic X11 "zap").
     * wm_run() returns, init exits, the kernel reclaims the framebuffer and
     * paints the text fallback screen. */
    if((ev&WM_KEY_CTRL) && (ev&WM_KEY_ALT) && sc==0x0E){
        klog("wm: Ctrl+Alt+Backspace - ending session\n");
        wm_session_end();
        return;
    }
    /* Super+Tab -> window switcher (all windows), the "Win+Tab task view" /
     * "GNOME Super+Tab" model.  Super alone opens the app launcher; pressing
     * Tab while Super is held switches to the window switcher instead.  Must
     * run BEFORE the G_grid search handler, which would otherwise swallow Tab. */
    if(G_super_held && sc==0x0F){ G_grid=false; G_dockmenu=false; switcher_advance(); return; }
    if(G_dockmenu){ if(ascii>=32&&ascii<127&&G_search_len<(int)sizeof(G_search)-1){G_search[G_search_len++]=(char)ascii;G_search[G_search_len]=0;} else if(ascii==8||ascii==127){if(G_search_len>0){G_search_len--;G_search[G_search_len]=0;}} else if(ascii==27){G_dockmenu=false;G_search_len=0;G_search[0]=0;} else if(ascii==13||ascii==10){ for(int i=0;i<G_apps;i++) if(search_match(G_app[i].name)){launch_app(G_app[i].path);G_dockmenu=false;G_search_len=0;G_search[0]=0;break;} } return; }
    if(G_grid){ if(ascii>=32&&ascii<127&&G_search_len<(int)sizeof(G_search)-1){G_search[G_search_len++]=(char)ascii;G_search[G_search_len]=0;} else if(ascii==8||ascii==127){if(G_search_len>0){G_search_len--;G_search[G_search_len]=0;}} else if(ascii==27){G_grid=false;G_search_len=0;G_search[0]=0;} else if(ascii==13||ascii==10){ for(int i=0;i<G_apps;i++) if(search_match(G_app[i].name)){launch_app(G_app[i].path);G_grid=false;break;} } return; }
    if(ascii==27){ if(G_switcher){ if(G_switcher_idx>=0&&G_switcher_idx<MAX_WIN){win_t*w=&G_win[G_switcher_idx]; if(w->active){bring_front(w); wm_focus(w->owner); G_focus_win=G_switcher_idx; if(w->minimized)restore_win(w);}} G_switcher=false; g_backdrop_dirty=true; damage_whole(); return; } close_all_overlays();return;}
    /* Super (Meta) opens the launcher — Skift: Super+Space / search pill */
    if((ev&WM_KEY_EXT) && (sc==0x5B || sc==0x5C)){ G_grid=!G_grid; if(G_grid){G_search_len=0; G_search[0]=0;} return; }
    if(sc==0x3B||sc==0x3F){ G_grid=!G_grid; if(G_grid){G_search_len=0; G_search[0]=0;} return; }
    if(sc==0x3C||sc==0x3D){ show_desktop_toggle(); }
    /* PrintScreen (E0 0x37): full screenshot.  Alt+PrintScreen: focused window.
     * Shift+PrintScreen: drag a region to capture. */
    if((ev&WM_KEY_EXT) && sc==0x37){
        if(ev & (1<<17)){ G_shot_region=true; osd("Drag to select screenshot area"); damage_whole(); return; }
        if(ev & WM_KEY_ALT){ screenshot_window(); return; }
        screenshot_full(); return;
    }
    /* Super+R: toggle screen recording */
    if(G_super_held && sc==0x13){ record_toggle(); return; }
    /* F9: full screenshot, F10: toggle recording (alternates that map
     * cleanly through QMP/PS/2, unlike PrintScreen's E0 2A E0 37 sequence). */
    if(sc==0x43){ screenshot_full(); return; }
    if(sc==0x44){ record_toggle(); return; }
    if(sc==0x3E){ theme_reset_defaults(); if(theme_load("/home/yart/theme.ini")==0) osd("Theme reloaded (F4)"); else osd("Theme reset (F4)"); g_backdrop_dirty=true; damage_whole(); return; }
    /* Workspace switching: Super+1..4 or Ctrl+Alt+Left/Right */
    if((ev&WM_KEY_CTRL) && sc>=0x03 && sc<=0x06){ int ws=sc-0x03; if(ws<MAX_WORKSPACES && ws!=G_workspace){ G_workspace=ws; char b[32]; int k=0; const char*q="Workspace "; while(q[k]){b[k]=q[k];k++;} b[k++]='0'+1+ws; b[k]=0; osd(b); g_backdrop_dirty=true; damage_whole(); wm_focus(0); } return; }
    if((ev&WM_KEY_CTRL) && sc==0x4B){ if(G_workspace>0){G_workspace--; osd("Workspace left"); g_backdrop_dirty=true; damage_whole();} return; }
    if((ev&WM_KEY_CTRL) && sc==0x4D){ if(G_workspace<MAX_WORKSPACES-1){G_workspace++; if(G_workspace>=G_ws_count)G_ws_count=G_workspace+1; char b[32];int k=0;const char*q="Workspace ";while(q[k]){b[k]=q[k];k++;} b[k++]='0'+1+G_workspace; b[k]=0; osd(b); g_backdrop_dirty=true; damage_whole();} return; }
    /* Ctrl+Alt+Up/Down -> Mission-Control-style overview */
    if((ev&WM_KEY_CTRL) && (ev&WM_KEY_ALT) && (sc==0x48 || sc==0x50)){ G_overview=!G_overview; return; }
    /* Alt+Tab window switcher (with live previews) — Super+Tab does the same */
    if((ev&WM_KEY_ALT) && sc==0x0F){ switcher_advance(); return; }
}
static int cursor_pick(void){
    int x=G_cx,y=G_cy;
    if(G_menu){ if(menu_hit(x,y)>=-2) return 1; }
    win_t *w=win_at(x,y);
    if(w){
        int cx0, cy0, cw0, ch0; win_client_rect(w, &cx0, &cy0, &cw0, &ch0);
        int ty=cy0-TB_H; if(ty<PANEL_H)ty=PANEL_H;
        if(ptin(x,y,cx0,ty,cx0+cw0,ty+TB_H)) return 1;
        /* resize cursor over the window edges (Skift ResizeCursor) */
        if(!w->maximized){
            int onl=x>=cx0-4&&x<=cx0+6, onr=x<=cx0+cw0+4&&x>=cx0+cw0-6;
            int onb=y<=ty+TB_H+ch0+4&&y>=ty+TB_H+ch0-6;
            int ont=y>=ty-4&&y<=ty+4;
            if(onl||onr||onb||ont) return 2;
        }
    }
    if(tray_hit(x,y)>=0||dock_hit(x,y)>=0||desk_hit(x,y)>=0) return 1;
    if(y<PANEL_H) return 1;  /* entire panel is interactive */
    if(G_grid||G_quick||G_calendar||G_overview) return 1;
    return 0;
}
/* Cursor compositing (Skift-style): every frame we damage the rectangle the
 * cursor occupied last frame (so the scene under it is recomposited from the
 * backdrop + windows) and the rectangle it will occupy this frame (so it is
 * presented to the scanout).  No pixel save/restore buffer is needed: the
 * per-rect repaint re-derives the pixels under the cursor from the scene. */
static int G_cursor_last_x, G_cursor_last_y, G_cursor_last_w, G_cursor_last_h;
static bool G_cursor_moved = false;
/* Cursor images are pre-scaled once to their draw size by cursors_draw_img()
 * (see cursors.c); the per-frame path is a plain integer alpha blit - no
 * per-pixel float math, so the pointer glides instead of lagging/jumping. */
static void cursor_blit(surface_t *dst, int dx, int dy, const cursor_img_t *im){
    for(int y = 0; y < im->h; y++){
        int yy = dy + y; if(yy < 0 || yy >= dst->h) continue;
        const u32 *sp = im->px + (long)y * im->w;
        u32 *dp = dst->px + (long)yy * dst->pitch + dx;
        for(int x = 0; x < im->w; x++){
            int xx = dx + x; if(xx < 0 || xx >= dst->w) continue;
            u32 c = sp[x];
            u32 sa = (c >> 24) & 0xFF;
            if(sa == 0) continue;
            u32 d = dp[x];
            if(sa == 255) { dp[x] = c; continue; }
            u32 sr = (c >> 16) & 0xFF, sg = (c >> 8) & 0xFF, sb = c & 0xFF;
            u32 dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
            u32 ia = 255 - sa;
            dp[x] = 0xFF000000
                  | (((sr*sa + dr*ia + 127)/255) << 16)
                  | (((sg*sa + dg*ia + 127)/255) << 8)
                  |  ((sb*sa + db*ia + 127)/255);
        }
    }
}
static void cursor_damage_prev(void){
    if(G_cursor_last_w > 0) damage_add((GfxRect){G_cursor_last_x, G_cursor_last_y, G_cursor_last_w, G_cursor_last_h});
}

/* Draw-size cursor at the current position/kind (pre-scaled image). */
static cursor_img_t *cursor_current_img(int *dx, int *dy){
    if(G_cur_theme<0){ G_cur_theme=cursors_theme_by_name("roblox"); if(G_cur_theme<0) G_cur_theme=0; }
    int k = cursor_pick();
    cursor_img_t *im = cursors_draw_img(G_cur_theme, k);
    if(!im && k == 2) im = cursors_draw_img(G_cur_theme, 0);  /* no resize: arrow */
    if(im){
        *dx = G_cx - im->hotx;
        *dy = G_cy - im->hoty;
        return im;
    }
    *dx = G_cx - 14; *dy = G_cy - 14;
    return NULL;
}

/* Footprint of the cursor at its current position/kind.  Used to damage the
 * cursor region BEFORE the compositing pass so the cursor is presented in
 * the SAME frame it moved to (Skift draws the cursor in the render pass;
 * damaging it only for the next frame made it trail 16ms + ghost). */
static GfxRect cursor_rect(void){
    int dx, dy;
    cursor_img_t *im = cursor_current_img(&dx, &dy);
    if(im) return (GfxRect){dx, dy, im->w, im->h};
    return (GfxRect){dx, dy, 28, 28};
}

static void cursor_draw(surface_t*s){
    int dx, dy;
    cursor_img_t *im = cursor_current_img(&dx, &dy);
    if(im){
        cursor_blit(s, dx, dy, im);
        G_cursor_last_x=dx; G_cursor_last_y=dy;
        G_cursor_last_w=im->w; G_cursor_last_h=im->h;
    } else {
        icon_t ic=icon_get(cursor_pick()==1?ICON_CURSOR_HAND:ICON_CURSOR_ARROW);
        sf_icon_scaled(s,G_cx,G_cy,ic,0,24,48);
        G_cursor_last_x=dx; G_cursor_last_y=dy;
        G_cursor_last_w=28; G_cursor_last_h=28;
    }
}

/* ---------- Skift-style compositor core ---------- */
/* The backdrop buffer holds the static base scene (wallpaper + desktop
 * icons).  It is rebuilt only when g_backdrop_dirty is set; the per-frame
 * repaint blits just the damaged rectangles out of it, exactly like Skift's
 * backbuffer/frontbuffer split. */
static void rebuild_backdrop(void){
    if(!G_backdrop.px) return;
    sf_blit(&G_backdrop, 0, 0, &G_wp, 0, 0, G_fb.w, G_fb.h);
    draw_desktop_icons(&G_backdrop);
    /* frosted-glass dock: blur the backdrop region behind the dock pill */
    if(G_blur_w > 0 && G_blur_h > 0){
        if(!G_dock_blur.px || G_dock_blur.w != G_blur_w || G_dock_blur.h != G_blur_h){
            if(G_dock_blur.px) sf_free(&G_dock_blur);
            G_dock_blur = sf_alloc(G_blur_w, G_blur_h);
        }
        if(G_dock_blur.px){
            sf_blit(&G_dock_blur, 0, 0, &G_backdrop, G_blur_x, G_blur_y, G_blur_w, G_blur_h);
            sf_blur_rect(&G_dock_blur, 0, 0, G_blur_w, G_blur_h, 12, 2);
            /* frosted-glass: lighten the blurred region */
            sf_fill_rect_blend(&G_dock_blur, 0, 0, G_blur_w, G_blur_h, ARGB(28,255,255,255));
        }
    }
    g_backdrop_dirty = false;
}
/* Repaint a single damaged rectangle with the global clip set to it, so no
 * draw call can bleed outside the damaged region (Skift g.clip(r) model).
 * Called once per dirty rect. */
static void composite_rect(GfxRect r, long now){
    sf_set_clip(r.x, r.y, r.w, r.h);
    sf_blit(&G_fb, 0, 0, &G_backdrop, 0, 0, G_fb.w, G_fb.h);
    /* live chrome, each gated to the dirty rect so a small cursor repaint
     * does NOT redraw the whole panel/dock (the old code did, every rect) */
    GfxRect pr = {0, 0, G_fb.w, PANEL_H};
    if(rect_colide(r, pr)) draw_panel(&G_fb);
    GfxRect dr = {G_dock_x-30, G_dock_y-80, G_dock_w+60, G_dock_h+170};
    if(rect_colide(r, dr)) draw_dock(&G_fb, now);
    GfxRect der = {0, PANEL_H, G_fb.w, G_fb.h-PANEL_H};
    if(rect_colide(r, der)) draw_desktop_live(&G_fb);
    /* windows in z-order, only those intersecting this rect */
    for(int z=0; z<=G_z_top; z++){
        for(int i=0;i<MAX_WIN;i++){
            win_t *w=&G_win[i];
            if(!w->active || w->z!=z) continue;
            if(w->workspace!=G_workspace && w->workspace>=0) continue;
            if(w->minimized || w->hidden) continue;
            GfxRect wr; win_draw_rect(w, &wr);
            if(!rect_colide(r, wr)) continue;
            draw_window(w);
        }
    }
    /* overlays, topmost.  Drawn unconditionally (the global clip already
     * confines the actual pixel writes to this dirty rect) because their
     * geometry is only assigned INSIDE the draw call.  Gating them on
     * rect_colide() against their own (initially zero) bounds meant they
     * NEVER rendered: on the first frame G_quick_x/y/w/h are all 0, so the
     * 0x0 rect never collides, draw_quick never runs, and the bounds stay 0
     * forever.  Clicking the clock / clipboard / wifi-chevron / status
     * cluster therefore showed nothing.  (app_grid/overview/switcher/menu
     * below were already drawn unconditionally for the same reason.) */
    if(G_quick)        draw_quick(&G_fb);
    if(G_calendar)     draw_calendar(&G_fb);
    if(G_clip_open)    draw_clipboard(&G_fb);
    if(G_netlist_open) draw_netlist(&G_fb);
    if(G_dockmenu)     draw_dockmenu(&G_fb);
    if(G_grid) draw_app_grid(&G_fb);
    if(G_overview) draw_overview(&G_fb);
    if(G_switcher) draw_switcher(&G_fb);
    draw_menu(&G_fb);
    draw_osd(&G_fb, now);
    if(G_locked) draw_lock(&G_fb, now);
    sf_clear_clip();
}

/* ---------- entry / render loop ---------- */
void wm_run(void){
    /* Claim the framebuffer.  When a session is RESTARTED (startwm), the
     * previous getty child is being reaped right as we exec, so the kernel
     * may not have cleared its fb claim yet - poll briefly, like the text
     * console does. */
    fb_info_t fi; void*fb=0;
    for(int i=0;i<200 && !fb;i++){ fb=fb_info(&fi); if(!fb) sleep(10); }
    if(!fb) return;
    G_fb.px=fb; G_fb.w=fi.w; G_fb.h=fi.h; G_fb.pitch=fi.pitch; G_cx=G_fb.w/2; G_cy=G_fb.h/2;
    G_backdrop=sf_alloc(G_fb.w,G_fb.h);
    if(!G_backdrop.px){ klog("wm: backdrop alloc failed\n"); return; }
    g_backdrop_dirty=true; damage_whole();
    assets_init(); cursors_init();
    { int gs=gfx_selftest(); klog(gs==0?"gfx: SIMD blit selftest ok (bit-exact)":"gfx: SIMD blit selftest FAILED"); }
    if(wallpaper_load(&G_wp)!=0||G_wp.w!=G_fb.w||G_wp.h!=G_fb.h){ klog("wm: wallpaper failed\n"); return; }
    load_all(); rebuild_dock_cache();
    theme_reset_defaults();
    theme_load("/home/yart/theme.ini");
    settings_load();          /* apply persisted accent/cursor/wallpaper/dock/volume */
    mkdir("/home/yart/.trash"); /* ensure the trash dir exists (real Trash) */
    G_start_ms=time_ms();
    long wt=wall_time(); if(wt>0){
        format_wallclock(wt,G_clk);format_date(wt,G_date);
        /* calendar opens on the REAL month/year, not a hardcoded date */
        long ds=wt/1000000ULL;
        G_cal_year=(int)(ds/10000);
        G_cal_month=(int)((ds/100)%100);
        if(G_cal_month<1||G_cal_month>12)G_cal_month=1;
    }
    osd("YartOS ready");
    /* calibrate the TSC rate for sub-ms frame pacing (50 ms span) */
    {   u64 t0 = wm_rdtsc(); long m0 = time_ms();
        sleep(50);
        u64 t1 = wm_rdtsc(); long m1 = time_ms();
        if(m1 > m0 && t1 > t0){ g_tsc_per_ms = (t1 - t0) / (u64)(m1 - m0); g_tsc_base = t1; g_ms_base = m1; }
    }
    bool first_frame=true;
    int prev_overlay_state=-1;
    bool osd_was_visible=false;
    static bool s_overlay_focus=false;
    long next_frame=time_ms();
    for(;;){
        /* ---- session end (Ctrl+Alt+Backspace) ---- */
        if(G_session_end) break;
        /* cursor position at the START of this frame (for hover damage) */
        int frm_prev_cx = G_cx, frm_prev_cy = G_cy;

        /* ---- input ---- */
        bool moved=false;
        int ev; while((ev=poll_key())!=0){ handle_key(ev); moved=true; }
        mouse_ev_t m;
        while(poll_mouse(&m)){
            int oldx=G_cx, oldy=G_cy;
            G_cx+=m.dx; G_cy+=m.dy;
            if(G_cx<0) G_cx=0;
            if(G_cy<0) G_cy=0;
            if(G_cx>=G_fb.w) G_cx=G_fb.w-1;
            if(G_cy>=G_fb.h) G_cy=G_fb.h-1;
            unsigned char prev=G_mb; G_mb=m.buttons;
            if((G_mb&1)&&!(prev&1))handle_press(G_cx,G_cy,1);
            if((G_mb&2)&&!(prev&2))handle_press(G_cx,G_cy,3);
            if((G_mb&4)&&!(prev&4))handle_press(G_cx,G_cy,2);
            if(G_mb&1)handle_drag(G_cx,G_cy);
            if((prev&1)&&!(G_mb&1))handle_release(G_cx,G_cy,1);
            if(G_cx!=oldx || G_cy!=oldy) G_cursor_moved = true;
            moved=true;
        }

        /* ---- cursor damage: ONLY when it moved (or the scene under a
         * stationary cursor was repainted).  Damaging the cursor rect every
         * frame unconditionally made the compositor redraw forever even when
         * completely idle -> a core pinned at 100% (the "heavy" feel). ---- */
        if(G_cursor_moved){
            cursor_damage_prev();        /* repaint what was under the old pos */
            damage_add(cursor_rect());   /* paint the cursor this same frame  */
            /* Panel button hover highlights are drawn from the cursor position
             * and are WIDER than the cursor footprint, so moving off a button
             * left a stale "brushed" highlight behind (the dock already
             * handles this in dock_update()).  Damage the panel strip between
             * the old and new cursor X so the hover state repaints cleanly. */
            if(frm_prev_cy < PANEL_H + 6 || G_cy < PANEL_H + 6){
                int x0 = (frm_prev_cx < G_cx ? frm_prev_cx : G_cx) - 44;
                int x1 = (frm_prev_cx > G_cx ? frm_prev_cx : G_cx) + 44;
                if(x0 < 0) x0 = 0;
                if(x1 > G_fb.w) x1 = G_fb.w;
                damage_add((GfxRect){x0, 0, x1 - x0, PANEL_H});
            }
            G_cursor_moved = false;
        } else if(G_ndirty > 0){
            damage_add(cursor_rect());   /* scene changed under the cursor    */
        }

        /* ---- housekeeping (throttled: these are syscalls) ---- */
        {
            static long ln=0;
            long nnow=time_ms();
            if(nnow-ln >= 500){ unsigned ni[5]; G_net_up=(net_info(ni)==0&&ni[4]!=0); ln=nnow; }
        }
        static int hf=0;
        /* reap dead children at most ~2x/s.  pid_forget_dead() issues one
         * waitpid_nohang() syscall PER recorded pid, and it ran every frame
         * (up to MAX_PIDS=24 syscalls/frame) - a large chunk of per-frame
         * latency under TCG.  Reaping twice a second is plenty: a closed app
         * shows as vanished within 500 ms. */
        static long last_reap=0;
        long rnow=time_ms();
        if(rnow-last_reap >= 500){ pid_forget_dead(); last_reap=rnow; }
        if((hf++ & 1)==0) scan_windows();   /* every other frame (32ms) */
        /* drain the kernel notification ring: toast + calendar list */
        char nb[128];
        long nr;
        while((nr=notify_poll(nb,sizeof nb))>0){
            osd(nb);

            if(G_notif_n < 16) copy_str(G_notifs[G_notif_n++], nb, 128);
            else { for(int q=0;q<15;q++) copy_str(G_notifs[q], G_notifs[q+1], 128); copy_str(G_notifs[15], nb, 128); }
        }
        (void)nr;
        long now=time_ms(),up=now-G_start_ms; unsigned long sec=up/1000;

        /* ---- panel clock ---- */
        if(sec!=G_last_sec){
            long wt2=wall_time(); if(wt2>0)format_wallclock(wt2,G_clk);
            else {int h=(int)((up/3600000)%24),mi=(int)((up/60000)%60); G_clk[0]='0'+h/10;G_clk[1]='0'+h%10;G_clk[2]=':';G_clk[3]='0'+mi/10;G_clk[4]='0'+mi%10;G_clk[5]=0;}
            G_last_sec=sec;
            int cw=sf_text_width(G_date)+sf_text_width(G_clk)+24;   /* + ", " */
            int cx=G_fb.w/2-cw/2;
            damage_add((GfxRect){cx-8,0,cw+16,PANEL_H});
            if(G_locked) damage_whole();   /* keep the lock-screen clock fresh */
        }

        /* ---- persisted settings (Settings app writes settings.conf) ---- */
        settings_poll(now);

        /* ---- installed-app scan: `apk add/del` writes .desktop files, so
         * re-scan every ~2s to keep the Super launcher in sync ---- */
        static long last_appscan=0;
        if(now - last_appscan >= 2000){ last_appscan = now; scan_desktop_apps(); }

        /* screen recording: capture a frame ~10x/s while recording */
        if(G_recording && now - G_rec_last >= 100){
            G_rec_last = now;
            record_frame();
            G_rec_frame++;
            if((G_rec_frame & 7) == 0) osd("recording...");
        }

        /* ---- dock animation/hover ---- */
        dock_update(now);

        /* ---- window animation + damage ---- */
        windows_update_and_damage(now);

        /* ---- overlay open/close: full damage + keyboard focus routing ----
         * While any overlay is up, the wm owns the keyboard (search typing,
         * Esc); when the last overlay closes, focus returns to the top
         * window so apps get their typing back. */
        /* Every popover MUST be in this mask.  G_clip_open and G_netlist_open
         * were MISSING, so opening/closing the clipboard or the Wi-Fi network
         * list never triggered a full repaint: their pixels lingered on screen
         * after "click elsewhere to dismiss" (they never cleared), and moving
         * the cursor over the stale popover redrew the backdrop in a trail -
         * the "brushing" erase the user saw. */
        int overlay_state = G_grid | (G_overview<<1) | (G_switcher<<2) |
                           (G_quick<<3) | (G_calendar<<4) |
                           (G_dockmenu<<5) | (G_menu<<6) |
                           (G_clip_open<<7) | (G_netlist_open<<8);
        if(overlay_state != prev_overlay_state){
            damage_whole();
            prev_overlay_state = overlay_state;
            if(overlay_state && !s_overlay_focus){
                s_overlay_focus = true;
                wm_focus(0);                      /* overlays own the keys */
            } else if(!overlay_state && s_overlay_focus){
                s_overlay_focus = false;
                if(G_focus_win>=0 && G_focus_win<MAX_WIN){
                    win_t *fw=&G_win[G_focus_win];
                    if(fw->active && !fw->minimized && (fw->workspace==G_workspace||fw->workspace<0))
                        wm_focus(fw->owner);      /* restore typing to app  */
                }
            }
        } else if(moved){
            bool overlay_full = G_grid || G_overview || G_switcher;
            bool overlay_small = G_quick || G_calendar || G_dockmenu || G_menu || G_clip_open || G_netlist_open;
            if(overlay_full) damage_whole();
            else if(overlay_small) damage_overlay_small();
        }

        /* ---- OSD toast (has a fade animation; keep damaging while live) ---- */
        bool osd_visible = G_osd[0] && (now-G_osd_t0)<1800;
        if(osd_visible || osd_was_visible){
            damage_add(osd_rect());
        }
        osd_was_visible = osd_visible;

        /* ---- first frame + backdrop ---- */
        if(first_frame){ g_backdrop_dirty=true; damage_whole(); first_frame=false; }
        if(g_backdrop_dirty) rebuild_backdrop();

        /* ---- repaint every damaged rect, clipped ---- */
        for(int d=0; d<G_ndirty; d++) composite_rect(G_dirty[d], now);

        /* ---- cursor on top ---- */
        cursor_draw(&G_fb);

        /* ---- present only the damaged rectangles (Skift blitUnsafe) ---- */
        fb_present(fb, (const fb_rect_t*)G_dirty, G_ndirty);
        G_ndirty = 0;

        /* ---- frame pacing (Skift: sleepAsync(lastFrame + interval)) ----
         * When the user is actively moving the mouse / typing, poll + render
         * at ~125 Hz (8 ms) instead of 60 Hz (16 ms), halving input latency.
         * The final sleep is split: a coarse tick-sleep for the bulk, then a
         * TSC busy-poll for the sub-4ms remainder, so frames land on a
         * precise sub-millisecond cadence instead of quantising to the
         * 250 Hz tick (4 ms) - this removes the last visible micro-jitter in
         * cursor motion. */
        int interval = moved ? 8 : 16;
        next_frame += interval;
        now = time_ms();
        if(next_frame > now){
            long remain = next_frame - now;
            /* coarse tick-sleep for the bulk, then a TSC busy-poll (inline
             * rdtsc, NO syscall) for the sub-4ms remainder - precise cadence
             * without per-iteration syscall cost. */
            if(remain > 4) sleep(remain - 4);
            if(g_tsc_per_ms){
                u64 dl = tsc_deadline(next_frame);
                while(wm_rdtsc() < dl) __asm__ volatile("pause");
            }
        }
        else if(now - next_frame > 100) next_frame = now;
    }
}
