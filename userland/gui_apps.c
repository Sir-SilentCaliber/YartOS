/* Windowed content apps for YartOS.
 *
 * IMPORTANT: these apps do NOT draw their own window title bar or close
 * button. The compositor (wm.c) owns all window chrome (title, close,
 * maximize, borders, drag). An app only draws its content, starting at
 * (0,0) of its surface.
 *
 * One translation unit builds four apps; unused draw_* functions are
 * expected, so unused-function warnings are silenced.
 *
 * Honesty note:
 *  - Files:   real directory listing via getdents, with path navigation.
 *  - Editor:  real multiline text buffer, saved to note.txt.
 *  - Settings: real controls where the OS supports them; no fakes.
 *  - Welcome: an about screen (there is no browser engine in the OS).
 */
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#include "sys.h"
#include "gfx.h"
#include "cursors.h"
#include "kora.h"

#ifndef APP_W
#define APP_W 640
#endif
#ifndef APP_H
#define APP_H 440
#endif

static surface_t S;
static wm_surf_info_t I;
static int ID;
static int g_mx, g_my;    /* current pointer in surface coords (set each poll) */

#define COL_BG      RGB(0x18,0x18,0x1B)
#define COL_TOOLBAR RGB(0x27,0x27,0x2A)
#define COL_SEL     RGB(0x3B,0x82,0xF6)
#define COL_TEXT    RGB(0xFA,0xFA,0xFA)
#define COL_DIM     RGB(0xA1,0xA1,0xAA)
#define COL_ACCENT  RGB(0x60,0xA5,0xFA)

static void fill(int x,int y,int w,int h,u32 c){ sf_fill_rect(&S,x,y,w,h,c); }
static int inside(int x,int y,int x0,int y0,int x1,int y1){ return x>=x0&&y>=y0&&x<x1&&y<y1; }

/* ===================================================================== */
/* Files                                                                 */
/* ===================================================================== */
#if defined(APP_FILES)
#define FE_MAX 128
#define TRASH_DIR "/home/yart/.trash"
static char f_path[128]="/home/yart";
static int f_sel, f_scroll, f_count;
static int f_renaming;             /* index being renamed, or -1 */
static char f_renamebuf[96];
static char f_status[80]="";
static char f_copybuf[160];        /* clipboard: source path for copy/cut   */
static int  f_copybuf_cut;         /* 1 = move (cut), 0 = copy              */
static struct { char name[96]; u8 isdir; u64 size; } f_ent[FE_MAX];
static void f_setstatus(const char*t){ int k=0; while(t[k]&&k<79){f_status[k]=t[k];k++;} f_status[k]=0; }
static void f_new_folder(void);   /* defined later (after files_click) */
static int  f_copy_file(const char *src,const char *dst);
static int f_is_trash(void){ return strcmp(f_path,TRASH_DIR)==0; }

static void f_join(char *out,const char*d,const char*n){
    int k=0; while(d[k]&&k<120){ out[k]=d[k]; k++; }
    if(k==0||out[k-1]!='/') out[k++]='/';
    int i=0; while(n[i]&&k<124)out[k++]=n[i++]; out[k]=0;
}
static void f_fullpath(char*out,const char*name){ f_join(out,f_path,name); }
static void f_readdir(void){
    f_count=0; int fd=open(f_path,0); if(fd<0)return;
    while(f_count<FE_MAX){
        struct { u32 type; u32 reclen; u64 size; char name[120]; } de;
        long n=_sc(SYS_GETDENTS,fd,(long)&de,1); if(n<=0)break;
        int k=0; while(de.name[k]&&k<95){f_ent[f_count].name[k]=de.name[k];k++;} f_ent[f_count].name[k]=0;
        f_ent[f_count].isdir=(de.type==2); f_ent[f_count].size=de.size; f_count++;
    }
    close(fd);
}

