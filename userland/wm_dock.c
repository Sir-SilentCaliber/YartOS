/* Dock + desktop icons (static base scene + frosted-glass dock). */
#include "wm.h"

dock_t    G_dock[MAX_DOCK];
int       G_dock_n;
bool      G_core_state[MAX_DOCK];
desk_t    G_desk[MAX_DESKTOP];
int       G_desk_n;
surface_t G_icon_cache[MAX_DOCK];
int       G_slot_sz[MAX_DOCK], G_slot_lift[MAX_DOCK], G_slot_target_sz[MAX_DOCK], G_slot_target_lift[MAX_DOCK], G_slot_bounce[MAX_DOCK];
int       G_dock_x, G_dock_y, G_dock_w, G_dock_h;
surface_t G_dock_blur;
int       G_blur_x, G_blur_y, G_blur_w, G_blur_h;
int       G_dock_hover = -1;
bool      G_dock_tooltip = false;
bool      G_dock_tooltip_prev = false;

int dock_find_path(const char *path){ for(int i=0;i<G_dock_n;i++) if(strcmp(G_dock[i].path,path)==0) return i; return -1; }

void rebuild_dock_cache(void){
    for(int i=0;i<MAX_DOCK;i++){ if(G_icon_cache[i].px) sf_free(&G_icon_cache[i]); G_icon_cache[i].px=0; G_slot_sz[i]=DOCK_REST; G_slot_lift[i]=G_slot_target_sz[i]=G_slot_target_lift[i]=G_slot_bounce[i]=0; }
    int content_w=G_dock_n*DOCK_PITCH+16*G_scale; G_dock_w=content_w; G_dock_h=68*G_scale;
    G_dock_x=G_fb.w/2-G_dock_w/2; G_dock_y=G_fb.h-G_dock_h-DOCK_MARGIN;
    /* frosted-glass blur region: the backdrop behind the dock pill */
    G_blur_x = G_dock_x - 30; if(G_blur_x < 0) G_blur_x = 0;
    G_blur_y = G_dock_y - 12; if(G_blur_y < 0) G_blur_y = 0;
    G_blur_w = G_dock_w + 60; if(G_blur_x + G_blur_w > G_fb.w) G_blur_w = G_fb.w - G_blur_x;
    G_blur_h = G_dock_h + 30; if(G_blur_y + G_blur_h > G_fb.h) G_blur_h = G_fb.h - G_blur_y;
    g_backdrop_dirty = true;      /* rebuild the blurred dock region */
    for(int i=0;i<G_dock_n;i++){
        icon_t ic=icon_get(G_dock[i].icon);
        if(ic.px){ G_icon_cache[i]=sf_alloc(DOCK_REST,DOCK_REST); if(G_icon_cache[i].px) sf_icon_scaled(&G_icon_cache[i],DOCK_REST/2,DOCK_REST/2,ic,0,DOCK_REST,48); }
    }
}

void add_dock_app(const char *path,const char *name,int icon,bool core){
    if(G_dock_n>=MAX_DOCK||dock_find_path(path)>=0) return;
    copy_str(G_dock[G_dock_n].name,name,sizeof(G_dock[0].name));
    copy_str(G_dock[G_dock_n].path,path,sizeof(G_dock[0].path));
    G_dock[G_dock_n].icon=icon; G_dock[G_dock_n].core=core; G_core_state[G_dock_n]=core; G_dock_n++; rebuild_dock_cache();
}

void remove_dock(int idx){
    if(idx<0||idx>=G_dock_n||G_dock[idx].core) return;
    for(int i=idx;i<G_dock_n-1;i++){ G_dock[i]=G_dock[i+1]; G_core_state[i]=G_core_state[i+1]; } G_dock_n--; rebuild_dock_cache();
}

void add_desktop_xy(const char *name,const char *path,int icon,int kind,int gx,int gy){
    if(G_desk_n>=MAX_DESKTOP) return;
    for(int i=0;i<G_desk_n;i++) if(G_desk[i].kind==kind&&strcmp(G_desk[i].path,path)==0) return;
    copy_str(G_desk[G_desk_n].name,name,sizeof(G_desk[0].name));
    copy_str(G_desk[G_desk_n].path,path,sizeof(G_desk[0].path));
    G_desk[G_desk_n].icon=icon; G_desk[G_desk_n].kind=kind; G_desk[G_desk_n].gx=gx; G_desk[G_desk_n].gy=gy; G_desk_n++;
}

