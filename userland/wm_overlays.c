/* Context menu, quick settings, calendar, OSD, switcher, overview. */
#include "wm.h"

/* Lock screen (Super+L): dims the desktop behind a frosted GRAY950 veil and
 * shows the clock + date + an unlock hint.  Enter unlocks. */
void draw_lock(surface_t*s, long now){
    (void)now;
    /* veil: dark, slightly translucent so the wallpaper ghosts through */
    sf_fill_rect_blend(s,0,0,s->w,s->h,ARGB(225,9,9,11));
    int cx=s->w/2;
    int cy=s->h/2-30;
    icon_t lk=icon_get(ICON_TRAY_LOCK);
    if(lk.px) sf_icon_scaled(s,cx,cy-92,lk,theme(T_ACCENT),30,48);
    /* time */
    char timebuf[16];
    copy_str(timebuf,G_clk,sizeof timebuf);
    if(!timebuf[0]){ copy_str(timebuf,"12:00",sizeof timebuf); }
    int tw=sf_text_width(timebuf);
    /* draw the time twice, offset, to fake a soft shadow */
    sf_text(s,cx-tw/2+1,cy-20+1,timebuf,ARGB(90,0,0,0));
    sf_text(s,cx-tw/2,cy-20,timebuf,theme(T_TEXT));
    /* date */
    int dw=sf_text_width(G_date);
    sf_text(s,cx-dw/2,cy+10,G_date,theme(T_TEXT_DIM));
    /* unlock hint / password field */
    if(G_lock_prompt){
        int pw_w = 220, pw_h = 34;
        sf_round_rect_blend(s,cx-pw_w/2-1,cy+46-1,pw_w+2,pw_h+2,17,theme(T_WIN_BORDER));
        sf_round_rect(s,cx-pw_w/2,cy+46,pw_w,pw_h,16,theme(T_SEARCH_BG));
        /* password as asterisks (font only covers ASCII) */
        char dots[64]; int k=0;
        while(k < G_lock_pw_len && k < 50){ dots[k] = '*'; k++; }
        dots[k]=0;
        if(G_lock_pw_len>0){
            sf_text(s,cx-pw_w/2+14,cy+46+(pw_h-18)/2,dots,theme(T_TEXT));
        } else {
            sf_text(s,cx-pw_w/2+14,cy+46+(pw_h-18)/2,"Enter password",theme(T_TEXT_FAINT));
        }
        if(G_lock_bad){
            const char*w="Wrong password";
            int ww=sf_text_width(w);
            sf_text(s,cx-ww/2,cy+46+pw_h+10,w,theme(T_DANGER));
        }
    } else {
        const char*hint="Press Enter to log in";
        int hw=sf_text_width(hint);
        sf_round_rect_blend(s,cx-hw/2-14,cy+46-9,hw+28,30,15,ARGB(40,255,255,255));
        sf_text(s,cx-hw/2,cy+46,hint,theme(T_TEXT_DIM));
    }
}