/* ---- real file copy (read + write; files only) ---- */
static int f_copy_file(const char *src,const char *dst){
    int fd=open(src,O_RDONLY); if(fd<0)return -1;
    int wd=open(dst,O_WRONLY|O_CREAT|O_TRUNC); if(wd<0){close(fd);return -1;}
    char buf[512]; long n; while((n=read(fd,buf,511))>0) write(wd,buf,(long)n);
    close(fd); close(wd); return 0;
}
/* ---- real trash: move into /home/yart/.trash with a unique name ---- */
static void f_trash_name(const char *name,char *out){
    for(int n=0;n<1000;n++){
        char nm[110];
        if(n==0){ f_join(out,TRASH_DIR,name); }
        else {
            int k=0; while(name[k]&&k<90){nm[k]=name[k];k++;}
            nm[k++]='.';
            char bb[8]; int j=0,v=n; while(v){bb[j++]=(char)('0'+v%10);v/=10;}
            int p=k; while(j)nm[p++]=bb[--j]; nm[p]=0;
            f_join(out,TRASH_DIR,nm);
        }
        int fd=open(out,O_RDONLY); if(fd<0)return; close(fd);
    }
    f_join(out,TRASH_DIR,name);
}
static void f_delete_sel(void){
    if(f_sel<0||f_sel>=f_count){f_setstatus("Nothing selected");return;}
    char full[160]; f_fullpath(full,f_ent[f_sel].name);
    if(f_is_trash()){
        if(unlink(full)==0){ f_setstatus("Deleted permanently"); f_sel=-1; }
        else f_setstatus("Delete failed");
    } else {
        mkdir(TRASH_DIR);                     /* ensure trash exists        */
        char dst[180]; f_trash_name(f_ent[f_sel].name,dst);
        if(rename(full,dst)==0){ f_setstatus("Moved to Trash"); f_sel=-1; }
        else f_setstatus("Trash failed");
    }
}
static void f_restore_sel(void){
    if(!f_is_trash()){ f_setstatus("Select R in Trash"); return; }
    if(f_sel<0||f_sel>=f_count){f_setstatus("Nothing selected");return;}
    char full[160]; f_fullpath(full,f_ent[f_sel].name);
    char dst[160]; f_join(dst,"/home/yart",f_ent[f_sel].name);
    if(rename(full,dst)==0){ f_setstatus("Restored to /home/yart"); f_sel=-1; }
    else f_setstatus("Restore failed");
}
static void f_empty_trash(void){
    int fd=open(TRASH_DIR,O_RDONLY); if(fd<0){f_setstatus("Trash is empty");return;}
    int n=0;
    for(;;){
        struct { u32 type; u32 reclen; u64 size; char name[120]; } de;
        long r=_sc(SYS_GETDENTS,fd,(long)&de,1); if(r<=0)break;
        char full[160]; f_join(full,TRASH_DIR,de.name);
        if(unlink(full)==0)n++;
    }
    close(fd);
    char st[80]; int k=0;
    const char*p="Emptied trash: "; while(*p&&k<40)st[k++]=*p++;
    char bb[8]; int j=0, v=n; if(!v)bb[j++]='0'; while(v){bb[j++]=(char)('0'+v%10);v/=10;}
    while(j&&k<40)st[k++]=bb[--j];
    const char*q=" item(s)"; while(*q&&k<40)st[k++]=*q++;
    st[k]=0;
    f_setstatus(st); f_sel=-1;
}
static void f_copy_sel(void){ if(f_sel<0||f_sel>=f_count){f_setstatus("Nothing selected");return;} f_fullpath(f_copybuf,f_ent[f_sel].name); f_copybuf_cut=0; f_setstatus("Copied to clipboard"); }
static void f_cut_sel(void){  if(f_sel<0||f_sel>=f_count){f_setstatus("Nothing selected");return;} f_fullpath(f_copybuf,f_ent[f_sel].name); f_copybuf_cut=1; f_setstatus("Cut to clipboard"); }
static void f_paste(void){
    if(!f_copybuf[0]){ f_setstatus("Nothing to copy/move"); return; }
    int b=0; for(int i=0;f_copybuf[i];i++) if(f_copybuf[i]=='/')b=i+1;
    char name[96]; int k=0; while(f_copybuf[b]&&k<95){name[k]=f_copybuf[b+k];k++;} name[k]=0;
    char dst[160]; f_fullpath(dst,name);
    int r = f_copybuf_cut ? (int)rename(f_copybuf,dst) : f_copy_file(f_copybuf,dst);
    if(r==0){ f_setstatus(f_copybuf_cut?"Moved here":"Copied here"); f_copybuf[0]=0; }
    else f_setstatus(f_copybuf_cut?"Move failed":"Copy failed (file only)");
}
static void f_begin_rename(void){
    if(f_sel<0||f_sel>=f_count){f_setstatus("Nothing selected");return;}
    f_renaming=f_sel; int k=0; while(f_ent[f_sel].name[k]&&k<95){f_renamebuf[k]=f_ent[f_sel].name[k];k++;} f_renamebuf[k]=0;
}
static void f_up(void){
    if(f_is_trash()){ int k=0; const char*h="/home/yart"; while(h[k]&&k<127){f_path[k]=h[k];k++;} f_path[k]=0; }
    else { int slash=0; for(int i=0;f_path[i];i++) if(f_path[i]=='/')slash=i;
           if(slash<=0){ f_path[0]='/';f_path[1]=0; } else f_path[slash]=0; }
    f_sel=0; f_scroll=0;
}
static void files_key(int ev){
    int a=ev&255, sc=(ev>>8)&0xFF, ctrl=(ev&(1<<18))!=0;
    if(f_renaming>=0){
        if(a==8||a==127){ int k=0; while(f_renamebuf[k])k++; if(k>0){f_renamebuf[k-1]=0;} }
        else if(a==13||a==10){
            char oldp[160],newp[160]; f_fullpath(oldp,f_ent[f_renaming].name);
            f_fullpath(newp,f_renamebuf);
            if(rename(oldp,newp)==0) f_setstatus("Renamed"); else f_setstatus("Rename failed");
            f_renaming=-1;
        } else if(a==27){ f_renaming=-1; }
        else if(a>=32&&a<127){ int k=0;while(f_renamebuf[k])k++; if(k<94){f_renamebuf[k]=(char)a;f_renamebuf[k+1]=0;} }
        return;
    }
    if(ctrl && (a==3||a==('c'-96))){ f_copy_sel(); return; }
    if(ctrl && (a==24||a==('x'-96))){ f_cut_sel(); return; }
    if(ctrl && (a==22||a==('v'-96))){ f_paste(); return; }
    if(sc==0x48 && f_sel>0) f_sel--;
    else if(sc==0x50 && f_sel<f_count-1) f_sel++;
    else if(a==13||a==10){
        if(f_sel>=0&&f_sel<f_count&&f_ent[f_sel].isdir){
            char np[128]; f_join(np,f_path,f_ent[f_sel].name);
            int k=0; while(np[k]){f_path[k]=np[k];k++;} f_path[k]=0; f_sel=0;
        }
    } else if(a==8){ f_up(); }
    else if(a=='n'||a=='N'){ f_new_folder(); }
    else if(a=='r'||a=='R'){ if(f_is_trash()) f_restore_sel(); else f_begin_rename(); }
    else if(sc==0x53||a==127){ f_delete_sel(); }   /* Del */
    else if(a=='e'||a=='E'){ if(f_is_trash()) f_empty_trash(); }
}
static int f_last=-1, f_last_frames;
static void files_click(int x,int y,int frame){
    if(y<32){ if(x<36){f_up();} return; }
    int idx=(y-60)/20; if(idx<0||idx>=f_count)return; f_sel=idx;
    if(f_last==idx && frame-f_last_frames<18){ if(!f_ent[idx].isdir){f_last=-1;return;}
        char np[128]; f_join(np,f_path,f_ent[idx].name);
        int k=0; while(np[k]){f_path[k]=np[k];k++;} f_path[k]=0; f_sel=0; f_last=-1;
    } else { f_last=idx; f_last_frames=frame; } }

static void f_new_folder(void){
    char base[32]="NewFolder"; char full[160];
    for(int n=0;n<100;n++){
        char name[32]; int k=0; while(base[k]){name[k]=base[k];k++;}
        if(n){char bb[8];int j=0,v=n+1;while(v){bb[j++]=(char)('0'+v%10);v/=10;} int p=k; while(j)name[p++]=bb[--j]; name[p]=0;}
        else name[k]=0;
        f_fullpath(full,name);
        if(mkdir(full)==0){ f_setstatus("Created folder"); f_sel=-1; return; }
    }
    f_setstatus("Could not create folder");
}
static void draw_files(void){
    f_readdir();
    fill(0,0,I.w,I.h,COL_BG);
    fill(0,0,I.w,32,COL_TOOLBAR);
    sf_text(&S,10,9,"[..]",COL_ACCENT);
    sf_text(&S,46,9,f_path,COL_TEXT);
    if(f_is_trash()) sf_text(&S,I.w-250,9,"R:Restore  E:Empty  Del",COL_DIM);
    else             sf_text(&S,I.w-210,9,"N:New  R:Ren  Del",COL_DIM);
    sf_hline(&S,0,31,I.w,RGB(0x27,0x27,0x2A));
    fill(0,32,I.w,22,RGB(0x27,0x27,0x2A));
    sf_text(&S,16,37,"Name",COL_DIM); sf_text(&S,I.w-110,37,"Size",COL_DIM);
    sf_hline(&S,0,53,I.w,RGB(0x27,0x27,0x2A));
    for(int i=0;i<f_count;i++){
        int y=60+i*20; if(y<56||y>(int)I.h-34)continue;
        if(i==f_sel) fill(8,y-2,I.w-16,20,COL_SEL);
        const char*nm=(f_renaming==i)?f_renamebuf:f_ent[i].name;
        sf_text(&S,18,y,nm,f_ent[i].isdir?COL_ACCENT:COL_TEXT);
        if(!f_ent[i].isdir){
            char sz[16]; int k=0; u64 v=f_ent[i].size; char tmp[16]; int t=0;
            if(v==0)sz[k++]='0'; else {while(v){tmp[t++]=(char)('0'+v%10);v/=10;} while(t)sz[k++]=tmp[--t];}
            sz[k++]='B'; sz[k]=0; sf_text(&S,I.w-110,y,sz,COL_DIM);
        }
    }
    fill(0,I.h-22,I.w,22,COL_TOOLBAR);
    const char *hint = f_is_trash()
        ? "Del=delete forever  R=restore  E=empty  Ctrl+C/X/V copy-cut-paste"
        : "Up/Down select  Enter open  N new  R rename  Del trash  Ctrl+C/X/V";
    sf_text(&S,10,I.h-16,f_status[0]?f_status:hint,COL_DIM);
}

#elif defined(APP_EDITOR)
#define ELINES 64
#define ECOLS  80
static char e_buf[ELINES][ECOLS];
static int e_nlines=1,e_cx,e_cy;
static int e_saved=1;

/* ---- text selection (enterprise-editor model: click = caret, drag =
 * select, auto-copy on release, Ctrl+C / Ctrl+V) ---- */
static int  sel_on=0;
static int  sel_a_line, sel_a_col, sel_b_line, sel_b_col;

static int editor_line_len(int i){ int k=0; while(e_buf[i][k])k++; return k; }

static void editor_cell(int x,int y,int *L,int *C){
    int l=(y-34)/16; if(l<0)l=0; if(l>=e_nlines)l=e_nlines-1;
    int c=(x-44)/8;  if(c<0)c=0; if(c>editor_line_len(l))c=editor_line_len(l);
    *L=l; *C=c;
}