void add_desktop(const char *name,const char *path,int icon,int kind){
    int gx=18+(G_desk_n%6)*104*G_scale, gy=PANEL_H+18*G_scale+(G_desk_n/4)*100*G_scale;
    add_desktop_xy(name,path,icon,kind,gx,gy);
}

void default_dock(void){
    G_dock_n=0;
    add_dock_app("","Show Apps",ICON_DOCK_APPS_GRID,true);
    add_dock_app("/bin/nyra","Console",ICON_DOCK_TERMINAL,false);
    add_dock_app("/bin/files","Files",ICON_DOCK_FILES,false);
    add_dock_app("trash","Trash",ICON_DOCK_TRASH,true);
}

void dock_apply_hidden(void){
    int fd=open("/home/yart/dock_hidden.conf",0); if(fd<0)return;
    char buf[256]; long n=read(fd,buf,sizeof(buf)-1); close(fd); if(n<=0)return; buf[n]=0;
    for(int pass=0;pass<3;pass++) for(int i=0;i<G_dock_n;){
        char *p=buf; bool hidden=false;
        while(*p){ char *e=p; while(*e&&*e!='\n')e++; char old=*e; *e=0; if(strcmp(p,G_dock[i].path)==0){ hidden=true; break;} p=old?e+1:e; }
        if(!G_dock[i].core && hidden){ for(int j=i;j<G_dock_n-1;j++){G_dock[j]=G_dock[j+1];G_core_state[j]=G_core_state[j+1];} G_dock_n--; } else i++;
    }
    rebuild_dock_cache();
}

void dock_save_hidden(const char *path,bool hidden){
    char buf[256]; int n=0; int fd=open("/home/yart/dock_hidden.conf",0);
    if(fd>=0){ n=(int)read(fd,buf,sizeof(buf)-2); if(n<0)n=0; close(fd); }
    int len=(int)strlen(path);
    if(hidden){
        bool has=false;
        for(int i=0;i<n;){ int j=i; while(j<n&&buf[j]!='\n')j++; if(j-i==len){ int same=1; for(int k=0;k<len;k++) if(buf[i+k]!=path[k]) same=0; if(same){has=true;break;} } i=j+1; }
        if(!has&&n+len+2<(int)sizeof(buf)){ for(int k=0;k<len;k++) buf[n++]=path[k]; buf[n++]='\n'; buf[n]=0; fd=open("/home/yart/dock_hidden.conf",O_WRONLY|O_CREAT|O_TRUNC); if(fd>=0){write(fd,buf,n);close(fd);} }
    } else {
        int w=0;
        for(int i=0;i<n;){ int j=i; while(j<n&&buf[j]!='\n')j++; int nl=(j<n); bool same=(j-i==len); for(int k=0;k<len&&same;k++) if(buf[i+k]!=path[k]) same=0; if(!same){ for(int k=i;k<j;k++) buf[w++]=buf[k]; if(nl) buf[w++]='\n'; } i=j+1; }
        buf[w]=0; fd=open("/home/yart/dock_hidden.conf",O_WRONLY|O_CREAT|O_TRUNC); if(fd>=0){write(fd,buf,w);close(fd);}
    }
}

int dock_slot_x(int i){ return G_dock_x+G_dock_w/2-(G_dock_n-1)*DOCK_PITCH/2+i*DOCK_PITCH; }

int dock_hit(int x,int y){ if(!G_dock_visible) return -1; if(!ptin(x,y,G_dock_x-8,G_dock_y-8,G_dock_x+G_dock_w+8,G_dock_y+G_dock_h+8))return -1; for(int i=0;i<G_dock_n;i++){ int cx=dock_slot_x(i); if(absi(x-cx)<DOCK_PITCH/2-2)return i; } return -2; }

bool dock_animating(void){
    for(int i=0;i<G_dock_n;i++){
        if(absi(G_slot_sz[i]-G_slot_target_sz[i])>1) return true;
        if(absi(G_slot_lift[i]-G_slot_target_lift[i])>1) return true;
        if(G_slot_bounce[i]) return true;
    }
    return false;
}

