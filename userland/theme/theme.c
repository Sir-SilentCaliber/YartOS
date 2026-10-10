/* YartOS Theme Engine - default theme + ini loader. */
#include "theme.h"
#include <stddef.h>
static int t_strcmp(const char*a,const char*b){ while(*a&&*a==*b){a++;b++;} return (int)(unsigned char)*a-(int)(unsigned char)*b; }

theme_t g_theme;

#define C(r,g,b)     (0xFF000000u | ((u32)(r)<<16) | ((u32)(g)<<8) | (u32)(b))
#define CA(a,r,g,b)  (((u32)(a)<<24) | ((u32)(r)<<16) | ((u32)(g)<<8) | (u32)(b))

void theme_reset_defaults(void){
    theme_t *t=&g_theme;
    for(int i=0;i<T__COUNT;i++) t->c[i]=0;
    /* surfaces */
    t->c[T_PANEL_BG]      = CA(153,0x09,0x09,0x0B);       /* GRAY950 @ 60% see-through */
    t->c[T_DOCK_BG]       = CA(170,0x18,0x18,0x1B);       /* GRAY900 frosted       */
    t->c[T_DOCK_SHADOW]   = CA(90,0,0,0);
    t->c[T_DESKTOP_SEL]   = CA(60,0x3B,0x82,0xF6);        /* ACCENT selection      */
    t->c[T_WIN_BG]        = C(0x18,0x18,0x1B);            /* GRAY900 window chrome */
    t->c[T_WIN_TITLE]     = C(0x18,0x18,0x1B);            /* GRAY900 titlebar      */
    t->c[T_WIN_BORDER]    = C(0x27,0x27,0x2A);            /* GRAY800 1px border    */
    t->c[T_WIN_SHADOW]    = CA(110,0,0,0);
    t->c[T_OVERLAY_BG]    = CA(210,8,10,15);
    t->c[T_OVERLAY_SURFACE]= C(0x18,0x18,0x1B);           /* GRAY900 panel         */
    t->c[T_MENU_BG]       = C(0x18,0x18,0x1B);            /* GRAY900               */
    t->c[T_MENU_HOVER]    = C(0x3F,0x3F,0x46);            /* GRAY700 hover         */
    t->c[T_TOOLTIP_BG]    = C(0x27,0x27,0x2A);            /* GRAY800               */
    t->c[T_GRID_BG]       = C(0x18,0x18,0x1B);            /* GRAY900 panel         */
    t->c[T_SEARCH_BG]     = C(0x27,0x27,0x2A);            /* GRAY800 search field  */
    /* chrome */
    t->c[T_BTN_CLOSE]     = CA(0,0,0,0);                  /* subtle idle           */
    t->c[T_BTN_CLOSE_GLYPH]= C(0xD4,0xD4,0xD8);           /* GRAY300 glyph         */
    t->c[T_BTN_MIN]       = CA(0,0,0,0);
    t->c[T_BTN_MIN_GLYPH] = C(0xD4,0xD4,0xD8);
    t->c[T_BTN_MAX]       = CA(0,0,0,0);
    t->c[T_BTN_MAX_GLYPH] = C(0xD4,0xD4,0xD8);
    t->c[T_BTN_TOGGLE_ON] = CA(220,0x3B,0x82,0xF6);       /* ACCENT                */
    t->c[T_BTN_TOGGLE_OFF]= C(0x3F,0x3F,0x46);            /* GRAY700               */
    /* text */
    t->c[T_TEXT]          = C(0xFA,0xFA,0xFA);            /* GRAY50                */
    t->c[T_TEXT_DIM]      = C(0xA1,0xA1,0xAA);            /* GRAY400               */
    t->c[T_TEXT_FAINT]    = C(0x71,0x71,0x7A);            /* GRAY500 placeholder   */
    t->c[T_TEXT_ON_ACCENT]= C(0xFF,0xFF,0xFF);
    t->c[T_ACCENT]        = C(0x3B,0x82,0xF6);            /* BLUE500               */
    t->c[T_ACCENT_DIM]    = C(0x25,0x63,0xEB);            /* BLUE600               */
    t->c[T_DANGER]        = C(0xEF,0x44,0x44);            /* RED500                */
    t->c[T_FOLDER]        = C(0xF5,0x9E,0x0B);            /* AMBER500 (Files)      */
    t->c[T_CURSOR_OUTLINE]= CA(180,0,0,0);
}

u32 theme_parse_color(const char *s){
    if(!s||!*s) return 0;
    while(*s==' '||*s=='\t') s++;
    if(*s=='#'){
        s++;
        u32 v=0; int n=0;
        while(n<8){
            char ch=*s++;
            if(!ch) break;
            u32 d;
            if(ch>='0'&&ch<='9')d=ch-'0';
            else if(ch>='a'&&ch<='f')d=ch-'a'+10;
            else if(ch>='A'&&ch<='F')d=ch-'A'+10;
            else break;
            v=(v<<4)|d; n++;
        }
        if(n==6) return 0xFF000000u|v;
        if(n==8) return v;
        return 0;
    }
    /* r,g,b(,a) */
    u32 parts[4]={255,0,0,0}; int pi=0;
    for(const char*p=s;*p&&pi<4;){
        u32 v=0;
        while(*p>='0'&&*p<='9'){v=v*10+(*p-'0');p++;}
        parts[pi++]=v>255?255:v;
        if(*p==',')p++; else if(*p) break;
    }
    if(pi>=3) return CA(parts[0],parts[1],parts[2],parts[3]);
    return 0;
}

