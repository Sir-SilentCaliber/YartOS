/* Window state machine, chrome paint, animations, z-order. */
#include "wm.h"

win_t G_win[MAX_WIN];
int   G_z_top;
int   G_focus_win = -1;

win_t *win_find(u32 id){ for(int i=0;i<MAX_WIN;i++) if(G_win[i].active&&G_win[i].id==id)return &G_win[i]; return 0; }

win_t *win_at(int x,int y){
    for(int z=G_z_top;z>=0;z--) for(int i=0;i<MAX_WIN;i++) if(G_win[i].active&&!G_win[i].hidden&&!G_win[i].minimized&&(G_win[i].workspace==G_workspace||G_win[i].workspace<0)&&G_win[i].z==z){
        int ty=G_win[i].y-TB_H; if(ty<PANEL_H)ty=PANEL_H;
        if(ptin(x,y,G_win[i].x,ty,G_win[i].x+G_win[i].w,ty+TB_H+G_win[i].h)) return &G_win[i];
    }
    return 0;
}

void bring_front(win_t*w){
    w->z=++G_z_top; G_focus_win=(int)(w-G_win);
    for(int i=0;i<MAX_WIN;i++) if(G_win[i].active&&&G_win[i]!=w&&G_win[i].z>=w->z) G_win[i].z--;
    /* z-order changed: the region of this window must be recomposited so it
     * paints over whatever it now covers (Skift-style targeted damage). */
    if(w->active && !w->minimized && w->scale > 0.01f){
        GfxRect r; win_draw_rect(w, &r); damage_add(r);
    }
}

void close_win(win_t*w){
    if(w->closing) return;
    w->closing=true; w->anim_start=time_ms();
    w->anim_from_x=w->x; w->anim_from_y=w->y; w->anim_from_w=w->w; w->anim_from_h=w->h;
    /* tell the app to die; the close animation (win_update) repaints and
     * then destroys the surface. */
    kill((long)w->owner);
}

void toggle_max(win_t*w){
    if(w->maximized){ wm_move(w->id,w->saved_x,w->saved_y); w->w=w->saved_w; w->h=w->saved_h; w->maximized=false; w->snap=0; }
    else { w->saved_x=w->x; w->saved_y=w->y; w->saved_w=w->w; w->saved_h=w->h; wm_move(w->id,40,PANEL_H+10); w->w=G_fb.w-80; w->h=G_fb.h-PANEL_H-90; w->maximized=true; }
    w->dirty=true; g_backdrop_dirty=true; damage_whole();
}

void scan_windows(void){
    wm_surf_info_t arr[MAX_WIN]; int n=(int)wm_scan(arr,MAX_WIN); if(n<0)return;
    for(int i=0;i<MAX_WIN;i++)G_win[i].seen=false;
    for(int i=0;i<n;i++){
        win_t *w=win_find(arr[i].id);
        bool was_new = false;
        if(!w){ w=0; for(int j=0;j<MAX_WIN;j++) if(!G_win[j].active){w=&G_win[j];break;} if(!w)continue; w->active=true; w->id=arr[i].id; w->owner=arr[i].owner_pid; w->z=++G_z_top; w->maximized=false; w->minimized=false; w->closing=false; w->workspace=G_workspace; w->saved_x=w->saved_y=w->saved_w=w->saved_h=0; w->path[0]=0; w->title[0]=0;
                  w->anim_start=time_ms(); w->anim_from_x=arr[i].win_x; w->anim_from_y=arr[i].win_y; w->anim_from_w=arr[i].w; w->anim_from_h=arr[i].h; was_new=true; }
        w->seen=true; w->misses=0; w->x=arr[i].win_x; w->y=arr[i].win_y; w->w=arr[i].w; w->h=arr[i].h; w->va=arr[i].app_va; copy_str(w->title,arr[i].title,sizeof(w->title));
        if(!w->path[0]) for(int p=0;p<G_pending_n;p++) if(G_pending[p].pid==(long)arr[i].owner_pid){ copy_str(w->path,G_pending[p].path,sizeof(w->path)); break; }
        if(arr[i].dirty)w->dirty=true;
        /* New window: give it keyboard focus so typing works immediately.
         * (was_new = this surface did not exist in ANY previous scan -
         *  the old code reset `seen` first and mis-detected EVERY window as
         *  new, re-focusing on every scan = focus thrash + CPU waste.) */
        if(was_new){ wm_focus(arr[i].owner_pid); G_focus_win=(int)(w-G_win); bring_front(w); }
    }
    /* CRITICAL FIX: a surface that vanished from wm_scan() has been torn down
     * by the kernel (owner exited) and its WM-side mapping UNMAPPED.  Keeping
     * the window "active" for a grace frame left a stale w->va that draw_window
     * then read -> SIGSEGV (the "wm killed" crash) and dead compositor.  A
     * surface never comes back once destroyed, so deactivate on the FIRST
     * miss and forget its VA. */
    for(int i=0;i<MAX_WIN;i++) if(G_win[i].active&&!G_win[i].seen){
        GfxRect r; win_draw_rect(&G_win[i],&r); damage_add(r);
        G_win[i].active=false; G_win[i].dirty=false; G_win[i].va=0;
        G_win[i].prev_draw=(GfxRect){0,0,0,0};
        if(G_focus_win==i)G_focus_win=-1;
    }
}