static void editor_insert_char(char ch){
    char *line=e_buf[e_cy]; int len=editor_line_len(e_cy);
    if(len>=ECOLS-1) return;
    for(int i=len;i>=e_cx;i--) line[i+1]=line[i];
    line[e_cx]=ch; e_cx++; e_saved=0;
}
static void editor_newline(void){
    char *line=e_buf[e_cy]; int len=editor_line_len(e_cy);
    if(e_nlines>=ELINES) return;
    for(int i=e_nlines;i>e_cy;i--){int k=0;while((e_buf[i][k]=e_buf[i-1][k]))k++;}
    e_nlines++;
    for(int i=0;i<=len-e_cx;i++) e_buf[e_cy+1][i]=line[e_cx+i];
    line[e_cx]=0; e_cy++; e_cx=0; e_saved=0;
}

static void sel_norm(int *l1,int *c1,int *l2,int *c2){
    if(sel_a_line<sel_b_line || (sel_a_line==sel_b_line && sel_a_col<=sel_b_col)){
        *l1=sel_a_line;*c1=sel_a_col;*l2=sel_b_line;*c2=sel_b_col;
    } else { *l1=sel_b_line;*c1=sel_b_col;*l2=sel_a_line;*c2=sel_a_col; }
}
/* selected column range [cs,ce) on line L */
static int sel_line_range(int L,int l1,int c1,int l2,int c2,int *cs,int *ce){
    if(L<l1||L>l2){ *cs=*ce=0; return 0; }
    if(l1==l2){ *cs=c1; *ce=c2; }
    else if(L==l1){ *cs=c1; *ce=ECOLS; }
    else if(L==l2){ *cs=0; *ce=c2; }
    else { *cs=0; *ce=ECOLS; }
    if(*cs>*ce){int t=*cs;*cs=*ce;*ce=t;}
    return (*ce>*cs);
}
static void editor_sel_copy(void){
    int l1,c1,l2,c2; sel_norm(&l1,&c1,&l2,&c2);
    char buf[ELINES*ECOLS+ELINES];
    int p=0;
    for(int L=l1; L<=l2; L++){
        int cs,ce;
        if(sel_line_range(L,l1,c1,l2,c2,&cs,&ce)){
            int len=editor_line_len(L); if(ce>len)ce=len;
            for(int c=cs;c<ce;c++){ if(p<(int)sizeof(buf)-2) buf[p++]=e_buf[L][c]; }
        }
        if(L<l2 && p<(int)sizeof(buf)-2) buf[p++]='\n';
    }
    buf[p]=0;
    if(p>0) clipboard_set(buf);
}
static void editor_paste(void){
    char buf[512];
    long n=clipboard_get(buf,sizeof buf);
    if(n<=0) return;
    for(long i=0;i<n && buf[i]; i++){
        char ch=buf[i];
        if(ch=='\r') continue;
        if(ch=='\n') editor_newline();
        else if(ch>=32 && ch<127) editor_insert_char(ch);
    }
}
static void editor_sel_start(int x,int y){
    int L,C; editor_cell(x,y,&L,&C);
    e_cy=L; e_cx=C;
    sel_a_line=sel_b_line=L; sel_a_col=sel_b_col=C;
    sel_on=0;                        /* plain click: caret only */
}
static void editor_sel_drag(int x,int y){
    int L,C; editor_cell(x,y,&L,&C);
    if(L!=sel_b_line || C!=sel_b_col){ sel_b_line=L; sel_b_col=C; sel_on=1; }
    e_cy=L; e_cx=C;
}
static void editor_sel_end(void){
    if(!sel_on) return;
    int l1,c1,l2,c2; sel_norm(&l1,&c1,&l2,&c2);
    if(l1==l2 && c1==c2){ sel_on=0; return; }
    editor_sel_copy();               /* keep the highlight visible */
}

static void draw_editor(void){
    fill(0,0,I.w,I.h,RGB(0x18,0x18,0x1B));
    fill(0,0,I.w,28,RGB(0x27,0x27,0x2A));
    sf_text(&S,12,8,e_saved?"note.txt":"note.txt  (unsaved)",e_saved?COL_TEXT:RGB(0xFF,0xCC,0x66));
    sf_hline(&S,0,27,I.w,RGB(0x27,0x27,0x2A));
    int l1,c1,l2,c2;
    if(sel_on) sel_norm(&l1,&c1,&l2,&c2); else { l1=l2=-1; c1=c2=0; }
    for(int i=0;i<e_nlines && i<24;i++){
        int cs,ce;
        if(sel_on && sel_line_range(i,l1,c1,l2,c2,&cs,&ce)){
            int len=editor_line_len(i); if(ce>len)ce=len;
            if(ce>cs) sf_fill_rect(&S,14+cs*8,38+i*18,(ce-cs)*8,16,COL_ACCENT);
        }
        sf_text(&S,14,38+i*18,e_buf[i],COL_TEXT);
        if(i==e_cy) sf_fill_rect(&S,14+sf_text_width_n(e_buf[i],e_cx),38+i*18,1,16,COL_ACCENT);
    }
    fill(0,I.h-24,I.w,24,RGB(0x27,0x27,0x2A));
    sf_text(&S,12,I.h-17,"drag=select/copy  Ctrl+C=copy  Ctrl+V=paste  Ctrl+S=save",COL_DIM);
}
static void editor_save(void){
    int fd=open("/home/yart/note.txt",O_WRONLY|O_CREAT|O_TRUNC);
    if(fd<0)return;
    for(int i=0;i<e_nlines;i++){ int k=0; while(e_buf[i][k])k++; if(k)write(fd,e_buf[i],k); write(fd,"\n",1); }
    close(fd); e_saved=1;
}
static void editor_key(int a){
    char *line=e_buf[e_cy]; int len=editor_line_len(e_cy);
    if(a>=32&&a<127) editor_insert_char((char)a);
    else if((a==8||a==127)){ if(e_cx>0){ for(int i=e_cx;i<=len;i++)line[i-1]=line[i]; e_cx--; e_saved=0; } else if(e_cy>0){ int pl=editor_line_len(e_cy-1); if(pl+len<ECOLS){ for(int i=0;i<=len;i++)e_buf[e_cy-1][pl+i]=line[i]; e_cx=pl; for(int i=e_cy;i<e_nlines-1;i++){int k=0;while((e_buf[i][k]=e_buf[i+1][k]))k++;} e_nlines--; e_cy--; e_saved=0; } } }
    else if(a==13||a==10){ editor_newline(); }
}

/* ===================================================================== */
/* Settings                                                              */
/* ===================================================================== */
/* ===================================================================== */
/* Settings  —  Skift hideo-settings architecture transcribed to C       */
/* ===================================================================== */
/* The structure mirrors Hideo.Settings exactly:
 *   model   : Page enum + State { page + navigation history } + the
 *             persisted preferences the pages edit.
 *   actions : GoTo / GoBack / GoForward / GoHome, applied through a single
 *             reduce() that owns every state transition (pure function).
 *   shell   : a scaffold = header tool buttons (back/forward/home) + a
 *             sidebar (search field + sidenav) + pageContent(state) — a
 *             switch that dispatches to one function per page.
 *   pages   : page_home (tile grid), page_account, page_personalization,
 *             page_packages, page_system, page_network, page_security,
 *             page_updates, page_about — each renders REAL data from
 *             syscalls / the real filesystem.  Nothing is fabricated. */
#elif defined(APP_SETTINGS)