void dock_update(long now){
    int hover = dock_hit(G_cx, G_cy);
    bool hover_changed = (hover != G_dock_hover);
    G_dock_hover = hover;
    /* Smooth, distance-based magnification (Skift/macOS style). */
    int max_sz = DOCK_REST+14;
    for(int i=0;i<G_dock_n;i++){
        int target=DOCK_REST, lift=0;
        if(hover>=0){
            int dist=absi(i-hover);
            if(dist==0){ target=max_sz; lift=8; }
            else if(dist==1){ target=DOCK_REST+7; lift=3; }
            else if(dist==2){ target=DOCK_REST+2; lift=1; }
        }
        target+=G_slot_bounce[i];
        G_slot_target_sz[i]=target; G_slot_target_lift[i]=lift;
        /* frame-rate-independent easing: move ~17% toward target */
        int ds=G_slot_target_sz[i]-G_slot_sz[i];
        int dl=G_slot_target_lift[i]-G_slot_lift[i];
        G_slot_sz[i]+=ds/6; if(absi(ds)<=1)G_slot_sz[i]=G_slot_target_sz[i];
        G_slot_lift[i]+=dl/6; if(absi(dl)<=1)G_slot_lift[i]=G_slot_target_lift[i];
        if(G_slot_bounce[i]){G_slot_bounce[i]=(G_slot_bounce[i]*7)/10; if(absi(G_slot_bounce[i])<2)G_slot_bounce[i]=0;}
    }
    /* tooltip timer (state updated here, not during paint) */
    if(hover >= 0){
        if(hover != G_tooltip_item){ G_tooltip_item = hover; G_tooltip_t0 = now; }
        G_dock_tooltip = (now - G_tooltip_t0) > 350;
    } else {
        G_tooltip_item = -1;
        G_dock_tooltip = false;
    }
    if(hover_changed || dock_animating() || G_dock_tooltip != G_dock_tooltip_prev){
        damage_add((GfxRect){G_dock_x-30, G_dock_y-70, G_dock_w+60, G_dock_h+110});
    }
    G_dock_tooltip_prev = G_dock_tooltip;
}

void draw_dock(surface_t*s,long now){
    (void)now;
    if(!G_dock_visible) return;
    /* Dock background: a translucent GRAY950 pill + 1px GRAY800 outline,
     * matching the top bar's translucent style. */
    int px0=G_dock_x-12, py0=G_dock_y-6, pw2=G_dock_w+24, ph2=G_dock_h+16;
    sf_round_rect_blend(s,px0-1,py0-1,pw2+2,ph2+2,19,theme(T_WIN_BORDER));
    sf_round_rect_blend(s,px0,py0,pw2,ph2,18,theme(T_PANEL_BG));
    int hover=G_dock_hover;
    int max_sz=DOCK_REST+14;
    /* Now draw back-to-front so lifted icons overlap neighbors cleanly */
    for(int i=0;i<G_dock_n;i++){
        int cx=dock_slot_x(i),cy=G_dock_y+G_dock_h/2-G_slot_lift[i];
        if(i==hover) sf_round_rect_blend(s,cx-(max_sz/2+6),cy-(max_sz/2+6),max_sz+12,max_sz+12,14,theme(T_MENU_HOVER));
        icon_t ic=icon_get(G_dock[i].icon);
        if(!ic.px){ sf_round_rect_blend(s,cx-16,cy-16,32,32,8,ARGB(160,90,90,100)); }
        else if(G_slot_sz[i]==DOCK_REST && G_icon_cache[i].px){
            sf_blit_alpha(s,cx-DOCK_REST/2,cy-DOCK_REST/2,&G_icon_cache[i],0,0,DOCK_REST,DOCK_REST);
        } else {
            sf_icon_scaled(s,cx,cy,ic,0,G_slot_sz[i],48);
        }
        bool running=false;
        if(G_dock[i].path[0]=='/'){ for(int j=0;j<MAX_WIN;j++) if(G_win[j].active && !G_win[j].closing && strcmp(G_win[j].path,G_dock[i].path)==0){running=true;break;} if(!running) running=pid_for_path(G_dock[i].path); }
        if(running){ int doty=G_dock_y+G_dock_h-6-G_slot_lift[i]/2; sf_round_rect(s,cx-2,doty,4,4,2,theme(T_ACCENT)); }
        if(i==hover && G_dock_tooltip){ int tw=sf_text_width(G_dock[i].name),tw_w=tw+16,tw_h=22,tx2=cx-tw_w/2,ty2=G_dock_y-tw_h-14-G_slot_lift[i]; sf_round_rect_blend(s,tx2-2,ty2-2,tw_w+4,tw_h+4,7,ARGB(180,0,0,0)); sf_round_rect(s,tx2,ty2,tw_w,tw_h,6,theme(T_TOOLTIP_BG)); sf_text(s,tx2+8,ty2+5,G_dock[i].name,theme(T_TEXT)); }
    }
}