/* Map ini key -> color id. Keep sorted-ish by frequency. */
static int key_to_id(const char *k){
    #define K(n,s) if(t_strcmp(k,n)==0) return s
    K("panel_bg",T_PANEL_BG); K("dock_bg",T_DOCK_BG);
    K("dock_shadow",T_DOCK_SHADOW); K("desktop_sel",T_DESKTOP_SEL);
    K("win_bg",T_WIN_BG); K("win_title",T_WIN_TITLE);
    K("win_border",T_WIN_BORDER); K("win_shadow",T_WIN_SHADOW);
    K("overlay_bg",T_OVERLAY_BG); K("overlay_surface",T_OVERLAY_SURFACE);
    K("menu_bg",T_MENU_BG); K("menu_hover",T_MENU_HOVER);
    K("tooltip_bg",T_TOOLTIP_BG); K("grid_bg",T_GRID_BG);
    K("search_bg",T_SEARCH_BG);
    K("btn_close",T_BTN_CLOSE); K("btn_close_glyph",T_BTN_CLOSE_GLYPH);
    K("btn_min",T_BTN_MIN); K("btn_min_glyph",T_BTN_MIN_GLYPH);
    K("btn_max",T_BTN_MAX); K("btn_max_glyph",T_BTN_MAX_GLYPH);
    K("toggle_on",T_BTN_TOGGLE_ON); K("toggle_off",T_BTN_TOGGLE_OFF);
    K("text",T_TEXT); K("text_dim",T_TEXT_DIM);
    K("text_faint",T_TEXT_FAINT); K("text_on_accent",T_TEXT_ON_ACCENT);
    K("accent",T_ACCENT); K("accent_dim",T_ACCENT_DIM);
    K("danger",T_DANGER); K("folder",T_FOLDER);
    K("cursor_outline",T_CURSOR_OUTLINE);
    #undef K
    return -1;
}

int theme_load(const char *path){
    int fd = open(path, 0);
    if(fd<0) return -1;
    static char buf[4096];
    long n=read(fd,buf,sizeof(buf)-1);
    close(fd);
    if(n<=0) return -1;
    buf[n]=0;
    char *p=buf;
    while(*p){
        char *e=p; while(*e&&*e!='\n')e++;
        char *line=p; int len=(int)(e-p);
        /* strip comment + cr */
        for(int i=0;i<len;i++){ if(line[i]=='#'){len=i;break;} }
        while(len&&(line[len-1]=='\r'||line[len-1]==' '))len--;
        /* find '=' */
        int eq=-1; for(int i=0;i<len;i++) if(line[i]=='='){eq=i;break;}
        if(eq>0){
            char key[40]; int kl=eq; while(kl&&(key[kl-1]==' '||key[kl-1]=='\t'))kl--;
            int ki; for(ki=0;ki<kl&&ki<39;ki++)key[ki]=line[ki]; key[ki]=0;
            const char *val=line+eq+1; while(*val==' '||*val=='\t')val++;
            int id=key_to_id(key);
            if(id>=0){ u32 c=theme_parse_color(val); if(c) g_theme.c[id]=c; }
        }
        p=*e?e+1:e;
    }
    return 0;
}

int theme_save(const char *path){
    int fd=open(path, O_WRONLY|O_CREAT|O_TRUNC);
    if(fd<0) return -1;
    /* write a documented template */
    static const char hdr[]="# YartOS theme (overrides defaults)\n# Colors: #rrggbb or #aarrggbb or r,g,b,a\n\n";
    write(fd,hdr,sizeof(hdr)-1);
    static const char*names[T__COUNT]={
        "panel_bg","dock_bg","dock_shadow","desktop_sel","win_bg","win_title",
        "win_border","win_shadow","overlay_bg","overlay_surface","menu_bg",
        "menu_hover","tooltip_bg","grid_bg","search_bg","btn_close",
        "btn_close_glyph","btn_min","btn_min_glyph","btn_max","btn_max_glyph",
        "toggle_on","toggle_off","text","text_dim","text_faint",
        "text_on_accent","accent","accent_dim","danger","folder","cursor_outline"};
    char line[80];
    for(int i=0;i<T__COUNT;i++){
        u32 c=g_theme.c[i];
        int n=0;
        const char*k=names[i]; while(*k)line[n++]=*k++;
        line[n++]='=';line[n++]='#';
        const char*hex="0123456789abcdef";
        for(int s=28;s>=0;s-=4)line[n++]=hex[(c>>s)&0xF];
        line[n++]='\n';
        write(fd,line,n);
    }
    close(fd);
    return 0;
}
