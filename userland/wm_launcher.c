/* App launcher panel + dock dropdown + search. */
#include "wm.h"

bool search_match(const char*name){
    if(!G_search_len)return true;
    for(int i=0;name[i];i++){
        int ok=1; for(int j=0;G_search[j];j++){ char pc=name[i+j]; if(!pc){ok=0;break;} char a=pc; char b=G_search[j]; if(a>='A'&&a<='Z')a=(char)(a+32); if(b>='A'&&b<='Z')b=(char)(b+32); if(a!=b){ok=0;break;} } if(ok)return true;
    }
    return false;
}

void draw_app_row(surface_t*s, int x, int y, int w, int h, app_t *a){
    int sc = G_scale;
    bool hov=ptin(G_cx,G_cy,x,y,x+w,y+h);
    if(hov) sf_round_rect_blend(s,x,y,w,h,6,theme(T_MENU_HOVER));
    /* Same kora colour icon as the dock: one icon registry, one look. */
    int isz=28*sc, icx=x+8*sc+isz/2, icy=y+h/2;
    icon_t ic=icon_get(a->icon);
    if(ic.px) sf_icon_scaled(s,icx,icy,ic,0,isz,48);
    else sf_round_rect_blend(s,icx-isz/2,icy-isz/2,isz,isz,6,ARGB(160,90,90,100));
    sf_text(s,x+8*sc+isz+12*sc,y+(h-18*sc)/2,a->name,theme(T_TEXT));
}

int app_grid_hit(int x,int y,int*idx){
    if(!G_grid)return false;
    int sc = G_scale;
    int w=500*sc,h=440*sc,px=G_fb.w/2-w/2,py=G_fb.h/2-h/2-20*sc;
    if(w>G_fb.w-8) w=G_fb.w-8;
    if(h>G_fb.h-40) h=G_fb.h-40;
    px=G_fb.w/2-w/2;
    if(!ptin(x,y,px,py,px+w,py+h)){ *idx=-2; return true; }         /* outside -> close */
    if(ptin(x,y,px+14,py+12,px+w-14,py+12+46*sc)){ *idx=-1; return true; } /* search bar */
    int ry=py+12+46*sc+10;
    for(int i=0;i<G_apps;i++){ if(!search_match(G_app[i].name))continue;
        if(ptin(x,y,px+8,ry,px+w-8,ry+44*sc)){*idx=i;return true;}
        ry+=48*sc;
    }
    *idx=-1; return true;
}

int dockmenu_hit(int x,int y,int*idx){
    if(!G_dockmenu) return false;
    *idx=-1;
    if(!ptin(x,y,G_dockmenu_x,G_dockmenu_y,G_dockmenu_x+G_dockmenu_w,G_dockmenu_y+G_dockmenu_h))return false;
    if(ptin(x,y,G_dockmenu_x+10,G_dockmenu_y+8,G_dockmenu_x+G_dockmenu_w-10,G_dockmenu_y+34)){ *idx=-2; return true; }
    int top=G_dockmenu_y+40, row=0;
    for(int i=0;i<G_apps;i++){ if(!search_match(G_app[i].name))continue;
        int iy=top+row*40; if(iy>G_dockmenu_y+G_dockmenu_h-8)break;
        if(ptin(x,y,G_dockmenu_x+8,iy,G_dockmenu_x+G_dockmenu_w-8,iy+36)){*idx=i;return true;}
        row++;
    }
    *idx=-1; return true;
}

void draw_dockmenu(surface_t*s){
    if(!G_dockmenu)return;
    sf_round_rect_blend(s,G_dockmenu_x-4,G_dockmenu_y-4,G_dockmenu_w+8,G_dockmenu_h+8,14,theme(T_WIN_SHADOW));
    sf_round_rect_blend(s,G_dockmenu_x-1,G_dockmenu_y-1,G_dockmenu_w+2,G_dockmenu_h+2,13,theme(T_WIN_BORDER));
    sf_round_rect(s,G_dockmenu_x,G_dockmenu_y,G_dockmenu_w,G_dockmenu_h,12,theme(T_OVERLAY_SURFACE));
    sf_round_rect_blend(s,G_dockmenu_x+10,G_dockmenu_y+8,G_dockmenu_w-20,30,10,theme(T_SEARCH_BG));
    icon_t sric=icon_get(ICON_ACT_SYSTEM_SEARCH);
    if(sric.px) sf_icon_scaled(s,G_dockmenu_x+24,G_dockmenu_y+23,sric,theme(T_TEXT_DIM),14,24);
    char shown[40]; int k=0; while(G_search[k]&&k<30)shown[k]=G_search[k],k++; shown[k]=0;
    if(!k){const char*q="Search apps..."; int j=0;while(q[j]){shown[j]=q[j];j++;}shown[j]=0;}
    sf_text(s,G_dockmenu_x+44,G_dockmenu_y+16,shown,k?theme(T_TEXT):theme(T_TEXT_DIM));
    int top=G_dockmenu_y+44, row=0;
    for(int i=0;i<G_apps;i++){ if(!search_match(G_app[i].name))continue;
        int iy=top+row*40; if(iy>G_dockmenu_y+G_dockmenu_h-44)break;
        app_t app2=G_app[i];
        draw_app_row(s,G_dockmenu_x+8,iy,G_dockmenu_w-16,36,&app2);
        row++;
    }
}

void draw_app_grid(surface_t*s){
    /* Skift appsLauncher: a centered 500x440 panel (GRAY900, GRAY800 border,
     * elevated shadow) with a search bar and a list of app rows. */
    int sc = G_scale;
    int w=500*sc, h=440*sc, x=s->w/2-w/2, y=s->h/2-h/2-20*sc;
    if(w>s->w-8) w=s->w-8;
    if(h>s->h-40) h=s->h-40;
    x=s->w/2-w/2;
    win_shadow(x,y,w,h);
    sf_round_rect_blend(s,x-1,y-1,w+2,h+2,13,theme(T_WIN_BORDER));
    sf_round_rect(s,x,y,w,h,12,theme(T_OVERLAY_SURFACE));
    /* search bar */
    int sbh=46*sc;
    sf_round_rect_blend(s,x+14*sc,y+12*sc,w-28*sc,sbh,10,theme(T_SEARCH_BG));
    icon_t search=icon_get(ICON_ACT_SYSTEM_SEARCH);
    if(search.px) sf_icon_scaled(s,x+w-38*sc,y+12*sc+sbh/2,search,theme(T_TEXT_DIM),16*sc,48);
    char shown[44]; int k=0; while(G_search[k]&&k<34)shown[k]=G_search[k],k++; shown[k]=0;
    if(!k){const char*q="Search for anything..."; int j=0; while(q[j]){shown[j]=q[j];j++;} shown[j]=0;}
    sf_text(s,x+30*sc,y+12*sc+(sbh-18*sc)/2,shown,k?theme(T_TEXT):theme(T_TEXT_FAINT));
    /* separator */
    sf_hline(s,x+14*sc,y+12*sc+sbh+9*sc,w-28*sc,theme(T_WIN_BORDER));
    /* app rows */
    int ry=y+12*sc+sbh+22*sc;
    for(int i=0;i<G_apps;i++){ if(!search_match(G_app[i].name))continue;
        if(ry+44*sc>y+h-12)break;
        draw_app_row(s,x+8,ry,w-16,44*sc,&G_app[i]);
        ry+=48*sc;
    }
}