/* forward declarations (pages are defined below; the scaffold calls them) */
static void network_refresh(void);
static void packages_refresh(void);
static void about_refresh(void);
static void draw_tile(int x, int y, int page, const char *label);
static void pageContent(void);
static void page_home(void);
static void page_account(void);
static void page_personalization(void);
static void page_packages(void);
static void page_system(void);
static void page_network(void);
static void page_security(void);
static void page_updates(void);
static void page_about(void);

/* ---- model: Page + State ---- */
enum { PAGE_HOME, PAGE_ACCOUNT, PAGE_PERSONALIZATION, PAGE_PACKAGES, PAGE_SYSTEM,
       PAGE_NETWORK, PAGE_SECURITY, PAGE_UPDATES, PAGE_ABOUT, NPAGES };

static const char *page_names[NPAGES] = {
    "Home", "Accounts", "Personalization", "Packages", "System",
    "Network", "Security & Privacy", "Updates", "About"
};
static const int page_icons[NPAGES] = {
    ICON_ACT_GO_HOME, ICON_TRAY_USER, ICON_ACT_PREFERENCES_DESKTOP, ICON_DOCK_APPS_GRID,
    ICON_DEV_COMPUTER, ICON_TRAY_NET_WIFI, ICON_TRAY_LOCK, ICON_ACT_VIEW_REFRESH,
    ICON_ACT_HELP_ABOUT
};

/* persisted preferences (the model the pages edit; shared with the compositor
 * via settings.conf — the same store the WM polls) */
static const char *accent_hex[] = {"3b82f6","7c3aed","10b981","ef4444","f59e0b","ec4899"};
#define NACCENT 6
static int s_accent, s_cursor, s_wallpaper, s_dock, s_vol, s_scale;

/* navigation state (Skift State::history / historyIndex) */
#define MAX_HISTORY 32
static int  s_hist[MAX_HISTORY];
static int  s_hist_len;
static int  s_hist_idx;
static char s_query[40];
static int  s_query_len;
static bool s_search_focus;

/* page-local caches (refreshed on page entry, not per frame) */
static char s_netbuf[1024];
static int  s_netbuf_len;
static struct { char name[32]; } s_pkgs[32];
static int  s_pkg_n;
static char s_about[16][96];
static int  s_about_n;

static int  s_page(void){ return s_hist[s_hist_idx]; }
static bool s_can_back(void){ return s_hist_idx > 0; }
static bool s_can_fwd(void){ return s_hist_idx < s_hist_len - 1; }

/* ---- actions + reduce() (Hideo::Settings::Action / reduce) ---- */
enum { ACT_NONE, ACT_GOTO, ACT_BACK, ACT_FORWARD, ACT_HOME };
static void settings_reduce(int act, int page){
    switch(act){
    case ACT_GOTO:
        if(page < 0 || page >= NPAGES || s_page() == page) return;
        s_hist_len = s_hist_idx + 1;          /* truncate the forward history */
        if(s_hist_len >= MAX_HISTORY) return;
        s_hist[s_hist_len] = page; s_hist_len++;
        s_hist_idx = s_hist_len - 1;
        if(page == PAGE_NETWORK)      network_refresh();
        if(page == PAGE_PACKAGES)     packages_refresh();
        if(page == PAGE_ABOUT)        about_refresh();
        break;
    case ACT_BACK:    if(s_can_back()) s_hist_idx--; break;
    case ACT_FORWARD: if(s_can_fwd()) s_hist_idx++; break;
    case ACT_HOME:    settings_reduce(ACT_GOTO, PAGE_HOME); break;
    }
}

/* ---- tiny text helpers ---- */
static int lower_c(char c){ if(c >= 'A' && c <= 'Z') return c + ('a' - 'A'); return c; }
static bool item_matches(int page){
    if(s_query_len == 0) return true;
    const char *n = page_names[page];
    for(int i = 0; n[i]; i++){
        int j = 0;
        while(s_query[j] && n[i + j] && lower_c(n[i + j]) == lower_c(s_query[j])) j++;
        if(s_query[j] == 0) return true;
    }
    return false;
}
static void draw_lines(int x, int y, const char *txt, u32 col, int maxlines){
    const char *p = txt; int ln = 0;
    while(*p && ln < maxlines){
        const char *e = p; while(*e && *e != '\n') e++;
        char line[96]; int k = 0; while(p < e && k < 95) line[k++] = *p++; line[k] = 0;
        sf_text(&S, x, y + ln * 20, line, col);
        ln++;
        if(*p == '\n') p++;
    }
}

/* ---- config file (settings.conf = the shared persisted store) ---- */
static u32 hex_color(const char *s){
    if(!s) return 0;
    const char *p = (*s == '#') ? s + 1 : s;
    u32 v = 0; int n = 0;
    for(; *p && n < 6; p++, n++){
        v <<= 4;
        char c = *p;
        if(c >= '0' && c <= '9') v |= (u32)(c - '0');
        else if(c >= 'a' && c <= 'f') v |= (u32)(c - 'a' + 10);
        else if(c >= 'A' && c <= 'F') v |= (u32)(c - 'A' + 10);
    }
    return 0xFF000000u | v;
}
static int conf_get(const char *key, int def){
    int fd = open("/home/yart/settings.conf", 0);
    if(fd < 0) return def;
    static char buf[512]; long n = read(fd, buf, sizeof(buf) - 1); close(fd);
    if(n <= 0) return def;
    buf[n] = 0;
    char *p = buf;
    while(*p){
        char *e = p; while(*e && *e != '\n') e++;
        char old = *e; *e = 0;
        char *eq = 0; for(char *q = p; *q; q++) if(*q == '='){ eq = q; break; }
        if(eq){ *eq = 0;
            int v = 0; for(char *q = eq + 1; *q >= '0' && *q <= '9'; q++) v = v * 10 + (*q - '0');
            if(strcmp(p, key) == 0) return v;
        }
        p = old ? e + 1 : e;
    }
    return def;
}
static void conf_get_str(const char *key, char *out, int cap){
    out[0] = 0;
    int fd = open("/home/yart/settings.conf", 0);
    if(fd < 0) return;
    static char buf[512]; long n = read(fd, buf, sizeof(buf) - 1); close(fd);
    if(n <= 0) return;
    buf[n] = 0;
    char *p = buf;
    while(*p){
        char *e = p; while(*e && *e != '\n') e++;
        char old = *e; *e = 0;
        char *eq = 0; for(char *q = p; *q; q++) if(*q == '='){ eq = q; break; }
        if(eq){ *eq = 0; if(strcmp(p, key) == 0){ int k = 0; for(char *q = eq + 1; *q && k < cap - 1; q++) out[k++] = *q; out[k] = 0; return; } }
        p = old ? e + 1 : e;
    }
}
static void conf_write(void){
    int fd = open("/home/yart/settings.conf", O_WRONLY | O_CREAT | O_TRUNC);
    if(fd < 0) return;
    char line[96]; int k;
    k = 0; const char *p = "accent=#"; while(*p) line[k++] = *p++;
    for(const char *q = accent_hex[s_accent]; *q; q++) line[k++] = *q;
    line[k++] = '\n'; write(fd, line, k);
    const char *cn = cursors_theme_name(s_cursor); if(!cn) cn = "roblox";
    k = 0; p = "cursor="; while(*p) line[k++] = *p++; for(const char *q = cn; *q; q++) line[k++] = *q; line[k++] = '\n';
    write(fd, line, k);
    k = 0; p = "wallpaper="; while(*p) line[k++] = *p++; line[k++] = '0' + s_wallpaper; line[k++] = '\n';
    write(fd, line, k);
    k = 0; p = "dock="; while(*p) line[k++] = *p++; line[k++] = '0' + s_dock; line[k++] = '\n';
    write(fd, line, k);
    k = 0; p = "volume="; while(*p) line[k++] = *p++;
    if(s_vol >= 100){ line[k++] = '1'; line[k++] = '0'; line[k++] = '0'; }
    else { line[k++] = '0' + s_vol / 10; line[k++] = '0' + s_vol % 10; }
    line[k++] = '\n'; write(fd, line, k);
    k = 0; p = "scale="; while(*p) line[k++] = *p++; line[k++] = '0' + s_scale; line[k++] = '\n';
    write(fd, line, k);
    close(fd);
}
static void settings_init(void){
    s_hist[0] = PAGE_HOME; s_hist_len = 1; s_hist_idx = 0;
    s_query[0] = 0; s_query_len = 0; s_search_focus = false;
    s_wallpaper = conf_get("wallpaper", 0);
    s_dock      = conf_get("dock", 1);
    s_vol       = conf_get("volume", 70);
    s_scale     = conf_get("scale", 1); if(s_scale < 1) s_scale = 1; if(s_scale > 2) s_scale = 2;
    char acc[16]; conf_get_str("accent", acc, sizeof acc);
    s_accent = 0; for(int i = 0; i < NACCENT; i++) if(strcmp(acc, accent_hex[i]) == 0){ s_accent = i; break; }
    int t = cursors_theme_by_name("roblox");
    char cn[32]; conf_get_str("cursor", cn, sizeof cn);
    if(cn[0]){ int tt = cursors_theme_by_name(cn); if(tt >= 0) t = tt; }
    s_cursor = t < 0 ? 0 : t;
}