void menu_open(int x,int y,int type,int idx){
    G_menu=true; G_menu_t0=time_ms(); G_menu_type=type; G_menu_idx=idx; G_menu_arg=idx; G_menu_n=0; G_menu_path[0]=0; G_menu_w=190; G_menu_h=8;
    if(type==1){ // dock
        if(idx<0||idx>=G_dock_n)return;
        copy_str(G_menu_path,G_dock[idx].path,sizeof(G_menu_path));
        if(G_dock[idx].path[0] && strcmp(G_dock[idx].path,"trash")!=0) G_menu_items[G_menu_n++]=(menuitem_t){"Open",ACT_DOCK_LAUNCH,idx,ICON_DOCK_LAUNCHER};
        if(!G_dock[idx].core && G_dock[idx].path[0]=='/') G_menu_items[G_menu_n++]=(menuitem_t){"Unpin from Dock",ACT_UNPIN_DOCK,idx,ICON_PLACE_DESKTOP};
        if(G_dock[idx].path[0]=='/' && app_index(G_dock[idx].path)>=0) G_menu_items[G_menu_n++]=(menuitem_t){"Add to Desktop",ACT_ADD_DESKTOP,idx,ICON_PLACE_DESKTOP};
    } else if(type==2){ // app grid
        if(idx<0||idx>=G_apps)return;
        copy_str(G_menu_path,G_app[idx].path,sizeof(G_menu_path));
        G_menu_items[G_menu_n++]=(menuitem_t){"Open",ACT_LAUNCH_APP,idx,ICON_DOCK_LAUNCHER};
        if(dock_find_path(G_app[idx].path)<0) G_menu_items[G_menu_n++]=(menuitem_t){"Pin to Dock",ACT_PIN_APP,idx,ICON_PLACE_DOCS};
        G_menu_items[G_menu_n++]=(menuitem_t){"Add to Desktop",ACT_ADD_DESKTOP,idx,ICON_PLACE_DESKTOP};
    } else if(type==3){ // desktop icon
        if(idx>=0&&idx<G_desk_n){ copy_str(G_menu_path,G_desk[idx].path,sizeof(G_menu_path)); G_menu_items[G_menu_n++]=(menuitem_t){"Open",ACT_OPEN_DESKTOP,idx,ICON_DOCK_LAUNCHER}; G_menu_items[G_menu_n++]=(menuitem_t){"Remove from Desktop",ACT_REMOVE_DESKTOP,idx,ICON_DOCK_TRASH}; }
        G_menu_items[G_menu_n++]=(menuitem_t){"Personalize...",ACT_CHANGE_WALLPAPER,0,ICON_PLACE_PICS};
        G_menu_items[G_menu_n++]=(menuitem_t){"Settings",ACT_OPEN_SETTINGS,0,ICON_DOCK_SETTINGS};
    } else if(type==4){ // empty desktop (Skift: Personalize... + Settings)
        G_menu_items[G_menu_n++]=(menuitem_t){"Personalize...",ACT_CHANGE_WALLPAPER,0,ICON_PLACE_PICS};
        G_menu_items[G_menu_n++]=(menuitem_t){"Settings",ACT_OPEN_SETTINGS,0,ICON_DOCK_SETTINGS};
    } else if(type==5){ // window titlebar (Skift TitlebarContent menu)
        if(idx<0||idx>=MAX_WIN||!G_win[idx].active) return;
        G_menu_items[G_menu_n++]=(menuitem_t){"Restore",ACT_RESTORE_WIN,idx,ICON_WIN_RESTORE};
        G_menu_items[G_menu_n++]=(menuitem_t){"Maximize",ACT_TOGGLE_MAX,idx,ICON_WIN_MAX};
        G_menu_items[G_menu_n++]=(menuitem_t){"Minimize",ACT_MIN_WIN,idx,ICON_WIN_MIN};
        G_menu_items[G_menu_n++]=(menuitem_t){"Snap Left",ACT_SNAP_LEFT,idx,ICON_WIN_RESTORE};
        G_menu_items[G_menu_n++]=(menuitem_t){"Snap Right",ACT_SNAP_RIGHT,idx,ICON_WIN_RESTORE};
        G_menu_items[G_menu_n++]=(menuitem_t){"Close",ACT_CLOSE_WIN,idx,ICON_WIN_CLOSE};
    }
    for(int i=0;i<G_menu_n;i++){ int tw=sf_text_width(G_menu_items[i].label); if(tw+52>G_menu_w)G_menu_w=tw+52; }
    G_menu_h=G_menu_n*24+8; G_menu_x=mini(maxi(x,4),G_fb.w-G_menu_w-4); G_menu_y=mini(maxi(y,4),G_fb.h-G_menu_h-4);
    if(G_menu_n==0)G_menu=false;
}

void menu_close(void){ G_menu=false; G_menu_n=0; }

int menu_hit(int x,int y){ if(!G_menu||!ptin(x,y,G_menu_x,G_menu_y,G_menu_x+G_menu_w,G_menu_y+G_menu_h))return -1; for(int i=0;i<G_menu_n;i++) if(ptin(x,y,G_menu_x+4,G_menu_y+4+i*24,G_menu_x+G_menu_w-4,G_menu_y+28+i*24)) return i; return -2; }