void desk_rect(int i,int*x0,int*y0,int*x1,int*y1){ *x0=G_desk[i].gx; *y0=G_desk[i].gy; *x1=*x0+88*G_scale; *y1=*y0+92*G_scale; }

void draw_label_clipped(surface_t*s,int cx,int y,const char*t,u32 col){
    int maxw=92; if(sf_text_width(t)<=maxw){sf_text(s,cx-sf_text_width(t)/2,y,t,col);return;}
    char buf[20]; int k=0; while(t[k]&&k<10){buf[k]=t[k];k++;} buf[k++]='.';buf[k++]='.';buf[k++]='.';buf[k]=0;
    while(k>4&&sf_text_width(buf)>maxw){ k--; buf[k]=0; }
    sf_text(s,cx-sf_text_width(buf)/2,y,buf,col);
}

int desk_hit(int x,int y){ for(int i=0;i<G_desk_n;i++){int x0,y0,x1,y1;desk_rect(i,&x0,&y0,&x1,&y1); if(ptin(x,y,x0,y0,x1,y1))return i;} return -1; }

/* Single-click selection ONLY.  The old code returned `G_multi_sel` here, so
 * once a marquee drag set G_multi_sel every icon on the desktop was
 * highlighted ("drag selects ALL apps").  Marquee (rubber-band) selection is
 * computed separately in draw_desktop_live() against the actual rectangle
 * intersection, and it now resets on release. */
bool desk_selected(int i){ return G_sel_desk==i; }

void draw_desktop_icons(surface_t*s){
    for(int i=0;i<G_desk_n;i++){
        int x0,y0,x1,y1; desk_rect(i,&x0,&y0,&x1,&y1);
        icon_t ic=icon_get(G_desk[i].icon); sf_icon_scaled(s,(x0+x1)/2,y0+20*G_scale,ic,0,44*G_scale,48);
        int tw=sf_text_width(G_desk[i].name), tx=(x0+x1)/2-tw/2; if(tx<x0)tx=x0; if(tx+tw>x1)tx=x1-tw;
        /* label sits right under the icon, no backdrop (Skift keeps icons bare) */
        draw_label_clipped(s,(x0+x1)/2,y0+45*G_scale,G_desk[i].name,theme(T_TEXT));
    }
}

void draw_desktop_live(surface_t*s){
    for(int i=0;i<G_desk_n;i++){
        int x0,y0,x1,y1; desk_rect(i,&x0,&y0,&x1,&y1);
        bool sel=desk_selected(i); if(G_multi_sel&&G_marquee){ int rx0=mini(G_mx0,G_mx1),ry0=mini(G_my0,G_my1),rx1=maxi(G_mx0,G_mx1),ry1=maxi(G_my0,G_my1); sel=sel||(x0<rx1&&x1>rx0&&y0<ry1&&y1>ry0); }
        if(sel) sf_round_rect_blend(s,x0-4,y0-4,84*G_scale,86*G_scale,10,theme(T_DESKTOP_SEL));
    }
    if(G_marquee){ int x0=mini(G_mx0,G_mx1),y0=mini(G_my0,G_my1),w=absi(G_mx1-G_mx0),h=absi(G_my1-G_my0); sf_fill_rect_blend(s,x0,y0,w,h,ARGB(35,91,167,223)); sf_rect_outline(s,x0,y0,w,h,RGB(0x9B,0xD0,0xFF)); }
}