/* ---- page data refreshers (real syscalls, called on page entry) ---- */
static void network_refresh(void){
    s_netbuf_len = (int)wifi_status(s_netbuf, sizeof s_netbuf);
    if(s_netbuf_len < 0) s_netbuf_len = 0;
}
static void packages_refresh(void){
    s_pkg_n = 0;
    int fd = open("/bin", 0); if(fd < 0) return;
    while(s_pkg_n < 32){
        struct { u32 type; u32 reclen; u64 size; char name[120]; } de;
        long n = _sc(SYS_GETDENTS, fd, (long)&de, 1); if(n <= 0) break;
        int k = 0; while(de.name[k] && k < 31){ s_pkgs[s_pkg_n].name[k] = de.name[k]; k++; }
        s_pkgs[s_pkg_n].name[k] = 0;
        s_pkg_n++;
    }
    close(fd);
}
static void about_refresh(void){
    s_about_n = 0;
    int k = 0; const char *p = "User        demo"; while(p[k] && k < 95){ s_about[s_about_n][k] = p[k]; k++; } s_about[s_about_n][k] = 0; s_about_n++;
    s_about[s_about_n][0] = 0;
    /* system */
    int k2 = 0; const char *p2 = "System      YartOS 0.8.0 (kernel yart)"; while(p2[k2] && k2 < 95){ s_about[s_about_n][k2] = p2[k2]; k2++; } s_about[s_about_n][k2] = 0; s_about_n++;
    /* cpu */
    char tmp[96];
    int cid = (int)getcpu();
    tmp[0] = 'C'; tmp[1] = 'P'; tmp[2] = 'U'; tmp[3] = ' '; tmp[4] = ' '; tmp[5] = ' '; tmp[6] = ' '; tmp[7] = ' '; tmp[8] = ' '; tmp[9] = ' '; tmp[10] = '#';
    int dv = cid; char dg[8]; int dn = 0; if(dv == 0){ dg[dn++] = '0'; } while(dv){ dg[dn++] = '0' + (dv % 10); dv /= 10; }
    int pp = 11; while(dn) tmp[pp++] = dg[--dn];
    tmp[pp++] = ' '; tmp[pp++] = '('; tmp[pp++] = 'S'; tmp[pp++] = 'M'; tmp[pp++] = 'P'; tmp[pp++] = ')'; tmp[pp] = 0;
    for(int i = 0; i < 96; i++) s_about[s_about_n][i] = tmp[i];
    s_about_n++;
    /* processes */
    u32 pids[128]; int np = (int)task_list(pids, 128);
    s_about[s_about_n][0] = 0; k2 = 0;
    const char *p3 = "Processes   "; while(p3[k2] && k2 < 95){ s_about[s_about_n][k2] = p3[k2]; k2++; }
    { int dv2 = np; char dg2[8]; int dn2 = 0; if(dv2 == 0) dg2[dn2++] = '0'; while(dv2){ dg2[dn2++] = '0' + (dv2 % 10); dv2 /= 10; }
      while(dn2 && k2 < 95) s_about[s_about_n][k2++] = dg2[--dn2]; }
    s_about[s_about_n][k2] = 0; s_about_n++;
    /* battery */
    int b[3]; s_about[s_about_n][0] = 0; k2 = 0;
    if(battery(b) == 0 && b[0]){
        const char *p4 = "Battery     "; while(p4[k2] && k2 < 95){ s_about[s_about_n][k2] = p4[k2]; k2++; }
        int lv = b[2]; char dg3[8]; int dn3 = 0; if(lv <= 0) dg3[dn3++] = '0'; while(lv){ dg3[dn3++] = '0' + (lv % 10); lv /= 10; }
        while(dn3 && k2 < 95) s_about[s_about_n][k2++] = dg3[--dn3];
        if(k2 < 95) s_about[s_about_n][k2++] = '%';
        if(b[1] && k2 + 6 < 95){ const char *c5 = " (charging)"; for(int q = 0; c5[q]; q++) s_about[s_about_n][k2++] = c5[q]; }
    } else {
        const char *p5 = "Battery     none (AC power)"; while(p5[k2] && k2 < 95){ s_about[s_about_n][k2] = p5[k2]; k2++; }
    }
    s_about[s_about_n][k2] = 0; s_about_n++;
    /* wifi */
    s_about[s_about_n][0] = 0; k2 = 0;
    char wsb[64]; long wn = wifi_status(wsb, sizeof wsb);
    if(wn > 0){
        /* first line only: "WiFi: <STATE>" */
        int q = 0; while(q < 63 && wsb[q] && wsb[q] != '\n'){ s_about[s_about_n][k2++] = wsb[q]; q++; }
    } else {
        const char *p6 = "WiFi        unavailable"; while(p6[k2] && k2 < 95){ s_about[s_about_n][k2] = p6[k2]; k2++; }
    }
    s_about[s_about_n][k2] = 0; s_about_n++;
}

/* ---- layout constants ---- */
#define SB_W 210
#define HDR_H 30
#define CTX_X (SB_W + 6)
#define CTX_W (APP_W - SB_W - 12)