void draw_menu(surface_t*s){
    if(!G_menu)return;
    sf_round_rect_blend(s,G_menu_x-2,G_menu_y-2,G_menu_w+4,G_menu_h+4,10,ARGB(110,0,0,0));
    sf_round_rect_blend(s,G_menu_x-1,G_menu_y-1,G_menu_w+2,G_menu_h+2,9,theme(T_WIN_BORDER));
    sf_round_rect(s,G_menu_x,G_menu_y,G_menu_w,G_menu_h,8,theme(T_MENU_BG));
    for(int i=0;i<G_menu_n;i++){
        int mx=G_cx,my=G_cy; if(ptin(mx,my,G_menu_x+4,G_menu_y+4+i*24,G_menu_x+G_menu_w-4,G_menu_y+28+i*24)) sf_round_rect_blend(s,G_menu_x+4,G_menu_y+4+i*24,G_menu_w-8,24,6,theme(T_MENU_HOVER));
        if(G_menu_items[i].icon>=0){ icon_t ic=icon_get(G_menu_items[i].icon); if(ic.px) sf_icon_scaled(s,G_menu_x+16,G_menu_y+16+i*24,ic,theme(T_TEXT_DIM),14,22); }
        sf_text(s,G_menu_x+38,G_menu_y+9+i*24,G_menu_items[i].label,theme(T_TEXT));
    }
}

void menu_dispatch(int i){
    if(i<0||i>=G_menu_n)return;
    menuitem_t mi=G_menu_items[i];
    if(mi.action==ACT_LAUNCH_APP){ launch_app(G_app[mi.arg].path); G_grid=false; }
    else if(mi.action==ACT_DOCK_LAUNCH){ if(mi.arg>=0&&mi.arg<G_dock_n) launch_app(G_dock[mi.arg].path); }
    else if(mi.action==ACT_PIN_APP){ dock_save_hidden(G_app[mi.arg].path,false); add_dock_app(G_app[mi.arg].path,G_app[mi.arg].name,G_app[mi.arg].icon,true); save_dock_only(); save_config(); osd("Pinned to Dock"); }
    else if(mi.action==ACT_UNPIN_DOCK){ char path[72]; if(mi.arg>=0&&mi.arg<G_dock_n)copy_str(path,G_dock[mi.arg].path,sizeof(path)); remove_dock(mi.arg); dock_save_hidden(path,true); save_dock_only(); osd("Unpinned"); }
    else if(mi.action==ACT_ADD_DESKTOP){
        const char *nm=0,*pt=0; int ic=ICON_MIME_APPLICATION_X_EXECUTABLE;
        if(G_menu_type==1&&G_menu_idx>=0&&G_menu_idx<G_dock_n){nm=G_dock[G_menu_idx].name;pt=G_dock[G_menu_idx].path;ic=G_dock[G_menu_idx].icon;}
        else if(G_menu_type==2&&G_menu_idx>=0&&G_menu_idx<G_apps){nm=G_app[G_menu_idx].name;pt=G_app[G_menu_idx].path;ic=G_app[G_menu_idx].icon;}
        if(nm&&pt){ add_desktop(nm,pt,ic,100); save_config(); osd("Added to Desktop"); }
    }
    else if(mi.action==ACT_REMOVE_DESKTOP){ if(G_menu_arg>=0&&G_menu_arg<G_desk_n){ for(int j=G_menu_arg;j<G_desk_n-1;j++)G_desk[j]=G_desk[j+1]; G_desk_n--; save_config(); osd("Removed from Desktop"); } }
    else if(mi.action==ACT_OPEN_DESKTOP){ if(G_menu_arg>=0&&G_menu_arg<G_desk_n){ if(G_desk[G_menu_arg].kind==4) launch_app("trash"); else if(G_desk[G_menu_arg].kind<=3) launch_app("/bin/files"); else launch_app(G_desk[G_menu_arg].path); } }
    else if(mi.action==ACT_OPEN_TERMINAL) launch_app("/bin/nyra");
    else if(mi.action==ACT_OPEN_SETTINGS) launch_app("/bin/settings");
    else if(mi.action==ACT_CHANGE_WALLPAPER){ int n=wallpaper_count(); if(n>0){G_wp_index=(G_wp_index+1)%n; wallpaper_load_index(G_wp_index); wallpaper_bind(&G_wp); g_backdrop_dirty=true; damage_whole(); osd("Wallpaper changed");} }
    else if(mi.action==ACT_SHOW_DESKTOP){ menu_close(); show_desktop_toggle(); }
    else if(mi.action==ACT_MIN_WIN){ if(mi.arg>=0&&mi.arg<MAX_WIN) minimize_win(&G_win[mi.arg]); }
    else if(mi.action==ACT_RESTORE_WIN){ if(mi.arg>=0&&mi.arg<MAX_WIN){ win_t*w=&G_win[mi.arg]; if(w->active&&w->maximized) toggle_max(w); if(w->active) restore_win(w); } }
    else if(mi.action==ACT_TOGGLE_MAX){ if(mi.arg>=0&&mi.arg<MAX_WIN) toggle_max(&G_win[mi.arg]); }
    else if(mi.action==ACT_SNAP_LEFT){ if(mi.arg>=0&&mi.arg<MAX_WIN) win_snap(&G_win[mi.arg],1); }
    else if(mi.action==ACT_SNAP_RIGHT){ if(mi.arg>=0&&mi.arg<MAX_WIN) win_snap(&G_win[mi.arg],2); }
    else if(mi.action==ACT_CLOSE_WIN){ if(mi.arg>=0&&mi.arg<MAX_WIN) close_win(&G_win[mi.arg]); }
    menu_close();
}