void minimize_win(win_t*w){
    if(w->maximized) toggle_max(w);
    if(w->minimized || w->minimizing) return;
    /* genie-style flight toward the app's dock slot (or bottom-center). */
    w->minimizing = true;
    w->anim_start = time_ms();
    int di = dock_find_path(w->path);
    w->min_to_x = (di >= 0) ? dock_slot_x(di) : G_fb.w/2;
    w->min_to_y = G_dock_y + G_dock_h/2;
    wm_focus(0);
}

void restore_win(win_t*w){
    w->minimized = false; w->minimizing = false; w->draw_dx = w->draw_dy = 0;
    w->anim_start = time_ms();       /* spring back open */
    bring_front(w); wm_focus(w->owner);
}

float win_anim_progress(win_t*w, long now, int dur){
    if(!w->anim_start) return 1.0f;
    float t=(float)(now-w->anim_start)/(float)dur;
    if(t<0){t=0;} if(t>1){t=1;} return t;
}

void win_update(win_t*w, long now){
    w->scale = 1.0f; w->open_prog = 1.0f; w->alpha = 255;
    w->draw_dx = 0; w->draw_dy = 0;
    if(w->minimized) return;
    if(w->closing){
        float ap = 1.0f - win_anim_progress(w, now, 240);
        if(ap <= 0.01f){
            wm_destroy(w->id);
            if(G_focus_win == (int)(w - G_win)){ G_focus_win = -1; wm_focus(0); }
            w->active = false; w->closing = false;
            damage_add(w->prev_draw);
            w->prev_draw = (GfxRect){0,0,0,0};
            return;
        }
        w->scale = 0.85f + 0.15f * ap;
        w->open_prog = 1.0f;
        w->alpha = (u8)(255.0f * ap);
        return;
    }
    if(w->minimizing){
        float t = win_anim_progress(w, now, 240);
        float e = anim_ease_out(t);
        if(t >= 1.0f){
            w->minimized = true; w->minimizing = false;
            w->draw_dx = w->draw_dy = 0;
            return;
        }
        w->scale = 1.0f - 0.9f * e;
        w->alpha = (u8)(255.0f * (1.0f - e));
        w->draw_dx = (int)((w->min_to_x - (w->x + w->w/2)) * e);
        w->draw_dy = (int)((w->min_to_y - (w->y + w->h/2)) * e);
        return;
    }
    /* opening: springy ease-out-back pop */
    float op = win_anim_progress(w, now, 300);
    w->open_prog = op;
    w->scale = 0.9f + 0.1f * anim_ease_out_back(op);
    if(op >= 1.0f) w->anim_start = 0;
}

