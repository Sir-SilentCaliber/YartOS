/* Dirty-rectangle damage tracking (Skift _dirty / _damage(r)). */
#include "wm.h"

GfxRect G_dirty[MAX_DIRTY];
int     G_ndirty = 0;

void damage_whole(void){ G_dirty[0]=(GfxRect){0,0,G_fb.w,G_fb.h}; G_ndirty=1; }
void damage_add(GfxRect r){
    if(rect_empty(r))return;
    r=rect_clip(r,(GfxRect){0,0,G_fb.w,G_fb.h});
    if(rect_empty(r))return;
    for(int i=0;i<G_ndirty;i++){ if(rect_colide(G_dirty[i],r)){ G_dirty[i]=rect_merge(G_dirty[i],r); return; } }
    if(G_ndirty<MAX_DIRTY){ G_dirty[G_ndirty++]=r; }
    else damage_whole();
}

void damage_overlay_small(void){
    if(G_menu)     damage_add((GfxRect){G_menu_x-4,     G_menu_y-4,     G_menu_w+8,     G_menu_h+8});
    if(G_quick)    damage_add((GfxRect){G_quick_x-4,    G_quick_y-4,    G_quick_w+8,    G_quick_h+8});
    if(G_calendar) damage_add((GfxRect){G_cal_x-4,      G_cal_y-4,      G_cal_w+8,      G_cal_h+8});
    if(G_dockmenu) damage_add((GfxRect){G_dockmenu_x-4, G_dockmenu_y-4, G_dockmenu_w+8, G_dockmenu_h+8});
    if(G_clip_open)    damage_add((GfxRect){G_clip_x-4,    G_clip_y-4,    G_clip_w+8,    G_clip_h+8});
    if(G_netlist_open) damage_add((GfxRect){G_nl_x-4,      G_nl_y-4,      G_nl_w+8,      G_nl_h+8});
}

GfxRect osd_rect(void){
    int tw = sf_text_width(G_osd), w = tw + 30, h = 30;
    int x = G_fb.w/2 - w/2, y = G_fb.h - 100;
    return (GfxRect){ x-3, y-3, w+6, h+6 };
}