void menu_open_win(int x,int y, win_t *w){
    if(!w) return;
    menu_open(x,y,5,(int)(w-G_win));
}

void draw_quick(surface_t*s){
    int w=320,h=180,x=G_fb.w-w-12,y=PANEL_H+6; G_quick_x=x;G_quick_y=y;G_quick_w=w;G_quick_h=h;
    win_shadow(x,y,w,h);
    sf_round_rect_blend(s,x-1,y-1,w+2,h+2,13,theme(T_WIN_BORDER));
    sf_round_rect(s,x,y,w,h,12,theme(T_OVERLAY_SURFACE));
    int bx=x+16,by=y+16;
    icon_t iwifi=icon_get(ICON_TRAY_NET_WIFI);
    icon_t iwire=icon_get(ICON_TRAY_NET_WIRED);
    /* Wi-Fi + Wired toggle tiles */
    sf_round_rect_blend(s,bx,by,140,44,10,G_wifi?theme(T_BTN_TOGGLE_ON):theme(T_BTN_TOGGLE_OFF));
    if(iwifi.px) sf_icon_scaled(s,bx+18,by+22,iwifi,G_wifi?theme(T_TEXT_ON_ACCENT):theme(T_TEXT_DIM),16,24);
    sf_text(s,bx+38,by+15,"Wi-Fi",G_wifi?theme(T_TEXT_ON_ACCENT):theme(T_TEXT_DIM));
    int cx2=bx+152;
    sf_round_rect_blend(s,cx2,by,140,44,10,G_wired?theme(T_BTN_TOGGLE_ON):theme(T_BTN_TOGGLE_OFF));
    if(iwire.px) sf_icon_scaled(s,cx2+18,by+22,iwire,G_wired?theme(T_TEXT_ON_ACCENT):theme(T_TEXT_DIM),16,24);
    sf_text(s,cx2+38,by+15,"Wired",G_wired?theme(T_TEXT_ON_ACCENT):theme(T_TEXT_DIM));
    /* Audio slider */
    int ay=by+64;
    icon_t ispk=icon_get(G_audio?ICON_TRAY_AUDIO_HI:ICON_TRAY_AUDIO_MUTE);
    if(ispk.px) sf_icon_scaled(s,bx+2,ay+8,ispk,theme(T_TEXT),16,24);
    sf_text(s,bx+26,ay,"Volume",theme(T_TEXT_DIM));
    int slx=bx+26,sly=ay+22,slw=w-58;
    sf_round_rect_blend(s,slx,sly,slw,6,3,ARGB(70,255,255,255));
    int fillw=(int)(slw*(G_vol/100.0f)); if(fillw<0)fillw=0; if(fillw>slw)fillw=slw;
    sf_round_rect(s,slx,sly,fillw,6,3,theme(T_ACCENT));
    sf_round_rect(s,slx+fillw-5,sly-2,10,10,5,RGB(0xFF,0xFF,0xFF));
}