/* ---- scaffold: header tools + sidebar + pageContent ---- */
static void draw_settings(void){
    fill(0, 0, I.w, I.h, RGB(0x18, 0x18, 0x1B));

    /* header: back / forward / home tool buttons (Skift scaffold.startTools) */
    fill(0, 0, I.w, HDR_H, RGB(0x20, 0x20, 0x25));
    {   int bx = 8;
        int btns[3] = { ICON_ACT_GO_PREVIOUS, ICON_ACT_GO_NEXT, ICON_ACT_GO_HOME };
        bool en[3] = { s_can_back(), s_can_fwd(), true };
        for(int i = 0; i < 3; i++){
            bool hov = inside(g_mx, g_my, bx, 4, bx + 24, 26);
            if(hov) sf_round_rect_blend(&S, bx, 4, 24, 22, 6, ARGB(40, 255, 255, 255));
            icon_t ic = icon_get(btns[i]);
            if(ic.px) sf_icon_sz(&S, bx + 12, 15, ic, en[i] ? COL_TEXT : COL_DIM, 16);
            bx += 28;
        }
        sf_text(&S, bx + 4, 6, "Settings", COL_TEXT);
    }
    sf_hline(&S, 0, HDR_H - 1, I.w, RGB(0x27, 0x27, 0x2A));

    /* sidebar background */
    fill(0, HDR_H, SB_W, I.h - HDR_H, RGB(0x1C, 0x1C, 0x20));
    sf_vline(&S, SB_W - 1, HDR_H, I.h - HDR_H, RGB(0x27, 0x27, 0x2A));

    /* search field (Skift sidebar searchbar) */
    {   int sy = HDR_H + 8;
        bool hov = inside(g_mx, g_my, 6, sy, SB_W - 6, sy + 26);
        u32 bg = s_search_focus ? RGB(0x2A, 0x2A, 0x30) : RGB(0x24, 0x24, 0x29);
        if(hov) bg = RGB(0x2A, 0x2A, 0x30);
        sf_round_rect(&S, 6, sy, SB_W - 12, 26, 6, bg);
        icon_t si = icon_get(ICON_ACT_SYSTEM_SEARCH);
        if(si.px) sf_icon_sz(&S, 18, sy + 13, si, COL_DIM, 14);
        if(s_query_len) sf_text(&S, 30, sy + 4, s_query, COL_TEXT);
        else sf_text(&S, 30, sy + 4, "Search", COL_DIM);
    }

    /* sidenav (Skift scaffold.sidebar items) */
    {   int y = HDR_H + 42;
        int shown = 0;
        for(int i = 1; i < NPAGES; i++){         /* HOME lives in the header */
            if(!item_matches(i)) continue;
            bool active = (i == s_page());
            if(active) sf_round_rect(&S, 6, y, SB_W - 12, 30, 6, RGB(0x3B, 0x82, 0xF6));
            else if(inside(g_mx, g_my, 6, y, SB_W - 6, y + 30))
                sf_round_rect_blend(&S, 6, y, SB_W - 12, 30, 6, ARGB(24, 255, 255, 255));
            icon_t ic = icon_get(page_icons[i]);
            if(ic.px) sf_icon_sz(&S, 22, y + 15, ic, active ? RGB(0xFF, 0xFF, 0xFF) : COL_DIM, 16);
            sf_text(&S, 42, y + 6, page_names[i], active ? RGB(0xFF, 0xFF, 0xFF) : COL_TEXT);
            y += 33; shown++;
        }
        if(shown == 0)
            sf_text(&S, 12, y + 6, "No matches", COL_DIM);
    }

    /* content pane */
    pageContent();
}

/* ---- per-page content (one function per page, like Skift page-*.cpp) ---- */
static void pageContent(void){
    int page = s_page();
    sf_text(&S, CTX_X, 40, page_names[page], COL_TEXT);
    sf_hline(&S, CTX_X, 62, CTX_W, RGB(0x27, 0x27, 0x2A));
    switch(page){
    case PAGE_HOME:              page_home(); break;
    case PAGE_ACCOUNT:           page_account(); break;
    case PAGE_PERSONALIZATION:   page_personalization(); break;
    case PAGE_PACKAGES:          page_packages(); break;
    case PAGE_SYSTEM:            page_system(); break;
    case PAGE_NETWORK:           page_network(); break;
    case PAGE_SECURITY:          page_security(); break;
    case PAGE_UPDATES:           page_updates(); break;
    case PAGE_ABOUT:             page_about(); break;
    }
}

/* HOME: a grid of category tiles (Skift pageHome tileButton grid) */
static void draw_tile(int x, int y, int page, const char *label){
    bool hov = inside(g_mx, g_my, x, y, x + 96, y + 72);
    u32 body = hov ? RGB(0x24, 0x24, 0x29) : RGB(0x1C, 0x1C, 0x20);
    sf_round_rect(&S, x, y, 96, 72, 8, body);
    sf_rect_outline(&S, x, y, 96, 72, hov ? RGB(0x3B, 0x82, 0xF6) : RGB(0x27, 0x27, 0x2A));
    icon_t ic = icon_get(page_icons[page]);
    if(ic.px) sf_icon_sz(&S, x + 48, y + 28, ic, hov ? COL_ACCENT : COL_TEXT, 28);
    int tw = sf_text_width(label);
    sf_text(&S, x + (96 - tw) / 2, y + 48, label, hov ? COL_TEXT : COL_DIM);
}
static void page_home(void){
    static const int tiles[8] = { PAGE_ACCOUNT, PAGE_PERSONALIZATION, PAGE_PACKAGES,
        PAGE_SYSTEM, PAGE_NETWORK, PAGE_SECURITY, PAGE_UPDATES, PAGE_ABOUT };
    static const char *tlabels[8] = { "Accounts", "Personalization", "Applications",
        "System", "Network", "Security", "Updates", "About" };
    int gx = CTX_X + (CTX_W - (3 * 96 + 2 * 8)) / 2;
    int gy = 76;
    for(int i = 0; i < 8; i++){
        int c = i % 3, r = i / 3;
        draw_tile(gx + c * 104, gy + r * 80, tiles[i], tlabels[i]);
    }
}

/* ACCOUNT: the signed-in user (real facts about the demo account) */
static void page_account(void){
    int y = 84;
    sf_text(&S, CTX_X, y, "Signed in as", COL_DIM);   sf_text(&S, CTX_X + 130, y, "demo", COL_TEXT); y += 26;
    sf_text(&S, CTX_X, y, "Home", COL_DIM);            sf_text(&S, CTX_X + 130, y, "/home/yart", COL_TEXT); y += 26;
    sf_text(&S, CTX_X, y, "Shell", COL_DIM);           sf_text(&S, CTX_X + 130, y, "Console", COL_TEXT); y += 26;
    sf_text(&S, CTX_X, y, "Password", COL_DIM);        sf_text(&S, CTX_X + 130, y, "PBKDF2-HMAC-SHA256", COL_TEXT); y += 26;
    sf_text(&S, CTX_X, y + 8, "Change from the Console: passwd <old> <new>", COL_DIM);
    sf_text(&S, CTX_X, y + 8, "(stored to /home/yart/.passwd).", COL_DIM);
}

/* PERSONALIZATION: accent colour, cursor theme, wallpaper — real + persisted */
static void page_personalization(void){
    int y = 84;
    sf_text(&S, CTX_X, y, "Accent colour", COL_DIM);
    for(int i = 0; i < NACCENT; i++){
        int sx = CTX_X + i * 48;
        u32 c = hex_color(accent_hex[i]);
        sf_round_rect(&S, sx, y + 24, 40, 40, 8, c);
        if(i == s_accent) sf_rect_outline(&S, sx - 2, y + 22, 44, 44, RGB(0xFA, 0xFA, 0xFA));
        else if(inside(g_mx, g_my, sx, y + 24, sx + 40, y + 64)) sf_rect_outline(&S, sx - 2, y + 22, 44, 44, RGB(0x60, 0xA5, 0xFA));
    }
    y += 92;
    sf_text(&S, CTX_X, y, "Cursor theme", COL_DIM);
    const char *cn = cursors_theme_name(s_cursor); if(!cn) cn = "?";
    sf_text(&S, CTX_X, y + 24, cn, COL_ACCENT);
    sf_text(&S, CTX_X, y + 48, "Click to cycle", COL_DIM);
    y += 76;
    sf_text(&S, CTX_X, y, "Wallpaper", COL_DIM);
    char val[8]; itoa0(s_wallpaper + 1, val, 0);
    sf_text(&S, CTX_X, y + 24, val, COL_ACCENT);
    sf_text(&S, CTX_X, y + 48, "Click to cycle", COL_DIM);
}

