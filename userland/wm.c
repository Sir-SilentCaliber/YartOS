/* YartOS ring-3 compositor — orchestrator module.
 * Screen concerns are split across wm_damage.c, wm_windows.c, wm_dock.c,
 * wm_panel.c, wm_launcher.c, wm_overlays.c (see wm.h).  This file owns the
 * main loop, input routing, cursor, backdrop compositing, config loading,
 * process management and app launching. */
#include "wm.h"

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

int icon_for_path(const char *p){
    if(strcmp(p,"/bin/nyra")==0) return ICON_DOCK_TERMINAL;
    if(strcmp(p,"/bin/files")==0) return ICON_DOCK_FILES;
    if(strcmp(p,"/bin/settings")==0) return ICON_DOCK_SETTINGS;
    if(strcmp(p,"/bin/browser")==0) return ICON_DOCK_BROWSER;
    if(strcmp(p,"/bin/editor")==0) return ICON_DOCK_EDITOR;
    return ICON_MIME_APPLICATION_X_EXECUTABLE;
}

/* ---------- app, dock and desktop persistence ---------- */
void add_app(const char *name,const char *path,int icon){
    if(G_apps>=MAX_APPS) return;
    for(int i=0;i<G_apps;i++) if(strcmp(G_app[i].path,path)==0) return;
    copy_str(G_app[G_apps].name,name,sizeof(G_app[0].name));
    copy_str(G_app[G_apps].path,path,sizeof(G_app[0].path));
    G_app[G_apps].icon=icon; G_app[G_apps].removable=(path[0]=='/'); G_apps++;
}
int app_index(const char *path){ for(int i=0;i<G_apps;i++) if(strcmp(G_app[i].path,path)==0) return i; return -1; }

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
    load_apps_config(); default_dock(); dock_apply_hidden();
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
    if(button==3){
        if(G_menu){menu_close();return;}
        if(G_grid){ int ai; if(app_grid_hit(x,y,&ai)){ if(ai>=0)menu_open(x,y,2,ai); return; } }
        /* right-click a window titlebar -> Skift window menu (Restore /
         * Maximize / Minimize / Snap Left / Snap Right / Close) */
        win_t *rw=win_at(x,y);
        if(rw){ int rty=rw->y-TB_H; if(rty<PANEL_H)rty=PANEL_H;
            if(ptin(x,y,rw->x,rty,rw->x+rw->w,rty+TB_H)){ menu_open_win(x,y,rw); return; } }
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
    if(w){ int ty=w->y-TB_H; if(ty<PANEL_H)ty=PANEL_H; bring_front(w); wm_focus(w->owner); G_focus_win=(int)(w-G_win);
        if(ptin(x,y,w->x,ty,w->x+w->w,ty+TB_H)){
            /* [min] [max] [close] buttons on the right (Skift style) */
            int bx=w->x+w->w-TB_H+4, by=ty+4, bs=TB_H-8;
            if(ptin(x,y,bx,by,bx+bs,by+bs)){ close_win(w); return; }         /* close */
            bx-=bs+2;
            if(ptin(x,y,bx,by,bx+bs,by+bs)){ toggle_max(w); return; }        /* maximize */
            bx-=bs+2;
            if(ptin(x,y,bx,by,bx+bs,by+bs)){ minimize_win(w); return; }      /* minimize */
            if(!w->maximized){
                long now=time_ms();
                if(now-G_last_title_click<320){ toggle_max(w); G_last_title_click=0; return; }
                G_last_title_click=now;
                G_title_drag=true;G_drag_win=(int)(w-G_win);G_drag_dx=x-w->x;G_drag_dy=(y+TB_H)-w->y;
            } return;
        }
        if(!w->maximized){
            int onl=x>=w->x-4&&x<=w->x+6, onr=x<=w->x+w->w+4&&x>=w->x+w->w-6;
            int onb=y<=ty+TB_H+w->h+4&&y>=ty+TB_H+w->h-6;
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
    if(G_quick){ int bx=G_quick_x+16, slx=bx+26, slw=G_quick_w-58;
        if(G_mb&1 && x>=slx-4 && x<=slx+slw+4){ int v=(x-slx)*100/slw; if(v<0)v=0; if(v>100)v=100; G_vol=v; G_audio=v>0; audio_vol(v); damage_add((GfxRect){G_quick_x-4,G_quick_y-4,G_quick_w+8,G_quick_h+8}); return; } }
    if(G_resize_win>=0&&G_resize_win<MAX_WIN&&G_win[G_resize_win].active){ win_t*w=&G_win[G_resize_win];
        int nx=w->x, ny=w->y, nw=w->w, nh=w->h;
        if(G_resize_edges&1){ int dx=x-w->x; if(w->w-dx>=240){nx=x;nw=w->w-dx;} }
        if(G_resize_edges&2){ nw=x-w->x; if(nw<240)nw=240; }
        if(G_resize_edges&4){ int dh=w->y-y; if(w->h+dh>=120){ ny=y; nh=w->h+dh; } }
        if(G_resize_edges&8){ nh=y-(w->y-TB_H)-TB_H; if(nh<120)nh=120; }
        if(nx<0){nx=0;} if(nx+nw>G_fb.w){nw=G_fb.w-nx;}
        wm_move(w->id,nx,ny); wm_resize(w->id,nw,nh); w->x=nx;w->y=ny;w->w=nw;w->h=nh;w->dirty=true; }
    else if(G_title_drag&&G_drag_win>=0&&G_drag_win<MAX_WIN&&G_win[G_drag_win].active){
        /* drag slop: ignore tiny movements so clicks don't jiggle the window */
        if(absi(x-G_pressed_x)+absi(y-G_pressed_y) < 5) return;
        win_t*w=&G_win[G_drag_win]; int nx=x-G_drag_dx,ny=maxi(PANEL_H+24,y-G_drag_dy); if(nx<20)nx=20; if(nx>G_fb.w-60)nx=G_fb.w-60; wm_move(w->id,nx,ny); w->x=nx;w->y=ny;w->dirty=true; }
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
    if(G_desktop_drag){ if(!G_marquee&&G_sel_desk<0){} G_desktop_drag=false; }
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
    if(G_dockmenu){ if(ascii>=32&&ascii<127&&G_search_len<(int)sizeof(G_search)-1){G_search[G_search_len++]=(char)ascii;G_search[G_search_len]=0;} else if(ascii==8||ascii==127){if(G_search_len>0){G_search_len--;G_search[G_search_len]=0;}} else if(ascii==27){G_dockmenu=false;G_search_len=0;G_search[0]=0;} else if(ascii==13||ascii==10){ for(int i=0;i<G_apps;i++) if(search_match(G_app[i].name)){launch_app(G_app[i].path);G_dockmenu=false;G_search_len=0;G_search[0]=0;break;} } return; }
    if(G_grid){ if(ascii>=32&&ascii<127&&G_search_len<(int)sizeof(G_search)-1){G_search[G_search_len++]=(char)ascii;G_search[G_search_len]=0;} else if(ascii==8||ascii==127){if(G_search_len>0){G_search_len--;G_search[G_search_len]=0;}} else if(ascii==27){G_grid=false;G_search_len=0;G_search[0]=0;} else if(ascii==13||ascii==10){ for(int i=0;i<G_apps;i++) if(search_match(G_app[i].name)){launch_app(G_app[i].path);G_grid=false;break;} } return; }
    if(ascii==27){ if(G_switcher){ if(G_switcher_idx>=0&&G_switcher_idx<MAX_WIN){win_t*w=&G_win[G_switcher_idx]; if(w->active){bring_front(w); wm_focus(w->owner); G_focus_win=G_switcher_idx; if(w->minimized)restore_win(w);}} G_switcher=false; g_backdrop_dirty=true; damage_whole(); return; } close_all_overlays();return;}
    /* Super (Meta) opens the launcher — Skift: Super+Space / search pill */
    if((ev&WM_KEY_EXT) && (sc==0x5B || sc==0x5C)){ G_grid=!G_grid; if(G_grid){G_search_len=0; G_search[0]=0;} return; }
    if(sc==0x3B||sc==0x3F){ G_grid=!G_grid; if(G_grid){G_search_len=0; G_search[0]=0;} return; }
    if(sc==0x3C||sc==0x3D){ show_desktop_toggle(); }
    if(sc==0x3E){ theme_reset_defaults(); if(theme_load("/home/yart/theme.ini")==0) osd("Theme reloaded (F4)"); else osd("Theme reset (F4)"); g_backdrop_dirty=true; damage_whole(); return; }
    /* Workspace switching: Super+1..4 or Ctrl+Alt+Left/Right */
    if((ev&WM_KEY_CTRL) && sc>=0x03 && sc<=0x06){ int ws=sc-0x03; if(ws<MAX_WORKSPACES && ws!=G_workspace){ G_workspace=ws; char b[32]; int k=0; const char*q="Workspace "; while(q[k]){b[k]=q[k];k++;} b[k++]='0'+1+ws; b[k]=0; osd(b); g_backdrop_dirty=true; damage_whole(); wm_focus(0); } return; }
    if((ev&WM_KEY_CTRL) && sc==0x4B){ if(G_workspace>0){G_workspace--; osd("Workspace left"); g_backdrop_dirty=true; damage_whole();} return; }
    if((ev&WM_KEY_CTRL) && sc==0x4D){ if(G_workspace<MAX_WORKSPACES-1){G_workspace++; if(G_workspace>=G_ws_count)G_ws_count=G_workspace+1; char b[32];int k=0;const char*q="Workspace ";while(q[k]){b[k]=q[k];k++;} b[k++]='0'+1+G_workspace; b[k]=0; osd(b); g_backdrop_dirty=true; damage_whole();} return; }
    /* Ctrl+Alt+Up/Down -> Mission-Control-style overview */
    if((ev&WM_KEY_CTRL) && (ev&WM_KEY_ALT) && (sc==0x48 || sc==0x50)){ G_overview=!G_overview; return; }
    /* Alt+Tab window switcher (with live previews) */
    if((ev&WM_KEY_ALT) && sc==0x0F && make){
        if(!G_switcher) G_switcher_idx = -1;      /* first press: start fresh */
        G_switcher=true;
        int start=G_switcher_idx;
        for(int step=0;step<MAX_WIN;step++){ G_switcher_idx=(G_switcher_idx+1)%MAX_WIN;
            win_t*c=&G_win[G_switcher_idx];
            if(c->active&&!c->minimized&&!c->closing&&(c->workspace==G_workspace||c->workspace<0)) break;
        }
        if(start==G_switcher_idx && !(G_win[G_switcher_idx].active&&!G_win[G_switcher_idx].minimized&&!G_win[G_switcher_idx].closing&&(G_win[G_switcher_idx].workspace==G_workspace||G_win[G_switcher_idx].workspace<0))){ G_switcher=false; }
        damage_whole(); return;
    }
}
static int cursor_pick(void){
    int x=G_cx,y=G_cy;
    if(G_menu){ if(menu_hit(x,y)>=-2) return 1; }
    win_t *w=win_at(x,y);
    if(w){
        int ty=w->y-TB_H; if(ty<PANEL_H)ty=PANEL_H;
        if(ptin(x,y,w->x,ty,w->x+w->w,ty+TB_H)) return 1;
        /* resize cursor over the window edges (Skift ResizeCursor) */
        if(!w->maximized){
            int onl=x>=w->x-4&&x<=w->x+6, onr=x<=w->x+w->w+4&&x>=w->x+w->w-6;
            int onb=y<=ty+TB_H+w->h+4&&y>=ty+TB_H+w->h-6;
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
/* Cursor render scale: the photo cursors are packed at 48px; 4/5 draws them
 * at ~38px — a touch smaller, Skift's own vector cursors are 28-32px. */
#define CURSOR_SCALE_NUM 4
#define CURSOR_SCALE_DEN 5
/* Bilinear-downscaled alpha-blend of a cursor image (straight-alpha ARGB). */
static void cursor_blit_raw(surface_t *dst, int dx, int dy, const cursor_img_t *im){
    const int SN = CURSOR_SCALE_NUM, SD = CURSOR_SCALE_DEN;
    int dw = (im->w * SN) / SD; if(dw < 1) dw = 1;
    int dh = (im->h * SN) / SD; if(dh < 1) dh = 1;
    for(int y = 0; y < dh; y++){
        int yy = dy + y; if(yy < 0 || yy >= dst->h) continue;
        int sy = (y * SD * 256) / SN;
        int iy = sy >> 8, fy = sy & 255;
        int iy1 = iy + 1; if(iy1 >= im->h) iy1 = im->h - 1;
        for(int x = 0; x < dw; x++){
            int xx = dx + x; if(xx < 0 || xx >= dst->w || !sf_clip_ok(xx, yy)) continue;
            int sx = (x * SD * 256) / SN;
            int ix = sx >> 8, fx = sx & 255;
            int ix1 = ix + 1; if(ix1 >= im->w) ix1 = im->w - 1;
            u32 c00 = im->px[iy  * im->w + ix];
            u32 c10 = im->px[iy  * im->w + ix1];
            u32 c01 = im->px[iy1 * im->w + ix];
            u32 c11 = im->px[iy1 * im->w + ix1];
            /* premultiplied bilinear (avoids dark fringing on the edges) */
            int a00 = (int)(c00 >> 24), r00 = (int)(u8)(c00 >> 16) * a00, g00 = (int)(u8)(c00 >> 8) * a00, b00 = (int)(u8)c00 * a00;
            int a10 = (int)(c10 >> 24), r10 = (int)(u8)(c10 >> 16) * a10, g10 = (int)(u8)(c10 >> 8) * a10, b10 = (int)(u8)c10 * a10;
            int a01 = (int)(c01 >> 24), r01 = (int)(u8)(c01 >> 16) * a01, g01 = (int)(u8)(c01 >> 8) * a01, b01 = (int)(u8)c01 * a01;
            int a11 = (int)(c11 >> 24), r11 = (int)(u8)(c11 >> 16) * a11, g11 = (int)(u8)(c11 >> 8) * a11, b11 = (int)(u8)c11 * a11;
            int r0 = r00 + (((r10 - r00) * fx) >> 8);
            int g0 = g00 + (((g10 - g00) * fx) >> 8);
            int b0 = b00 + (((b10 - b00) * fx) >> 8);
            int al0 = a00 + (((a10 - a00) * fx) >> 8);
            int r1 = r01 + (((r11 - r01) * fx) >> 8);
            int g1 = g01 + (((g11 - g01) * fx) >> 8);
            int b1 = b01 + (((b11 - b01) * fx) >> 8);
            int al1 = a01 + (((a11 - a01) * fx) >> 8);
            int pr = r0 + (((r1 - r0) * fy) >> 8);
            int pg = g0 + (((g1 - g0) * fy) >> 8);
            int pb = b0 + (((b1 - b0) * fy) >> 8);
            int pa = al0 + (((al1 - al0) * fy) >> 8);   /* straight alpha (0..255) */
            if(pa <= 0) continue;
            int A = pa; if(A > 255) A = 255;
            if(A == 0) continue;
            int R = (pr * 255 + pa / 2) / pa;
            int G = (pg * 255 + pa / 2) / pa;
            int B = (pb * 255 + pa / 2) / pa;
            if(R > 255) R = 255;
            if(G > 255) G = 255;
            if(B > 255) B = 255;
            u32 bg = dst->px[yy * dst->pitch + xx];
            u8 br = (u8)bg, bg2 = (u8)(bg >> 8), bb = (u8)(bg >> 16);
            if(A == 255) dst->px[yy * dst->pitch + xx] = ARGB(255, R, G, B);
            else {
                u8 rr = (u8)((R * A + br * (255 - A)) / 255);
                u8 gg = (u8)((G * A + bg2 * (255 - A)) / 255);
                u8 bb2 = (u8)((B * A + bb * (255 - A)) / 255);
                dst->px[yy * dst->pitch + xx] = 0xFF000000u | (bb2 << 16) | (gg << 8) | rr;
            }
        }
    }
}
static void cursor_damage_prev(void){
    if(G_cursor_last_w > 0) damage_add((GfxRect){G_cursor_last_x, G_cursor_last_y, G_cursor_last_w, G_cursor_last_h});
}

/* Footprint of the cursor at its current position/kind.  Used to damage the
 * cursor region BEFORE the compositing pass so the cursor is presented in
 * the SAME frame it moved to (Skift draws the cursor in the render pass;
 * damaging it only for the next frame made it trail 16ms + ghost). */
static GfxRect cursor_rect(void){
    if(G_cur_theme<0){ G_cur_theme=cursors_theme_by_name("roblox"); if(G_cur_theme<0) G_cur_theme=0; }
    int kind=cursor_pick();
    cursor_theme_t *t=cursors_theme(G_cur_theme);
    int dx=G_cx,dy=G_cy,cw=24,ch=24;
    int k=kind;
    if(t && !t->img[k].present && k==2) k=0;
    if(t && t->img[k].present){
        cursor_img_t *im=&t->img[k];
        cw = (im->w  * CURSOR_SCALE_NUM) / CURSOR_SCALE_DEN;
        ch = (im->h  * CURSOR_SCALE_NUM) / CURSOR_SCALE_DEN;
        dx = G_cx - (im->hotx * CURSOR_SCALE_NUM) / CURSOR_SCALE_DEN;
        dy = G_cy - (im->hoty * CURSOR_SCALE_NUM) / CURSOR_SCALE_DEN;
    } else {
        cw=28; ch=28; dx=G_cx-14; dy=G_cy-14;
    }
    return (GfxRect){dx,dy,cw,ch};
}

static void cursor_draw(surface_t*s){
    if(G_cur_theme<0){ G_cur_theme=cursors_theme_by_name("roblox"); if(G_cur_theme<0) G_cur_theme=0; }
    int kind=cursor_pick();
    cursor_theme_t *t=cursors_theme(G_cur_theme);
    int dx=G_cx,dy=G_cy,cw=24,ch=24;
    /* resize falls back to the arrow when the theme has no resize cursor */
    int k = kind;
    if(t && !t->img[k].present && k==2) k=0;
    if(t && t->img[k].present){
        cursor_img_t *im=&t->img[k];
        cw = (im->w  * CURSOR_SCALE_NUM) / CURSOR_SCALE_DEN;
        ch = (im->h  * CURSOR_SCALE_NUM) / CURSOR_SCALE_DEN;
        dx = G_cx - (im->hotx * CURSOR_SCALE_NUM) / CURSOR_SCALE_DEN;
        dy = G_cy - (im->hoty * CURSOR_SCALE_NUM) / CURSOR_SCALE_DEN;
        cursor_blit_raw(s,dx,dy,im);
    } else {
        icon_t ic=icon_get(k==1?ICON_CURSOR_HAND:ICON_CURSOR_ARROW);
        sf_icon_scaled(s,G_cx,G_cy,ic,0,24,48);
        cw=28; ch=28; dx=G_cx-14; dy=G_cy-14;
    }
    G_cursor_last_x=dx; G_cursor_last_y=dy; G_cursor_last_w=cw; G_cursor_last_h=ch;
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
    /* overlays, topmost (gated by rect where bounds are known) */
    if(G_quick && rect_colide(r,(GfxRect){G_quick_x,G_quick_y,G_quick_w,G_quick_h})) draw_quick(&G_fb);
    if(G_calendar && rect_colide(r,(GfxRect){G_cal_x,G_cal_y,G_cal_w,G_cal_h})) draw_calendar(&G_fb);
    if(G_clip_open && rect_colide(r,(GfxRect){G_clip_x,G_clip_y,G_clip_w,G_clip_h})) draw_clipboard(&G_fb);
    if(G_netlist_open && rect_colide(r,(GfxRect){G_nl_x,G_nl_y,G_nl_w,G_nl_h})) draw_netlist(&G_fb);
    if(G_dockmenu && rect_colide(r,(GfxRect){G_dockmenu_x,G_dockmenu_y,G_dockmenu_w,G_dockmenu_h})) draw_dockmenu(&G_fb);
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
    fb_info_t fi; void*fb=fb_info(&fi); if(!fb)return;
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
    bool first_frame=true;
    int prev_overlay_state=-1;
    bool osd_was_visible=false;
    static bool s_overlay_focus=false;
    long next_frame=time_ms();
    for(;;){
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
        pid_forget_dead();
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

        /* ---- dock animation/hover ---- */
        dock_update(now);

        /* ---- window animation + damage ---- */
        windows_update_and_damage(now);

        /* ---- overlay open/close: full damage + keyboard focus routing ----
         * While any overlay is up, the wm owns the keyboard (search typing,
         * Esc); when the last overlay closes, focus returns to the top
         * window so apps get their typing back. */
        int overlay_state = G_grid | (G_overview<<1) | (G_switcher<<2) |
                           (G_quick<<3) | (G_calendar<<4) |
                           (G_dockmenu<<5) | (G_menu<<6);
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

        /* ---- fixed 60 Hz frame pacing (Skift: sleepAsync(lastFrame+16ms)) ---- */
        next_frame += 16;
        now = time_ms();
        if(next_frame > now) sleep(next_frame - now);
        else if(now - next_frame > 100) next_frame = now;
    }
}