/* ---- clipboard popover (top-right, under the panel) ---- */
void draw_clipboard(surface_t*s){
    int w=300,h=150,x=G_fb.w-w-12,y=PANEL_H+6;
    G_clip_x=x; G_clip_y=y; G_clip_w=w; G_clip_h=h;
    win_shadow(x,y,w,h);
    sf_round_rect_blend(s,x-1,y-1,w+2,h+2,13,theme(T_WIN_BORDER));
    sf_round_rect(s,x,y,w,h,12,theme(T_OVERLAY_SURFACE));
    sf_text(s,x+16,y+12,"Clipboard",theme(T_TEXT));
    if(G_clipboard_len<=0){
        sf_text(s,x+16,y+44,"(empty) — select text in the Console to copy",theme(T_TEXT_DIM));
    } else {
        char line[64];
        int lo=0, r0=0, cy=y+44;
        for(int i=0;i<G_clipboard_len && cy<y+h-12;i++){
            line[r0++]=G_clipboard[i];
            if(G_clipboard[i]=='\n' || r0>=63){ line[r0]=0; sf_text(s,x+16,cy,line,theme(T_TEXT_DIM)); cy+=20; r0=0; lo=i+1; }
            if(lo>0 && i-lo>300) break;
        }
        if(r0>0){ line[r0]=0; sf_text(s,x+16,cy,line,theme(T_TEXT_DIM)); }
    }
}

/* ---- scanned Wi-Fi network list (top-right, under the panel) ---- */
void draw_netlist(surface_t*s){
    int w=320,h=210,x=G_fb.w-w-12,y=PANEL_H+6;
    G_nl_x=x; G_nl_y=y; G_nl_w=w; G_nl_h=h;
    win_shadow(x,y,w,h);
    sf_round_rect_blend(s,x-1,y-1,w+2,h+2,13,theme(T_WIN_BORDER));
    sf_round_rect(s,x,y,w,h,12,theme(T_OVERLAY_SURFACE));
    sf_text(s,x+16,y+12,"Networks",theme(T_TEXT));
    int cy=y+44, shown=0;
    if(G_netlist_len<=0){
        sf_text(s,x+16,cy,"Scanning… / no networks found",theme(T_TEXT_DIM));
    } else {
        /* parse "  SSID [sig ch sec] bssid" lines */
        int i=0;
        while(i<G_netlist_len && shown<7){
            /* find a line that starts with two spaces (an AP entry) */
            int ls=i;
            while(ls<G_netlist_len && (G_netlist[ls]=='\r'||G_netlist[ls]=='\n')) ls++;
            if(ls<G_netlist_len && G_netlist[ls]==' ' && G_netlist[ls+1]==' '){
                int ss=ls+2, se=ss;
                while(se<G_netlist_len && G_netlist[se]!=' ' && G_netlist[se]!='[' && G_netlist[se]!='\n') se++;
                char ssid[40]; int k=0; for(int q=ss;q<se&&k<39;q++) ssid[k++]=G_netlist[q]; ssid[k]=0;
                if(k>0){
                    bool hov=ptin(G_cx,G_cy,x+10,cy-4,x+w-10,cy+20);
                    if(hov) sf_round_rect_blend(s,x+10,cy-4,w-20,22,6,theme(T_MENU_HOVER));
                    icon_t wf=icon_get(ICON_TRAY_NET_WIFI);
                    if(wf.px) sf_icon_scaled(s,x+16,cy+11,wf,theme(T_TEXT),12,18);
                    sf_text(s,x+34,cy,ssid,theme(T_TEXT));
                    cy+=24; shown++;
                }
                i=se;
            }
            while(i<G_netlist_len && G_netlist[i]!='\n') i++;
            if(i<G_netlist_len) i++;
        }
        if(shown==0) sf_text(s,x+16,cy,"No networks found",theme(T_TEXT_DIM));
    }
}