void win_draw_rect(win_t*w, GfxRect *out){
    int tb = TB_H;
    int tx = w->x + w->draw_dx, ty = w->y + w->draw_dy - tb; if(ty < PANEL_H){ ty = PANEL_H; }
    int total = w->h + tb;
    int cx = tx + w->w/2, cy = ty + total/2;
    int aw = (int)(w->w * w->scale), ah = (int)(total * w->scale);
    tx = cx - aw/2; ty = cy - ah/2;
    *out = (GfxRect){ tx-14, ty-4, aw+28, ah+22 };
}

void win_shadow(int tx,int ty,int aw,int ah){
    sf_round_rect_blend(&G_fb,tx-14,ty+6,aw+28,ah+22,20,ARGB(16,0,0,0));
    sf_round_rect_blend(&G_fb,tx-8, ty+3,aw+16,ah+14,15,ARGB(26,0,0,0));
    sf_round_rect_blend(&G_fb,tx-4, ty+1,aw+8, ah+8, 11,ARGB(40,0,0,0));
}

/* Skift-style window-control glyphs (MDI look, drawn with 2px strokes). */
void draw_close_glyph(int cx,int cy,u32 c){
    for(int i=-4;i<=4;i++){
        for(int t=0;t<2;t++){
            sf_putpx(&G_fb,cx+i,cy+i-t,c);
            sf_putpx(&G_fb,cx+i,cy-i+t,c);
        }
    }
}
static void draw_min_glyph(int cx,int cy,u32 c){
    sf_hline(&G_fb,cx-5,cy,11,c);
    sf_hline(&G_fb,cx-5,cy+1,11,c);
}
static void draw_max_glyph(int cx,int cy,u32 c){
    /* square outline */
    sf_hline(&G_fb,cx-5,cy-4,10,c); sf_hline(&G_fb,cx-5,cy+4,10,c);
    sf_vline(&G_fb,cx-5,cy-4,9,c);  sf_vline(&G_fb,cx+4,cy-4,9,c);
}
static void draw_restore_glyph(int cx,int cy,u32 c){
    /* two overlapping squares (restore) */
    sf_hline(&G_fb,cx-4,cy-2,7,c); sf_hline(&G_fb,cx-4,cy+4,7,c);
    sf_vline(&G_fb,cx-4,cy-2,7,c); sf_vline(&G_fb,cx+2,cy-2,7,c);
    sf_hline(&G_fb,cx-2,cy-4,7,c); sf_vline(&G_fb,cx-2,cy-4,7,c);
}

