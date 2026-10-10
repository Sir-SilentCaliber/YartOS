/* Taskbar (black bar, search pill, clock, status cluster). */
#include "wm.h"

tray_t G_tray[8];
int    G_tray_n;
int    G_act_x0, G_act_x1, G_clock_x0, G_clock_x1, G_sys_x0, G_sys_x1;
int    G_ws_x0, G_ws_x1;

void format_wallclock(long wt, char *buf) {
    long s=wt%100; wt/=100; long m=wt%100; wt/=100; long h=wt%100;
    (void)s;
    buf[0]='0'+h/10;buf[1]='0'+h%10;buf[2]=':';buf[3]='0'+m/10;buf[4]='0'+m%10;buf[5]=0;
}
void format_date(long wt,char *buf){
    long day_secs = wt / 1000000ULL;
    int year  = (int)(day_secs / 10000);
    int month = (int)((day_secs / 100) % 100);
    int day   = (int)(day_secs % 100);
    static const char *mn[]={"","Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    int k=0;
    if(month>=1&&month<=12){
        for(const char*q=mn[month];*q;q++)buf[k++]=*q;
    }
    buf[k++]='.'; buf[k++]=' ';
    if(day>=10){buf[k++]='0'+day/10;buf[k++]='0'+day%10;} else buf[k++]='0'+day;
    buf[k++]=' ';                      /* Skift: "Aug. 14 2026" (no comma) */
    buf[k++]='0'+year/1000; buf[k++]='0'+(year/100)%10;
    buf[k++]='0'+(year/10)%10; buf[k++]='0'+year%10;
    buf[k]=0;
}

void draw_panel(surface_t*s){
    /* Skift taskbar: translucent GRAY950 over the wallpaper (no search zone),
     * centred date+time button, status icons (wifi / volume / battery / %)
     * at the top-right. */
    sf_fill_rect_blend(s,0,0,s->w,PANEL_H,theme(T_PANEL_BG));
    int sc = G_scale;                      /* HiDPI UI scale (1 or 2) */
    int ah = PANEL_H - 8*sc, ay = 4*sc;
    int tc  = PANEL_H/2 - 9*sc;            /* text top (2x glyphs)      */
    int icy = PANEL_H/2;                   /* icon centre y             */

    /* ---- centre: "Aug. 14 2026, 12:32" (calendar button) ---- */
    char datetime[80]; int dk=0;
    for(const char*q=G_date;*q;q++) datetime[dk++]=*q;
    datetime[dk++]=','; datetime[dk++]=' ';
    for(const char*q=G_clk;*q;q++) datetime[dk++]=*q;
    datetime[dk]=0;
    int cw=sf_text_width(datetime);
    int cx=s->w/2-cw/2;
    bool chov = ptin(G_cx,G_cy,cx-8*sc,0,cx+cw+8*sc,PANEL_H);
    if(chov || G_calendar) sf_round_rect_blend(s,cx-8*sc,ay,cw+16*sc,ah,ah/2,theme(T_MENU_HOVER));
    sf_text(s,cx,tc,datetime,theme(T_TEXT));
    G_clock_x0=cx-8*sc; G_clock_x1=cx+cw+8*sc;

    /* ---- top-right: wifi, volume, battery + percentage (Skift order) ---- */
    int binfo[3]; battery(binfo);
    int bpresent = binfo[0], bcharging = binfo[1], blevel = binfo[2];
    bool bunknown = (blevel < 0);
    if(blevel < 0) blevel = 0;
    if(blevel > 100) blevel = 100;
    int rx=s->w-12*sc;
    /* battery label: Skift statusbar style — icon + "NN%".  With no battery
     * (wall power) show "AC" (what real desktops show on mains). */
    char btxt[8];
    if(!bpresent){ btxt[0]='A'; btxt[1]='C'; btxt[2]=0; }
    else if(bunknown){ btxt[0]='?'; btxt[1]='%'; btxt[2]=0; }
    else { btxt[0]='0'+blevel/10; btxt[1]='0'+blevel%10; btxt[2]='%'; btxt[3]=0; }
    int pw=sf_text_width(btxt);
    rx-=pw; sf_text(s,rx,tc,btxt,theme(T_TEXT)); rx-=8*sc;
    /* battery colour encodes state: red critical, accent low, white normal */
    u32 bcol = !bpresent ? theme(T_TEXT_DIM)
             : bunknown ? theme(T_TEXT_DIM)
             : blevel<=10 ? theme(T_DANGER)
             : blevel<=20 ? theme(T_ACCENT)
             : theme(T_TEXT);
    /* ink-equalised draw sizes (target ~20px ink height) x HiDPI scale */
    icon_t bat=icon_get(ICON_TRAY_BATTERY); if(bat.px){ sf_icon_sz(s,rx-15*sc,icy,bat,bcol,31*sc); rx-=43*sc; }
    icon_t vol=icon_get(G_audio?ICON_TRAY_AUDIO_HI:ICON_TRAY_AUDIO_MUTE); if(vol.px){ sf_icon_sz(s,rx-10*sc,icy,vol,theme(T_TEXT),20*sc); rx-=30*sc; }
    /* WiFi: show the connected icon ONLY when online; otherwise a ">"
     * chevron button that opens the scanned-network list. */
    int net_icon=!G_net_up?ICON_TRAY_NET_IDLE:(G_wifi?ICON_TRAY_NET_WIFI:ICON_TRAY_NET_WIRED);
    if(G_wifi && G_net_up){
        icon_t net=icon_get(net_icon); if(net.px){ sf_icon_sz(s,rx-18*sc,icy,net,theme(T_TEXT),37*sc); rx-=46*sc; }
    } else {
        icon_t chev=icon_get(ICON_ACT_GO_NEXT);
        bool chov = ptin(G_cx,G_cy,rx-34*sc,0,rx+2,PANEL_H);
        if(chov || G_netlist_open) sf_round_rect_blend(s,rx-34*sc,ay,36*sc,ah,ah/2,theme(T_MENU_HOVER));
        if(chev.px) sf_icon_sz(s,rx-26*sc,icy,chev,theme(T_TEXT_DIM),20*sc);
        G_net_x0=rx-34*sc; G_net_x1=rx+2;
        rx-=38*sc;
    }
    /* clipboard button (opens the clipboard popover) */
    {
        icon_t cp=icon_get(ICON_ACT_EDIT_PASTE);
        bool hov = ptin(G_cx,G_cy,rx-34*sc,0,rx+2,PANEL_H);
        if(hov || G_clip_open) sf_round_rect_blend(s,rx-34*sc,ay,36*sc,ah,ah/2,theme(T_MENU_HOVER));
        if(cp.px) sf_icon_sz(s,rx-26*sc,icy,cp,theme(T_TEXT),20*sc);
        G_clip_x0=rx-34*sc; G_clip_x1=rx+2;   /* hit region = the drawn button */
        rx-=38*sc;
    }
    /* input-language button (honest: only an English layout exists) */
    {
        const char *lb = "EN";
        int lw = sf_text_width(lb);
        bool hov = ptin(G_cx,G_cy,rx-lw-16*sc,0,rx,PANEL_H);
        if(hov) sf_round_rect_blend(s,rx-lw-16*sc,ay,lw+16*sc,ah,ah/2,theme(T_MENU_HOVER));
        sf_text(s,rx-lw-8*sc,tc,lb,theme(T_TEXT));
        G_lang_x0=rx-lw-16*sc; G_lang_x1=rx;
        rx-=lw+20*sc;
    }
    (void)bcharging;
    G_sys_x0=rx; G_sys_x1=s->w-6;
    G_act_x0=G_act_x1=0;      /* no search/activities pill in the bar */
    G_ws_x0=G_ws_x1=0;        /* no workspace dots in the bar           */
    G_tray_n=0;
    G_tray[G_tray_n++]=(typeof(G_tray[0])){G_sys_x0,0,G_sys_x1,PANEL_H,net_icon,"status"};
}

int tray_hit(int x,int y){ for(int i=0;i<G_tray_n;i++) if(ptin(x,y,G_tray[i].x0,G_tray[i].y0,G_tray[i].x1,G_tray[i].y1))return i; return -1; }