void draw_calendar(surface_t*s){
    int w=300,h=264,x=G_fb.w/2-w/2,y=PANEL_H+6; G_cal_x=x;G_cal_y=y;G_cal_w=w;G_cal_h=h;
    win_shadow(x,y,w,h);
    sf_round_rect_blend(s,x-1,y-1,w+2,h+2,13,theme(T_WIN_BORDER));
    sf_round_rect(s,x,y,w,h,12,theme(T_OVERLAY_SURFACE));
    const char*m[]={"","Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    char title[40]; int k=0; const char*mn=m[G_cal_month]; while(*mn)title[k++]=*mn++; title[k++]=' '; title[k++]='0'+G_cal_year/1000; title[k++]='0'+(G_cal_year/100)%10; title[k++]='0'+(G_cal_year/10)%10; title[k++]='0'+G_cal_year%10; title[k]=0;
    sf_text(s,x+20,y+15,title,theme(T_TEXT)); sf_text(s,x+w-48,y+15,"<  >",theme(T_TEXT_DIM));
    const char*days[]={"Su","Mo","Tu","We","Th","Fr","Sa"}; for(int i=0;i<7;i++)sf_text(s,x+18+i*36,y+52,days[i],theme(T_TEXT_DIM));
    /* real days-in-month */
    int dim=31; int mm=G_cal_month;
    if(mm==4||mm==6||mm==9||mm==11)dim=30;
    else if(mm==2){ dim=((G_cal_year%4==0&&G_cal_year%100)||G_cal_year%400==0)?29:28; }
    /* Zeller's congruence for 1st of month: 0=Sat..6=Fri -> map to Sun=0 */
    { int yy=G_cal_year, m=mm; if(m<3){m+=12;yy--;} int K=yy%100,J=yy/100; int z=(1+13*(m+1)/5+K+K/4+J/4+5*J)%7; int start=(z+1)%7;
    for(int d=1;d<=dim;d++){ int cell=start+d-1,col=cell%7,row=cell/7; char num[8]; int z2=0; if(d<10)num[z2++]=' '; num[z2++]='0'+d%10; if(d>=10)num[0]='0'+d/10; num[z2]=0; int tx=x+18+col*36,ty=y+82+row*26; sf_text(s,tx,ty,num,theme(T_TEXT)); } }
    /* real notification list (apps push via SYS_NOTIFY; the WM collects) */
    if(G_notif_n == 0){
        sf_text(s,x+18,y+h-32,"No notifications",theme(T_TEXT_DIM));
    } else {
        int ny=y+h-32;
        for(int i=G_notif_n-1, c=0; i>=0 && c<2; i--, c++){
            char line[128]; int k=0;
            line[k++]='-'; line[k++]=' ';
            for(int q=0; G_notifs[i][q] && k<120; q++) line[k++]=G_notifs[i][q];
            line[k]=0;
            sf_text(s,x+18,ny,line,theme(T_TEXT_DIM));
            ny+=20;
        }
    }
}

void draw_osd(surface_t*s,long now){ long dt=now-G_osd_t0; if(dt<0||dt>1800)return;
    int tw=sf_text_width(G_osd),w=tw+30,h=32,x=s->w/2-w/2,y=s->h-100;
    sf_round_rect_blend(s,x-3,y-3,w+6,h+6,9,ARGB(80,0,0,0));
    sf_round_rect_blend(s,x-1,y-1,w+2,h+2,9,theme(T_WIN_BORDER));
    sf_round_rect(s,x,y,w,h,8,theme(T_OVERLAY_SURFACE));
    sf_text(s,x+15,y+8,G_osd,theme(T_TEXT)); }

int overview_layout(int *per_row_out){
    int per_row = (G_fb.w-80)/(OV_CW+OV_GAP); if(per_row<1)per_row=1;
    *per_row_out = per_row;
    return per_row;
}

int overview_card_at(int x,int y){
    /* returns window slot index or -1 */
    int per_row; overview_layout(&per_row);
    int n=count_windows_on_ws();
    int row_count = (n + per_row - 1)/per_row;
    int total_w = n<per_row ? n*OV_CW+(n-1)*OV_GAP : per_row*OV_CW+(per_row-1)*OV_GAP;
    int x0 = G_fb.w/2 - total_w/2, y0 = 76;
    int i=0;
    for(int w=0;w<MAX_WIN;w++){ win_t*win=&G_win[w];
        if(!win->active||win->minimized||win->hidden||win->closing) continue;
        if(win->workspace!=G_workspace&&win->workspace>=0) continue;
        int col=i%per_row, row=i/per_row;
        int cx0=x0+col*(OV_CW+OV_GAP), cy0=y0+row*(OV_CH+56);
        if(ptin(x,y,cx0-8,cy0-8,cx0+OV_CW+8,cy0+OV_CH+44)) return w;
        i++;
    }
    (void)row_count;
    return -1;
}

void draw_overview(surface_t*s){
    sf_fill_rect_blend(s,0,0,s->w,s->h,ARGB(215,9,9,11));
    int per_row; overview_layout(&per_row);
    int n=count_windows_on_ws();
    sf_text(s,64,38,"Overview",theme(T_TEXT));
    sf_text(s,64,60,"Click a window to focus  -  Esc or Ctrl+Alt+Down to close",theme(T_TEXT_DIM));
    if(!n){ sf_text(s,64,140,"No open windows on this workspace.",theme(T_TEXT_DIM)); return; }
    int total_w = n<per_row ? n*OV_CW+(n-1)*OV_GAP : per_row*OV_CW+(per_row-1)*OV_GAP;
    int x0 = s->w/2 - total_w/2, y0 = 76, i=0;
    for(int w=0;w<MAX_WIN;w++){ win_t*win=&G_win[w];
        if(!win->active||win->minimized||win->hidden||win->closing) continue;
        if(win->workspace!=G_workspace&&win->workspace>=0) continue;
        int col=i%per_row, row=i/per_row;
        int x=x0+col*(OV_CW+OV_GAP), y=y0+row*(OV_CH+56);
        bool hov=ptin(G_cx,G_cy,x-8,y-8,x+OV_CW+8,y+OV_CH+44);
        win_shadow(x,y,OV_CW,OV_CH);
        sf_round_rect_blend(s,x-1,y-1,OV_CW+2,OV_CH+2,12,(hov?theme(T_ACCENT):theme(T_WIN_BORDER)));
        sf_round_rect(s,x,y,OV_CW,OV_CH,11,RGB(0x0A,0x0A,0x0C));
        surface_t src; src.px=(u32*)(unsigned long)win->va; src.w=win->w; src.h=win->h; src.pitch=win->w;
        if(src.px) sf_blit_scaled(s,x+4,y+4,OV_CW-8,OV_CH-8,&src,0,0,win->w,win->h);
        icon_t ic=icon_get(ICON_DOCK_TERMINAL);
        if(win->path[0]=='/'){ int ai=app_index(win->path); if(ai>=0) ic=icon_get(G_app[ai].icon); }
        if(ic.px) sf_icon_scaled(s,x+16,y+OV_CH+14,ic,0,16,48);
        sf_text(s,x+30,y+OV_CH+8,win->title[0]?win->title:"Window",theme(T_TEXT));
        i++;
    }
}

void draw_switcher(surface_t*s){
    if(!G_switcher) return;
    int n=count_windows_on_ws();

    if(n==0){ G_switcher=false; return; }
    int cw=176, ch=124, gap=12;
    int total = n*cw + (n-1)*gap + 36;
    if(total > s->w-48){ total = s->w-48; cw = (total - 36 - (n-1)*gap)/n; ch = cw*124/176; }
    int bx = s->w/2 - total/2, by = s->h/2 - (ch+48)/2 - 10;
    win_shadow(bx-4,by-4,total+8,ch+56);
    sf_round_rect_blend(s,bx-1,by-1,total+2,ch+54,14,theme(T_WIN_BORDER));
    sf_round_rect(s,bx,by,total,ch+52,12,theme(T_OVERLAY_SURFACE));
    int x=bx+18, shown=0;
    for(int i=0;i<MAX_WIN;i++){ win_t*w=&G_win[i];
        if(!w->active||w->minimized||w->hidden||w->closing) continue;
        if(w->workspace!=G_workspace&&w->workspace>=0) continue;
        bool sel=(i==G_switcher_idx);
        /* live scaled preview of the app surface */
        if(sel) sf_round_rect_blend(s,x-4,by+12-4,cw+8,ch+8,10,theme(T_ACCENT));
        sf_round_rect_blend(s,x-1,by+12-1,cw+2,ch+2,9,theme(T_WIN_BORDER));
        sf_round_rect(s,x,by+12,cw,ch,8,RGB(0x09,0x09,0x0B));
        surface_t src; src.px=(u32*)(unsigned long)w->va; src.w=w->w; src.h=w->h; src.pitch=w->w;
        if(src.px) sf_blit_scaled(s,x+4,by+12+4,cw-8,ch-8,&src,0,0,w->w,w->h);
        /* icon + title below */
        icon_t ic=icon_get(ICON_DOCK_TERMINAL);
        if(w->path[0]=='/'){ int ai=app_index(w->path); if(ai>=0) ic=icon_get(G_app[ai].icon); }
        if(ic.px) sf_icon_scaled(s,x+13,by+12+ch+14,ic,0,14,48);
        sf_text(s,x+26,by+12+ch+9,w->title[0]?w->title:"Window",sel?theme(T_TEXT):theme(T_TEXT_DIM));
        x += cw+gap; shown++;
    }
    (void)shown;
}