void draw_window(win_t*w){
    if(w->minimized || !w->active) return;
    int tb = TB_H;
    int tx = w->x + w->draw_dx, ty = w->y + w->draw_dy - tb; if(ty < PANEL_H){ ty = PANEL_H; }
    int total = w->h + tb;
    int cx = tx + w->w/2, cy = ty + total/2;
    int aw = (int)(w->w * w->scale), ah = (int)(total * w->scale);
    tx = cx - aw/2; ty = cy - ah/2;
    u8 alpha = w->alpha;
    if(alpha > 24) win_shadow(tx,ty,aw,ah);
    /* 1px rounded border + body (Skift: borderWidth 1 GRAY800, radii 8) */
    u32 bord = theme(T_WIN_BORDER);
    sf_round_rect_blend(&G_fb,tx-1,ty-1,aw+2,ah+2,9,((u32)alpha<<24)|(bord&0xFFFFFF));
    u32 bg = theme(T_WIN_BG);
    if(alpha>=250){ sf_round_rect(&G_fb,tx,ty,aw,ah,8,bg); }
    else { sf_round_rect_blend(&G_fb,tx,ty,aw,ah,8,((u32)alpha<<24)|(bg&0xFFFFFF)); }
    /* app surface (fills everything below the titlebar).  w->va is 0 for a
     * window whose surface is gone (see scan_windows) — skip rather than
     * dereference a stale/unmapped WM-side VA. */
    surface_t src; src.px=(u32*)(unsigned long)w->va; src.w=w->w; src.h=w->h; src.pitch=w->w;
    if(src.px && w->va && alpha>40){ int cw=aw, ch=ah-tb; if(cw>w->w)cw=w->w; if(ch>w->h)ch=w->h;
        if(G_scale == 2 && cw*2 <= G_fb.w && ch*2 <= G_fb.h)
            sf_blit_scaled(&G_fb, tx, ty+tb, cw*2, ch*2, &src, 0, 0, cw, ch);  /* HiDPI 2x upscale of 1x app surface */
        else
            sf_blit(&G_fb,tx,ty+tb,&src,0,0,cw,ch);
    }
    /* titlebar: app icon + title left; minimize / maximize / close buttons
     * right — subtle Skift buttons (transparent, GRAY300 glyph, GRAY700
     * hover). */
    int icx = tx+16, icy = ty+tb/2;
    icon_t wico = icon_get(icon_for_path(w->path));
    if(wico.px) sf_icon_scaled(&G_fb,icx,icy,wico,0,18,48);
    sf_text(&G_fb,tx+34,ty+(tb-18)/2,w->title,theme(T_TEXT));
    int bs = tb-8, by = ty+4;
    u32 glyph = theme(T_BTN_CLOSE_GLYPH);
    /* [min] [max] [close], right to left */
    int bx = tx+aw-tb+4;
    bool chov = ptin(G_cx,G_cy,bx,by,bx+bs,by+bs);
    if(chov) sf_round_rect_blend(&G_fb,bx,by,bs,bs,6,theme(T_MENU_HOVER));
    draw_close_glyph(bx+bs/2, by+bs/2, glyph);
    bx -= bs+2;
    bool xhov = ptin(G_cx,G_cy,bx,by,bx+bs,by+bs);
    if(xhov) sf_round_rect_blend(&G_fb,bx,by,bs,bs,6,theme(T_MENU_HOVER));
    if(w->maximized) draw_restore_glyph(bx+bs/2, by+bs/2, glyph);
    else draw_max_glyph(bx+bs/2, by+bs/2, glyph);
    bx -= bs+2;
    bool mhov = ptin(G_cx,G_cy,bx,by,bx+bs,by+bs);
    if(mhov) sf_round_rect_blend(&G_fb,bx,by,bs,bs,6,theme(T_MENU_HOVER));
    draw_min_glyph(bx+bs/2, by+bs/2, glyph);
    /* resize grip */
    sf_hline(&G_fb,tx+aw-10,ty+ah-3,8,RGB(0x52,0x52,0x5B));
    sf_hline(&G_fb,tx+aw-8,ty+ah-2,5,RGB(0x52,0x52,0x5B));
}

void windows_update_and_damage(long now){
    for(int i=0;i<MAX_WIN;i++){
        win_t *w = &G_win[i];
        if(!w->active){ w->prev_draw=(GfxRect){0,0,0,0}; continue; }
        win_update(w, now);          /* may destroy the window */
        if(!w->active) continue;
        bool on_ws = (w->workspace == G_workspace || w->workspace < 0);
        GfxRect cr;
        if(!on_ws || w->minimized || w->hidden) cr = (GfxRect){0,0,0,0};
        else win_draw_rect(w, &cr);
        bool changed = cr.x!=w->prev_draw.x || cr.y!=w->prev_draw.y ||
                       cr.w!=w->prev_draw.w || cr.h!=w->prev_draw.h;
        bool anim = w->closing || w->minimizing || w->open_prog < 1.0f;
        if(w->dirty || changed || anim){
            if(!rect_empty(cr) || !rect_empty(w->prev_draw))
                damage_add(rect_merge(cr, w->prev_draw));
        }
        w->prev_draw = cr;
        w->dirty = false;
    }
}

int count_windows_on_ws(void){
    int n=0; for(int i=0;i<MAX_WIN;i++){ win_t*w=&G_win[i];
        if(w->active&&!w->minimized&&!w->hidden&&!w->closing&&(w->workspace==G_workspace||w->workspace<0)) n++; }
    return n;
}