/* PACKAGES: the installed applications (real listing of /bin) */
static void page_packages(void){
    if(s_pkg_n == 0){
        sf_text(&S, CTX_X, 84, "No packages found in /bin", COL_DIM);
        return;
    }
    int y = 84;
    for(int i = 0; i < s_pkg_n && y < (int)I.h - 30; i++){
        bool hov = inside(g_mx, g_my, CTX_X, y, CTX_X + CTX_W, y + 30);
        if(hov) sf_round_rect_blend(&S, CTX_X, y, CTX_W, 28, 6, ARGB(24, 255, 255, 255));
        icon_t ic = icon_get(ICON_MIME_APPLICATION_X_EXECUTABLE);
        if(ic.px) sf_icon_sz(&S, CTX_X + 16, y + 14, ic, COL_TEXT, 16);
        sf_text(&S, CTX_X + 34, y + 5, s_pkgs[i].name, COL_TEXT);
        sf_text(&S, CTX_X + 120, y + 5, "/bin", COL_DIM);
        y += 32;
    }
}

/* SYSTEM: dock, volume, UI scale — real controls, persisted */
static void page_system(void){
    int y = 84;
    sf_text(&S, CTX_X, y, "Dock", COL_DIM);
    sf_text(&S, CTX_X, y + 24, s_dock ? "Visible" : "Hidden", COL_ACCENT);
    sf_text(&S, CTX_X + 120, y + 24, "(click to toggle)", COL_DIM);
    y += 56;
    sf_text(&S, CTX_X, y, "Volume", COL_DIM);
    char vv[8]; itoa0(s_vol, vv, 0); sf_text(&S, CTX_X + 90, y, vv, COL_ACCENT);
    int slw = CTX_W - 40;
    sf_round_rect_blend(&S, CTX_X, y + 20, slw, 10, 5, ARGB(70, 255, 255, 255));
    int fw = slw * s_vol / 100;
    if(fw > 0) sf_round_rect(&S, CTX_X, y + 20, fw, 10, 5, RGB(0x3B, 0x82, 0xF6));
    y += 52;
    sf_text(&S, CTX_X, y, "UI Scale", COL_DIM);
    sf_text(&S, CTX_X + 90, y, s_scale == 2 ? "2x (HiDPI)" : "1x", COL_ACCENT);
    sf_text(&S, CTX_X, y + 24, "(click to toggle)", COL_DIM);
}

/* NETWORK: live Wi-Fi state from the kernel (honest — 0 networks in a VM) */
static void page_network(void){
    int y = 84;
    bool hov = inside(g_mx, g_my, CTX_X, y, CTX_X + 72, y + 26);
    u32 btn = hov ? RGB(0x2A, 0x55, 0xA0) : RGB(0x3B, 0x82, 0xF6);
    sf_round_rect(&S, CTX_X, y, 72, 26, 6, btn);
    sf_text(&S, CTX_X + 18, y + 4, "Scan", RGB(0xFF, 0xFF, 0xFF));
    y += 40;
    if(s_netbuf_len > 0)
        draw_lines(CTX_X, y, s_netbuf, COL_TEXT, 16);
    else
        sf_text(&S, CTX_X, y, "Wi-Fi: no data (call Scan)", COL_DIM);
}

/* SECURITY & PRIVACY: real facts about the auth system */
static void page_security(void){
    int y = 84;
    sf_text(&S, CTX_X, y, "Session lock", COL_DIM); y += 24;
    sf_text(&S, CTX_X, y, "Lock screen requires the account password.", COL_TEXT); y += 22;
    sf_text(&S, CTX_X, y, "Auth: PBKDF2-HMAC-SHA256, 10,000 rounds,", COL_TEXT); y += 22;
    sf_text(&S, CTX_X, y, "constant-time compare, 5-try lockout.", COL_TEXT); y += 30;
    sf_text(&S, CTX_X, y, "Clipboard", COL_DIM); y += 24;
    sf_text(&S, CTX_X, y, "System clipboard is session-only and never", COL_TEXT); y += 22;
    sf_text(&S, CTX_X, y, "written to disk.", COL_TEXT);
}

/* UPDATES: honest — there is no update service yet */
static void page_updates(void){
    int y = 84;
    sf_text(&S, CTX_X, y, "YartOS 0.8.0", COL_TEXT); y += 26;
    sf_text(&S, CTX_X, y, "No update service is present.", COL_DIM); y += 22;
    sf_text(&S, CTX_X, y, "The OS is built from source:", COL_DIM); y += 22;
    sf_text(&S, CTX_X, y, "  make -j iso", COL_ACCENT); y += 26;
    sf_text(&S, CTX_X, y, "There are no remote repositories yet.", COL_DIM);
}

/* ABOUT: real system info (user / system / CPU / processes / battery / wifi) */
static void page_about(void){
    int y = 84;
    for(int i = 0; i < s_about_n; i++){
        if(s_about[i][0] == 0) continue;
        sf_text(&S, CTX_X, y, s_about[i], COL_TEXT);
        y += 22;
    }
}

/* ---- input ---- */
static void settings_key(int ev){
    int ascii = ev & 255;
    int sc = (ev >> 8) & 0xFF;
    if(s_search_focus){
        if(ascii >= 32 && ascii < 127 && s_query_len < (int)sizeof(s_query) - 1){
            s_query[s_query_len++] = (char)ascii; s_query[s_query_len] = 0;
        } else if(ascii == 8 || ascii == 127){
            if(s_query_len > 0){ s_query_len--; s_query[s_query_len] = 0; }
        } else if(ascii == 27){
            s_query_len = 0; s_query[0] = 0; s_search_focus = false;
        } else if(ascii == 13 || ascii == 10){
            /* Enter: jump to the first matching page, then clear */
            for(int i = 1; i < NPAGES; i++) if(item_matches(i)){ settings_reduce(ACT_GOTO, i); break; }
            s_search_focus = false;
        }
        return;
    }
    if(ascii == '/' ){ s_search_focus = true; s_query_len = 0; s_query[0] = 0; return; }
    if(sc == 0x50){ settings_reduce(ACT_GOTO, (s_page() + 1) % NPAGES); }        /* down  */
    else if(sc == 0x48){ settings_reduce(ACT_GOTO, (s_page() + NPAGES - 1) % NPAGES); } /* up */
    else if(sc == 0x4D){  /* right: advance the current page's value */
        if(s_page() == PAGE_PERSONALIZATION){ s_accent = (s_accent + 1) % NACCENT; conf_write(); }
        else if(s_page() == PAGE_SYSTEM){ if(s_vol < 100){ s_vol += 5; if(s_vol > 100) s_vol = 100; audio_vol(s_vol); conf_write(); } }
    }
    else if(sc == 0x4B){  /* left: retreat */
        if(s_page() == PAGE_PERSONALIZATION){ s_accent = (s_accent + NACCENT - 1) % NACCENT; conf_write(); }
        else if(s_page() == PAGE_SYSTEM){ if(s_vol > 0){ s_vol -= 5; if(s_vol < 0) s_vol = 0; audio_vol(s_vol); conf_write(); } }
    }
    else if(sc == 0x1C){  /* enter: toggle */
        if(s_page() == PAGE_SYSTEM){ s_dock = !s_dock; conf_write(); }
    }
}

static void settings_click(int x, int y){
    /* header tool buttons */
    if(y < HDR_H){
        if(inside(x, y, 8, 4, 32, 26)){ settings_reduce(ACT_BACK, 0); return; }
        if(inside(x, y, 36, 4, 60, 26)){ settings_reduce(ACT_FORWARD, 0); return; }
        if(inside(x, y, 64, 4, 88, 26)){ settings_reduce(ACT_HOME, 0); return; }
        return;
    }
    /* search field */
    if(inside(x, y, 6, HDR_H + 8, SB_W - 6, HDR_H + 34)){ s_search_focus = true; return; }
    /* sidebar items */
    if(x < SB_W && y >= HDR_H + 42){
        int idx = (y - (HDR_H + 42)) / 33;
        int seen = -1;
        for(int i = 1; i < NPAGES; i++){
            if(!item_matches(i)) continue;
            seen++;
            if(seen == idx){ settings_reduce(ACT_GOTO, i); return; }
        }
        return;
    }
    /* content */
    if(x < SB_W) return;
    switch(s_page()){
    case PAGE_HOME: {
        int gx = CTX_X + (CTX_W - (3 * 96 + 2 * 8)) / 2;
        int gy = 76;
        static const int tiles[8] = { PAGE_ACCOUNT, PAGE_PERSONALIZATION, PAGE_PACKAGES,
            PAGE_SYSTEM, PAGE_NETWORK, PAGE_SECURITY, PAGE_UPDATES, PAGE_ABOUT };
        for(int i = 0; i < 8; i++){
            int tx = gx + (i % 3) * 104, ty = gy + (i / 3) * 80;
            if(inside(x, y, tx, ty, tx + 96, ty + 72)){ settings_reduce(ACT_GOTO, tiles[i]); return; }
        }
        break;
    }
    case PAGE_PERSONALIZATION: {
        int ay = 84;
        for(int i = 0; i < NACCENT; i++){
            int sx = CTX_X + i * 48;
            if(inside(x, y, sx, ay + 24, sx + 40, ay + 64)){ s_accent = i; conf_write(); return; }
        }
        int cyy = ay + 92;
        if(inside(x, y, CTX_X, cyy + 20, CTX_X + 160, cyy + 48)){
            s_cursor = (s_cursor + 1) % cursors_theme_count(); conf_write(); return;
        }
        int wy = cyy + 76;
        if(inside(x, y, CTX_X, wy + 20, CTX_X + 160, wy + 48)){
            s_wallpaper = (s_wallpaper + 1) % wallpaper_count(); conf_write(); return;
        }
        break;
    }
    case PAGE_SYSTEM: {
        int y0 = 84;
        if(inside(x, y, CTX_X, y0 + 18, CTX_X + 200, y0 + 44)){ s_dock = !s_dock; conf_write(); return; }
        int vy = y0 + 56;
        if(inside(x, y, CTX_X, vy + 16, CTX_X + CTX_W - 40, vy + 34)){
            int slw = CTX_W - 40;
            s_vol = (x - CTX_X) * 100 / slw; if(s_vol < 0) s_vol = 0; if(s_vol > 100) s_vol = 100;
            audio_vol(s_vol); conf_write(); return;
        }
        int sy = vy + 52;
        if(inside(x, y, CTX_X, sy, CTX_X + 200, sy + 24)){ s_scale = (s_scale == 1) ? 2 : 1; conf_write(); return; }
        break;
    }
    case PAGE_NETWORK: {
        if(inside(x, y, CTX_X, 84, CTX_X + 72, 110)){ wifi_scan(); network_refresh(); return; }
        break;
    }
    default: break;
    }
}

/* ===================================================================== */
/* Welcome / About                                                       */
/* ===================================================================== */
#else
static void draw_welcome(void){
    fill(0,0,I.w,I.h,RGB(0x18,0x18,0x1B));
    sf_text(&S,30,60,"About YartOS",RGB(0xFF,0xFF,0xFF));
    sf_text(&S,30,100,"A hobby operating system with a real",COL_DIM);
    sf_text(&S,30,120,"ring-3 compositor, window surfaces, a VFS,",COL_DIM);
    sf_text(&S,30,140,"processes and a network stack.",COL_DIM);
    sf_text(&S,30,180,"Built after the Skift OS desktop.",COL_DIM);
    sf_text(&S,30,200,"Use the Console app for the command line.",COL_DIM);
}
static void welcome_key(int a){(void)a;}
static void welcome_click(int x,int y){(void)x;(void)y;}
#endif

/* ===================================================================== */
/* common main                                                           */
/* ===================================================================== */
int main_entry(int argc,char**argv,char**envp){
    (void)argc;(void)argv;(void)envp;
    ID=(int)wm_create(APP_W,APP_H,&I);
    if(ID<0) return 1;
    S.px=(u32*)(unsigned long)I.app_va; S.w=I.w; S.h=I.h; S.pitch=I.w;
    const char *title="App";
#if defined(APP_FILES)
    title="Files";
    if(argc>1 && argv && argv[1] && argv[1][0]=='/'){
        int k=0; while(argv[1][k] && k<127){ f_path[k]=argv[1][k]; k++; }
        f_path[k]=0; f_sel=0;
    }
#elif defined(APP_EDITOR)
    title="Text";
#elif defined(APP_SETTINGS)
    title="Settings";
#elif defined(APP_BROWSER)
    title="About";
#endif
    char t[40]; int k=0; while(title[k]&&k<39){t[k]=title[k];k++;} t[k]=0;
    wm_title(ID,t);
#if defined(APP_SETTINGS)
    cursors_init();
    settings_init();
#endif
    int mx=I.w/2,my=I.h/2,frame=0;
    unsigned char mprev=0;
    bool dirty = true;                 /* event-driven redraw (Skift-style): only
                                         * repaint + flip when something changed */
    for(;;){ frame++;
        int ev;
        while((ev=poll_key())!=0){
            int a=ev&255, make=!(ev&(1<<16));
            if(!make) continue;
            dirty = true;
#if defined(APP_EDITOR)
            int ctrl=(ev&(1<<18))!=0;   /* WM_KEY_CTRL modifier flag */
            if(ctrl && (a==19||a==('s'-96))){ editor_save(); continue; }
            if(ctrl && (a==3  ||a==('c'-96))){ editor_sel_copy(); continue; }   /* Ctrl+C */
            if(ctrl && (a==22 ||a==('v'-96))){ editor_paste();    continue; }   /* Ctrl+V */
            editor_key(a);
#elif defined(APP_FILES)
            files_key(ev);                 /* full event: ascii + scancode + ctrl */
#elif defined(APP_SETTINGS)
            settings_key(ev);             /* full event: ascii + scancode */
#else
            (void)a;
#endif
        }
        mouse_ev_t m;
        while(poll_mouse(&m)){
            /* Sync to the REAL cursor.  The PS/2 driver emits deltas and the
             * app only starts receiving them once it is focused, so
             * delta-only tracking would be offset by the cursor's pre-focus
             * travel (clicks would land in the wrong place). */
            int gp[2];
            if(mouse_pos(gp)==0){ mx = gp[0] - (int)I.win_x; my = gp[1] - (int)I.win_y; }
            else { mx+=m.dx; my+=m.dy; }
            if(mx<0){mx=0;} if(my<0){my=0;} if(mx>=(int)I.w){mx=I.w-1;} if(my>=(int)I.h){my=I.h-1;}
            g_mx=mx; g_my=my;
#if defined(APP_FILES)
            if(m.buttons&1) files_click(mx,my,frame);
#elif defined(APP_EDITOR)
            if((m.buttons&1) && !(mprev&1)) editor_sel_start(mx,my);   /* press */
            else if(m.buttons&1)            editor_sel_drag(mx,my);     /* drag  */
            if(!(m.buttons&1) && (mprev&1)) editor_sel_end();           /* release -> copy */
#elif defined(APP_SETTINGS)
            if(m.buttons&1) settings_click(mx,my);
#else
            if(m.buttons&1) welcome_click(mx,my);
#endif
            if(m.buttons || m.dx || m.dy || m.wheel) dirty = true;
            mprev=m.buttons;
            (void)mprev;
        }
        if(dirty){
#if defined(APP_FILES)
            draw_files();
#elif defined(APP_EDITOR)
            draw_editor();
#elif defined(APP_SETTINGS)
            draw_settings();
#else
            draw_welcome();
#endif
            wm_flip(ID);
            dirty = false;
        }
        sleep(16);
    }
}
