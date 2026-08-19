/* Nyra Terminal - a REAL VT100/ANSI terminal emulator (reliable edition).
 *
 * Cell grid (char + 16-colour fg + bg) laid out at the font's true metrics
 * (FONT_W x FONT_H = 12x18): the UI font is proportional (glyphs 3..12px
 * wide), so cells MUST be 12px or glyphs overlap.  The grid is sized from
 * the surface at runtime.
 *
 *  - VT100/ANSI parser: SGR colours (bold -> bright), CSI cursor movement
 *    (CUU/CUD/CUF/CUB/CUP), ED/EL erase, DEC ?25l/h cursor, RIS reset.
 *  - Wrap-aware readline-style editing: left/right/home/end, insert/delete,
 *    backspace, Ctrl+A/E/U/K, Ctrl+C cancel, Ctrl+L clear, Ctrl+D exit.
 *  - 512-row scrollback ring + mouse-wheel scrolling.
 *  - drag-to-select = copy, click = paste.
 */

#include "sys.h"
#include "gfx.h"
#include "apk.h"

#define WIN_W 640
#define WIN_H 400
#define PROMPT " $ "

#define MAX_COLS 128
#define MAX_ROWS 40
#define SB_ROWS  512
#define LINE_LEN 200
#define T_PAD_X 4
#define T_PAD_Y 4

static surface_t G_surf;
static wm_surf_info_t G_info;
static int G_win_id;

/* ---- terminal grid ---- */
typedef struct { char ch; u8 fg; u8 bg; } tcell_t;
static tcell_t G_grid[MAX_ROWS][MAX_COLS];
static tcell_t G_sb[SB_ROWS][MAX_COLS];
static int T_COLS = 128, T_ROWS = 24;
static int G_usable_px = 632;             /* width available for text (px)  */
static int G_sb_count = 0;
static int G_cur_r = 0, G_cur_c = 0;      /* grid cursor (where next char goes) */
static u8  G_fg = 7, G_bg = 0;
static bool G_bold = false, G_cursor_on = true;
static int  G_view_scroll = 0;            /* 0 = live, >0 = scrolled back */

static const u32 ANSI16[16] = {
    RGB(0x0C,0x0C,0x0E), RGB(0xE0,0x5B,0x5B), RGB(0x22,0xC5,0x5E),
    RGB(0xE0,0xB0,0x5B), RGB(0x60,0xA5,0xFA), RGB(0xC0,0x7E,0xE0),
    RGB(0x22,0xC5,0xC5), RGB(0xE5,0xE5,0xE5), RGB(0x5A,0x5A,0x5E),
    RGB(0xFF,0x7B,0x7B), RGB(0x7C,0xFF,0x9E), RGB(0xFF,0xE0,0x7B),
    RGB(0x8E,0xC6,0xFF), RGB(0xFF,0x9E,0xFF), RGB(0x7B,0xFF,0xFF),
    RGB(0xFF,0xFF,0xFF),
};

/* ---- command line (the prompt lives IN the grid) ---- */
static char G_cmd[LINE_LEN];
static int  G_cmd_len = 0;
static int  G_edit = 0;                   /* edit cursor index into G_cmd  */
static int  G_prompt_row = 0;             /* grid row where the prompt is  */
static int  G_prompt_rows = 1;            /* rows the prompt occupies       */
static int  G_cursor_blink = 0;

#define MAX_HISTORY 64
static char G_history[MAX_HISTORY][LINE_LEN];
static int G_hist_count = 0;
static int G_hist_pos = 0;

/* ---- job control (background jobs) ---- */
#define MAX_JOBS 16
typedef struct { long pid; char cmd[LINE_LEN]; bool stopped; } job_t;
static job_t G_jobs[MAX_JOBS];
static int G_job_count = 0;
static long G_fg_pid = -1;            /* foreground job, -1 = none */

/* ---- POSIX shell state ---- */
#define MAX_ENV 16
static struct { char name[32]; char val[96]; } G_env[MAX_ENV];
static int G_env_count = 0;
static int G_status = 0;              /* $? - last command status        */

static bool G_capturing = false;      /* command output -> capture buffer */
static char G_capture[16384];
static int  G_cap_len = 0;
static bool G_pipe_input = false;     /* next cmd's `cat` reads this      */
static char G_pipebuf[16384];
static int  G_pipebuf_len = 0;

static void put_dec(long v, char *out);
static void add_line(const char *s);
static void term_write(const char *s);
static void *xmemmove(void *d, const void *s, size_t n);
static void run_one(const char *cmd);
static void run_segment(char *cmd);
static char *xstrchr(const char *s, char c){
    for(; *s; s++) if(*s==c) return (char*)s;
    return NULL;
}

static const char *env_get(const char *name){
    for(int i=0;i<G_env_count;i++)
        if(strcmp(G_env[i].name, name)==0) return G_env[i].val;
    return "";
}
static void env_set(const char *name, const char *val){
    for(int i=0;i<G_env_count;i++)
        if(strcmp(G_env[i].name, name)==0){ strncpy(G_env[i].val, val, 95); G_env[i].val[95]=0; return; }
    if(G_env_count < MAX_ENV){
        strncpy(G_env[G_env_count].name, name, 31); G_env[G_env_count].name[31]=0;
        strncpy(G_env[G_env_count].val, val, 95);   G_env[G_env_count].val[95]=0;
        G_env_count++;
    }
}
static void env_unset(const char *name){
    for(int i=0;i<G_env_count;i++)
        if(strcmp(G_env[i].name, name)==0){
            for(int j=i;j<G_env_count-1;j++) G_env[j]=G_env[j+1];
            G_env_count--; return;
        }
}
static void env_init(void){
    G_env_count=0;
    env_set("HOME","/home/yart");
    env_set("USER","demo");
    env_set("TERM","nyra");
    env_set("PATH","/bin");
    env_set("SHELL","/bin/nyra");
    env_set("PWD","/home/yart");
}
/* append `s` to the capture buffer, stripping ANSI escape sequences */
static void cap_append(const char *s){
    for(int i=0;s[i] && G_cap_len < (int)sizeof(G_capture)-2;i++){
        if(s[i]==0x1B){
            i++;
            while(s[i] && !(s[i]>='@' && s[i]<='~')) i++;
            continue;                      /* drop the whole CSI/ESC seq */
        }
        G_capture[G_cap_len++]=s[i];
    }
}
/* output a chunk of text: capture (pipes/redirection) or the grid */
static void shell_write(const char *s){
    if(G_capturing) cap_append(s);
    else term_write(s);
}

/* ---- $VAR / $? expansion with single + double quotes ---- */
static int expand_var(const char *in, int i, char *out, int *o, int cap){
    i++;                                   /* skip '$' */
    if(in[i]=='?'){
        char b[8]; put_dec((long)G_status, b);
        for(char *p=b; *p && *o<cap-1; p++) out[(*o)++]=*p;
        return i+1;
    }
    char name[32]; int n=0;
    while((in[i]>='A'&&in[i]<='Z')||(in[i]>='a'&&in[i]<='z')||
          (in[i]>='0'&&in[i]<='9')||in[i]=='_'){
        if(n<31) name[n++]=in[i];
        i++;
    }
    name[n]=0;
    const char *v = (n>0) ? env_get(name) : "";
    for(const char *p=v; *p && *o<cap-1; p++) out[(*o)++]=*p;
    return i;
}
static void expand(const char *in, char *out, int cap){
    int i=0, o=0; char q=0;
    while(in[i] && o<cap-1){
        char c=in[i];
        if(q=='\''){
            if(c=='\'') q=0; else out[o++]=c;
            i++; continue;
        }
        if(q=='"'){
            if(c=='"') q=0;
            else if(c=='$'){ i=expand_var(in,i,out,&o,cap); continue; }
            else out[o++]=c;
            i++; continue;
        }
        if(c=='\'' || c=='"'){ q=c; i++; continue; }
        if(c=='$'){ i=expand_var(in,i,out,&o,cap); continue; }
        out[o++]=c; i++;
    }
    out[o]=0;
}
static void trim(char *s){
    int l=strlen(s);
    while(l>0 && (s[l-1]==' '||s[l-1]=='\t')) s[--l]=0;
    char *p=s; while(*p==' '||*p=='\t') p++;
    if(p!=s) xmemmove(s,p,strlen(p)+1);
}

static int job_add(long pid, const char *cmd){
    if(G_job_count >= MAX_JOBS) return -1;
    G_jobs[G_job_count].pid = pid;
    strncpy(G_jobs[G_job_count].cmd, cmd, LINE_LEN-1);
    G_jobs[G_job_count].stopped = false;
    G_job_count++;
    return G_job_count;                 /* 1-based job number */
}
static int job_find(long pid){
    for(int i=0;i<G_job_count;i++) if(G_jobs[i].pid==pid) return i;
    return -1;
}
static void job_remove(int idx){
    if(idx<0||idx>=G_job_count) return;
    for(int i=idx;i<G_job_count-1;i++) G_jobs[i]=G_jobs[i+1];
    G_job_count--;
}
static void job_status_line(int idx, const char *state){
    char line[LINE_LEN+16];
    int k=0;
    char nb[8]; put_dec((long)(idx+1), nb);
    const char *pre="["; while(*pre) line[k++]=*pre++;
    for(int i=0;nb[i];i++) line[k++]=nb[i];
    const char *post="]  "; while(*post) line[k++]=*post++;
    for(const char *p=state;*p && k<LINE_LEN-2;p++) line[k++]=*p;
    line[k++]=' ';
    for(const char *p=G_jobs[idx].cmd;*p && k<LINE_LEN-2;p++) line[k++]=*p;
    line[k]=0;
    add_line(line);
}

static int  G_sel_anchor = -1, G_sel_end = -1;
static bool G_sel_active = false, G_btn_left = false, G_did_drag = false;

static void put_dec(long v, char *out){
    char tmp[24]; int i=0; if(v==0) tmp[i++]='0'; while(v>0){ tmp[i++]=(char)('0'+v%10); v/=10; } int j=0; while(i) out[j++]=tmp[--i]; out[j]=0;
}
static void *xmemmove(void *d, const void *s, size_t n){
    u8 *dd=d; const u8 *ss=s;
    if(dd < ss){ for(size_t i=0;i<n;i++) dd[i]=ss[i]; }
    else if(dd > ss){ for(size_t i=n;i>0;i--) dd[i-1]=ss[i-1]; }
    return d;
}

/* ---------------- grid primitives ---------------- */
static void cell_set(int r, int c, char ch, u8 fg, u8 bg){
    if(r<0||r>=T_ROWS||c<0||c>=T_COLS) return;
    G_grid[r][c].ch=ch; G_grid[r][c].fg=fg; G_grid[r][c].bg=bg;
}
static void row_copy(tcell_t *dst, const tcell_t *src){
    for(int c=0;c<T_COLS;c++) dst[c]=src[c];
}
static int G_scrolled = 0;      /* total scroll_up() calls (for prompt tracking) */
static void scroll_up(void){
    G_scrolled++;
    if(G_sb_count < SB_ROWS) G_sb_count++;
    else { for(int i=1;i<SB_ROWS;i++) row_copy(G_sb[i-1], G_sb[i]); }
    row_copy(G_sb[G_sb_count-1], G_grid[0]);
    for(int r=1;r<T_ROWS;r++) row_copy(G_grid[r-1], G_grid[r]);
    for(int c=0;c<T_COLS;c++) cell_set(T_ROWS-1, c, ' ', G_fg, G_bg);
}
static void term_clear_all(void){
    for(int r=0;r<T_ROWS;r++) for(int c=0;c<T_COLS;c++) cell_set(r,c,' ',G_fg,G_bg);
    G_cur_r=0; G_cur_c=0;
}
static void term_clear_eol(void){
    for(int c=G_cur_c;c<T_COLS;c++) cell_set(G_cur_r,c,' ',G_fg,G_bg);
}
static void term_clear_bol(void){
    for(int c=0;c<=G_cur_c;c++) cell_set(G_cur_r,c,' ',G_fg,G_bg);
}
static void term_newline(void){
    G_cur_c=0;
    if(G_cur_r+1 >= T_ROWS){ scroll_up(); }
    else G_cur_r++;
}

/* ---- raw char into the grid (control chars only) ----
 * The font is PROPORTIONAL (like the rest of the OS), so wrapping is by
 * pixel width: a char wraps when the row's rendered width + its width would
 * exceed the usable area.  Cells store the character (not a fixed-width
 * slot); rendering advances by each glyph's real width via sf_text. */
static int row_px_upto(int r, int c){
    int px = 0;
    for(int i=0;i<c;i++){
        char ch = G_grid[r][i].ch;
        if(!ch) ch = ' ';
        px += sf_char_width(ch);
    }
    return px;
}
static void term_put(char c){
    if(c=='\n'){ term_newline(); return; }
    if(c=='\r'){ G_cur_c=0; return; }
    if(c=='\b'){ if(G_cur_c>0) G_cur_c--; return; }
    if(c=='\t'){ do { if(G_cur_c>=T_COLS-1) term_newline(); cell_set(G_cur_r,G_cur_c,' ',G_fg,G_bg); G_cur_c++; } while((row_px_upto(G_cur_r,G_cur_c)) % 32 != 0 && G_cur_c < T_COLS); return; }
    if(c < 0x20 || c == 0x7F) return;
    u8 fg = (u8)(G_fg + (G_bold ? 8 : 0));
    if(fg > 15) fg = 15;
    /* wrap when this glyph would overflow the usable width */
    if(G_cur_c >= T_COLS || row_px_upto(G_cur_r, G_cur_c) + sf_char_width(c) > G_usable_px){
        term_newline();
    }
    cell_set(G_cur_r, G_cur_c, c, fg, G_bg);
    G_cur_c++;
}

/* ---- VT100 parser ---- */
static void csi_dispatch(const char *p, int n, char final){
    int params[8]; int np=0; params[np]=0;
    bool priv=false;
    for(int i=0;i<n;i++){
        char ch=p[i];
        if(ch=='?'){ priv=true; continue; }
        if(ch>='0'&&ch<='9'){ params[np]=params[np]*10+(ch-'0'); }
        else if(ch==';'){ if(np<7){ np++; params[np]=0; } }
    }
    int a=params[0];
    switch(final){
    case 'm':
        for(int i=0;i<=np;i++){
            int v=params[i];
            if(v==0){ G_fg=7; G_bg=0; G_bold=false; }
            else if(v==1) G_bold=true;
            else if(v==22) G_bold=false;
            else if(v>=30&&v<=37) G_fg=(u8)(v-30);
            else if(v>=90&&v<=97) G_fg=(u8)(v-90+8);
            else if(v>=40&&v<=47) G_bg=(u8)(v-40);
            else if(v>=100&&v<=107) G_bg=(u8)(v-100+8);
        }
        break;
    case 'H': case 'f':
        { int r=(a>0?a:1)-1; int c=(np>0&&params[1]>0?params[1]:1)-1;
          if(r<0) r=0;
          if(r>=T_ROWS) r=T_ROWS-1;
          if(c<0) c=0;
          if(c>=T_COLS) c=T_COLS-1;
          G_cur_r=r; G_cur_c=c; }
        break;
    case 'J':
        if(a==2) term_clear_all();
        else if(a==1){ for(int r=0;r<G_cur_r;r++) for(int c=0;c<T_COLS;c++) cell_set(r,c,' ',G_fg,G_bg); term_clear_bol(); }
        else { term_clear_eol(); for(int r=G_cur_r+1;r<T_ROWS;r++) for(int c=0;c<T_COLS;c++) cell_set(r,c,' ',G_fg,G_bg); }
        break;
    case 'K':
        if(a==2){ G_cur_c=0; term_clear_eol(); }
        else if(a==1) term_clear_bol();
        else term_clear_eol();
        break;
    case 'A': { int k=a?a:1; G_cur_r-=k; if(G_cur_r<0) G_cur_r=0; } break;
    case 'B': { int k=a?a:1; G_cur_r+=k; if(G_cur_r>=T_ROWS) G_cur_r=T_ROWS-1; } break;
    case 'C': { int k=a?a:1; G_cur_c+=k; if(G_cur_c>=T_COLS) G_cur_c=T_COLS-1; } break;
    case 'D': { int k=a?a:1; G_cur_c-=k; if(G_cur_c<0) G_cur_c=0; } break;
    case 'l': if(priv && a==25) G_cursor_on=false; break;
    case 'h': if(priv && a==25) G_cursor_on=true;  break;
    default: break;
    }
}

static void term_write(const char *s){
    if(!s) return;
    for(int i=0; s[i]; i++){
        if(s[i]==0x1B){
            if(s[i+1]=='['){
                int j=i+2; char params[32]; int pn=0;
                while(s[j] && !((s[j]>='@'&&s[j]<='~'))){ if(pn<31) params[pn++]=s[j]; j++; }
                char final=s[j]; if(!final) return;
                params[pn]=0;
                csi_dispatch(params, pn, final);
                i=j;
            } else if(s[i+1]=='c'){ term_clear_all(); G_fg=7; G_bg=0; G_bold=false; i++; }
            else if(s[i+1]=='7'){ i++; }
            else if(s[i+1]=='8'){ i++; }
        } else term_put(s[i]);
    }
}

static void add_line(const char *s){
    if(G_capturing){
        cap_append(s);
        if(G_cap_len < (int)sizeof(G_capture)-2) G_capture[G_cap_len++]='\n';
        return;
    }
    term_write(s); term_put('\n');
}

/* ---------------- prompt + line editing ---------------- */
/* Cursor pixel position for edit index `idx` (chars into the command).
 * Walks the command with real glyph widths and wraps at G_usable_px. */
static void edit_cursor_px(int idx, int *px, int *py){
    int x = T_PAD_X + sf_text_width(PROMPT);
    int y = G_prompt_row * FONT_H + T_PAD_Y;
    for(int i=0;i<idx && i<G_cmd_len;i++){
        int w = sf_char_width(G_cmd[i]);
        if(x + w > T_PAD_X + G_usable_px){ x = T_PAD_X; y += FONT_H; }
        x += w;
    }
    *px = x; *py = y;
}
static void line_render(void){
    int start=G_prompt_row, prev=G_prompt_rows;
    int sc0=G_scrolled;
    G_cur_r=start; G_cur_c=0;
    G_fg=2; G_bold=false;
    /* prompt = current directory + " $ " (like a real shell), so `cd` is
     * visibly reflected in the prompt. */
    char cwd[128]; long cr=_sc(SYS_GETCWD, (long)cwd, 128, 0);
    const char *ps = (cr>0 && cwd[0]) ? cwd : "/";
    for(const char *p=ps; *p; p++) term_put(*p);
    for(const char *p=PROMPT; *p; p++) term_put(*p);
    G_fg=7;
    for(int i=0;i<G_cmd_len;i++) term_put(G_cmd[i]);
    term_clear_eol();
    int sc1=G_scrolled;
    G_prompt_row = start - (sc1-sc0);   /* the prompt scrolled up with the grid */
    int rows=G_cur_r-G_prompt_row+1;
    for(int r=G_prompt_row+rows; r<start+prev; r++)
        for(int c=0;c<T_COLS;c++) cell_set(r,c,' ',7,0);
    G_prompt_rows=rows;
}
static void prompt_print(void){
    G_prompt_row=G_cur_r;
    G_edit=0; G_cmd_len=0; G_cmd[0]=0;
    line_render();
}
static void cmd_insert(char ch){
    if(G_cmd_len>=LINE_LEN-1) return;
    xmemmove(G_cmd+G_edit+1, G_cmd+G_edit, (size_t)(G_cmd_len-G_edit));
    G_cmd[G_edit]=ch; G_cmd_len++; G_edit++;
    line_render();
}
static void cmd_backspace(void){
    if(G_edit<=0) return;
    xmemmove(G_cmd+G_edit-1, G_cmd+G_edit, (size_t)(G_cmd_len-G_edit));
    G_cmd_len--; G_edit--;
    line_render();
}
static void cmd_delete(void){
    if(G_edit>=G_cmd_len) return;
    xmemmove(G_cmd+G_edit, G_cmd+G_edit+1, (size_t)(G_cmd_len-G_edit-1));
    G_cmd_len--;
    line_render();
}
static void cmd_cancel(void){
    /* print ^C, newline, fresh prompt */
    G_fg=1; term_put('^'); term_put('C'); G_fg=7;
    term_newline();
    prompt_print();
}
static void screen_clear_and_prompt(void){
    term_clear_all();
    G_cur_r=0; G_cur_c=0;
    G_prompt_row=0; G_prompt_rows=1;
    G_edit=0; G_cmd_len=0; G_cmd[0]=0;
    line_render();
}

/* ---------------- FS / shell commands ---------------- */
static void cmd_ls(const char *path){
    /* empty path = current directory (the kernel resolves "" to the cwd). */
    int fd=open(path, 0);
    if(fd<0){ add_line("ls: cannot open"); return; }
    while(1){
        struct { u32 type; u32 reclen; u64 size; char name[96]; } de;
        long n=_sc(SYS_GETDENTS, fd, (long)&de, 1);
        if(n<=0) break;
        char line[140]; int k=0;
        if(de.type==2){ const char*pre="\x1b[1;34m"; for(;*pre && k<130;pre++) line[k++]=*pre; }
        else          { const char*pre="\x1b[32m"; for(;*pre && k<130;pre++) line[k++]=*pre; }
        char sz[16]; put_dec((long)de.size, sz);
        for(int i=0;sz[i] && k<130;i++) line[k++]=sz[i];
        line[k++]=' ';
        for(int i=0;de.name[i] && k<130;i++) line[k++]=de.name[i];
        const char*res="\x1b[0m"; for(;*res && k<130;res++) line[k++]=*res;
        line[k]=0;
        add_line(line);
    }
    close(fd);
}
static void cmd_cat(const char *path){
    if(!path || !path[0] || strcmp(path,"-")==0){
        /* stdin: a piped buffer (cat at the end of a pipeline) */
        if(G_pipebuf_len>0){
            G_pipebuf[G_pipebuf_len]=0;
            shell_write(G_pipebuf);
            if(G_cur_c!=0 && !G_capturing) term_put('\n');
        }
        return;
    }
    int fd=open(path,0);
    if(fd<0){ add_line("cat: not found"); return; }
    char buf[1024];
    long n;
    while((n=read(fd, buf, sizeof buf-1))>0){
        buf[n]=0;
        shell_write(buf);
        if(G_sb_count > SB_ROWS-8) break;   /* stop before scrollback overflows */
    }
    /* newline if the file did not end with one */
    if(G_cur_c!=0 && !G_capturing) term_put('\n');
    close(fd);
}
static void cmd_echo(const char *msg){
    if(!msg) msg="";
    add_line(msg);
    /* the echo.txt side effect is skipped when piping/redirecting */
    if(G_capturing) return;
    int fd=open("/home/yart/echo.txt", 0x241);
    if(fd>=0){ write(fd, msg, strlen(msg)); write(fd, "\n",1); close(fd); }
}
static void cmd_ps(void){
    u32 pids[32]; long n=task_list(pids, 32);
    if(n<0){ add_line("ps: failed"); return; }
    char line[64];
    for(int i=0;i<n;i++){
        char nb[16]; put_dec(pids[i], nb);
        line[0]=0; int k=0; const char *pre="pid "; while(*pre) line[k++]=*pre++;
        for(int j=0;nb[j];j++){ line[k++]=nb[j]; } line[k]=0;
        add_line(line);
    }
}
static void cmd_wifi(const char *sub){
    if(!sub || strcmp(sub,"scan")==0){
        long cnt=wifi_scan(); char buf[32]; put_dec(cnt, buf);
        add_line("WiFi scan..."); char line[32]="APs: "; int k=5; for(int i=0;buf[i];i++) line[k++]=buf[i]; line[k]=0; add_line(line);
        char stat[512]; long n=wifi_status(stat, 512); if(n>0){ stat[n]=0; shell_write(stat); }
    } else if(strncmp(sub,"connect ",8)==0){
        const char *ssid=sub+8; const char *psk="";
        int k=0; while(ssid[k] && ssid[k]!=' ' && k<40) k++;
        if(ssid[k]==' '){ char *s=(char*)ssid; s[k]=0; psk=ssid+k+1; }
        if(!ssid[0]){ add_line("usage: wifi connect <ssid> <psk>"); return; }
        long r=wifi_connect(ssid, psk);
        if(r==0) add_line("WiFi connected (802.11 auth/assoc/EAPOL ok)");
        else add_line("WiFi connect failed (no radio in this machine, or bad PSK)");
    } else if(strcmp(sub,"status")==0){
        char stat[512]; long n=wifi_status(stat,512); if(n>0){ stat[n]=0; shell_write(stat); } else add_line("wifi status failed");
    } else if(strcmp(sub,"disconnect")==0){
        wifi_disconnect(); add_line("WiFi disconnected");
    } else {
        add_line("wifi: scan | status | connect <ssid> <psk> | disconnect");
    }
}
static void cmd_colors(void){
    for(int b=0;b<2;b++){
        for(int f=0;f<8;f++){
            char line[40]; int k=0;
            line[k++]='\x1b'; line[k++]='[';
            char c1[4]; put_dec(f+(b?90:30), c1);
            for(char *p=c1;*p;p++) line[k++]=*p;
            line[k++]='m';
            const char *nm=" ";
            if(f==0) nm="blk ";
            if(f==1) nm="red ";
            if(f==2) nm="grn ";
            if(f==3) nm="yel ";
            if(f==4) nm="blu ";
            if(f==5) nm="mag ";
            if(f==6) nm="cyn ";
            if(f==7) nm="wht ";
            for(const char *p=nm;*p;p++) line[k++]=*p;
            line[k++]='\x1b'; line[k++]='['; line[k++]='0'; line[k++]='m';
            line[k]=0;
            shell_write(line);
        }
        if(!G_capturing) term_put('\n');
    }
}

/* ---- run ONE segment: pipes | redirection < > >> , then a simple cmd ---- */
static void run_segment(char *cmd){
    trim(cmd);
    if(!cmd[0]) return;
    /* pipe: run the left side capturing output, feed it to the right side */
    char *pipe=NULL;
    for(char *p=cmd; *p; p++) if(*p=='|'){ pipe=p; break; }
    if(pipe){
        *pipe=0;
        G_capturing=true; G_cap_len=0;
        run_segment(cmd);
        G_capturing=false;
        if(G_cap_len >= (int)sizeof(G_pipebuf)) G_cap_len=sizeof(G_pipebuf)-1;
        memcpy(G_pipebuf, G_capture, (size_t)G_cap_len);
        G_pipebuf_len=G_cap_len;
        G_pipe_input=true;
        run_segment(pipe+1);
        G_pipe_input=false;
        return;
    }
    /* output redirection: > or >> (find the LAST one) */
    {
        char *gt=NULL; bool append=false;
        for(char *p=cmd; *p; p++){
            if(*p=='>'){
                if(p>cmd && *(p-1)=='>'){ gt=p-1; append=true; }
                else { gt=p; append=false; }
            }
        }
        if(gt){
            *gt=0;
            trim(cmd);                       /* drop the space before '>'  */
            char *file=gt+1;
            while(*file==' '||*file=='\t') file++;
            trim(file);
            if(!file[0]){ add_line("redirect: missing file"); return; }
            G_capturing=true; G_cap_len=0;
            run_one(cmd);
            G_capturing=false;
            int flags=O_WRONLY|O_CREAT|(append?0:O_TRUNC);
            int fd=open(file, flags);
            if(fd<0){ add_line("redirect: cannot open file"); return; }
            write(fd, G_capture, (size_t)G_cap_len);
            close(fd);
            G_status=0;
            return;
        }
    }
    /* input redirection: < file -> feeds `cat` / builtins that read stdin */
    {
        char *lt=NULL;
        for(char *p=cmd; *p; p++) if(*p=='<'){ lt=p; break; }
        if(lt){
            *lt=0;
            trim(cmd);
            char *file=lt+1;
            while(*file==' '||*file=='\t') file++;
            trim(file);
            if(!file[0]){ add_line("redirect: missing file"); return; }
            int fd=open(file, O_RDONLY);
            if(fd<0){ add_line("redirect: cannot open file"); return; }
            long n=read(fd, G_pipebuf, sizeof(G_pipebuf)-1);
            close(fd);
            if(n<0) n=0;
            G_pipebuf[n]=0; G_pipebuf_len=(int)n;
            G_pipe_input=true;
            run_one(cmd);
            G_pipe_input=false;
            return;
        }
    }
    run_one(cmd);
}

static void execute(const char *raw){
    if(!raw || !raw[0]) return;
    if(G_hist_count<MAX_HISTORY){ strncpy(G_history[G_hist_count], raw, LINE_LEN-1); G_hist_count++; }
    else { for(int i=1;i<MAX_HISTORY;i++) memcpy(G_history[i-1], G_history[i], LINE_LEN); strncpy(G_history[MAX_HISTORY-1], raw, LINE_LEN-1); }
    G_hist_pos=G_hist_count;
    G_status=0;
    /* expand $VAR / $? / quotes, then run each ';'-separated segment */
    char buf[LINE_LEN*3];
    expand(raw, buf, sizeof buf);
    char *seg=buf;
    while(*seg){
        char *end=seg;
        while(*end && *end!=';') end++;
        char saved=*end; *end=0;
        run_segment(seg);
        *end=saved;
        if(!*end) break;
        seg=end+1;
    }
}

/* ---- run one SIMPLE command (no pipes/redirection; already expanded) ---- */
static void run_one(const char *cmd){
    if(!cmd || !cmd[0]) return;

    if(strcmp(cmd,"help")==0){
        add_line("\x1b[1;36mConsole Help:\x1b[0m");
        add_line("  ls [path]      - list dir");
        add_line("  cd <path>      - change dir");
        add_line("  cat <file>     - show file");
        add_line("  echo <msg>     - echo");
        add_line("  mkdir <dir>    - make dir");
        add_line("  ln -s <t> <l>  - create symlink");
        add_line("  readlink <l>   - show symlink target");
        add_line("  rm <file>      - remove");
        add_line("  touch <file>   - create empty");
        add_line("  ps             - list PIDs");
        add_line("  kill <pid>     - kill process");
        add_line("  clear          - clear screen (Ctrl+L)");
        add_line("  colors         - show the 16 ANSI colours");
        add_line("  pwd            - print dir");
        add_line("  uptime         - show time");
        add_line("  net            - net info");
        add_line("  wifi scan|status|connect|disconnect");
        add_line("  passwd <old> <new>  - change the account password");
        add_line("  fsync          - flush FS to disk");
        add_line("  dmesg          - kernel log");
        add_line("  notify <msg>   - raise a desktop notification");
        add_line("  apk add <pkg>  - install a package (appears in the launcher)");
        add_line("  apk del <pkg>  - remove it   apk list / search / info");
        add_line("  linuxtest      - run a Linux binary through the Linux ABI");
        add_line("  linuxtest2     - Linux threads (clone+futex) + sockets + execve");
        add_line("  dynhello       - run a DYNAMICALLY-LINKED Linux program (.so)");
        add_line("  tlsdemo        - __thread vars through the dynamic linker + TLS");
        add_line("  ifuncdemo      - IFUNC (indirect function) resolution");
        add_line("  copydemo       - copy relocation (.so referencing a program global)");
        add_line("  tlstest        - verify TLS (arch_prctl ARCH_SET_FS -> %fs)");
        add_line("  reboot         - restart the machine");
        add_line("  exit           - close terminal (Ctrl+D)");
        add_line("  /bin/prog [&]  - run a program as a job");
        add_line("  jobs | fg [n] | bg [n]  - job control (Ctrl+Z stops fg job)");
        add_line("  export NAME=val | unset NAME | env");
        add_line("  cmd > file | cmd >> file | cmd < file | cmd1 | cmd2");
        add_line("  cmd1; cmd2     - run in sequence   $? - last status");
        add_line("Edit keys: Left/Right Home/End Del, Ctrl+A/E/U/K, Up/Down history");
    } else if(strcmp(cmd,"env")==0){
        for(int i=0;i<G_env_count;i++){
            char l[140]; int k=0;
            for(const char*p=G_env[i].name;*p;k++) l[k]=*p++;
            l[k++]='=';
            for(const char*p=G_env[i].val;*p;k++) l[k]=*p++;
            l[k]=0;
            add_line(l);
        }
    } else if(strncmp(cmd,"export ",7)==0){
        const char *a=cmd+7; char name[32]; int k=0;
        while(a[k] && a[k]!='=' && a[k]!=' ' && k<31){ name[k]=a[k]; k++; }
        name[k]=0;
        if(a[k]=='='){ env_set(name, a+k+1); G_status=0; }
        else { env_set(name, env_get(name)); G_status=0; }
    } else if(strncmp(cmd,"unset ",6)==0){
        env_unset(cmd+6); G_status=0;
    } else if(xstrchr(cmd,'=') && ((cmd[0]>='A'&&cmd[0]<='Z')||(cmd[0]>='a'&&cmd[0]<='z'))){
        /* VAR=value with no command: set it */
        char name[32]; int k=0;
        while(cmd[k] && cmd[k]!='=' && k<31){ name[k]=cmd[k]; k++; }
        name[k]=0;
        env_set(name, cmd+k+1); G_status=0;
    } else if(strcmp(cmd,"cd")==0){
        const char *h=env_get("HOME");
        if(_sc(SYS_CHDIR, (long)h,0,0)==0){ G_status=0; } else { add_line("cd: no HOME"); G_status=1; }
    } else if(strncmp(cmd,"ls",2)==0){
        /* `ls` with no argument lists the CURRENT directory ("" resolves to
         * the cwd in the kernel), not "/".  The old code defaulted to "/",
         * so after `cd /home/yart` the prompt still showed root - making `cd`
         * look broken. */
        const char *arg = cmd[2]==' ' ? cmd+3 : "";
        cmd_ls(arg);
    } else if(strncmp(cmd,"cd ",3)==0){
        if(_sc(SYS_CHDIR, (long)(cmd+3),0,0)==0){
            char pwd[128]; long r=_sc(SYS_GETCWD, (long)pwd, 128,0);
            if(r>0) env_set("PWD", pwd);
            G_status=0;
        } else { add_line("cd failed"); G_status=1; }
    } else if(strcmp(cmd,"pwd")==0){
        char buf[128]; long r=_sc(SYS_GETCWD, (long)buf, 128,0); if(r>0) add_line(buf); else add_line("/");
        G_status=0;
    } else if(strncmp(cmd,"cat ",4)==0){
        cmd_cat(cmd+4);
    } else if(strncmp(cmd,"echo ",5)==0){
        cmd_echo(cmd+5);
    } else if(strncmp(cmd,"mkdir ",6)==0){
        if(_sc(SYS_MKDIR, (long)(cmd+6),0,0)==0) add_line("mkdir ok"); else add_line("mkdir failed");
    } else if(strncmp(cmd,"rm ",3)==0){
        if(unlink(cmd+3)==0) add_line("removed"); else add_line("rm failed");
    } else if(strncmp(cmd,"ln -s ",6)==0){
        /* ln -s <target> <link> */
        const char *a=cmd+6; while(*a==' ')a++;
        const char *tgt=a; while(*tgt&&*tgt!=' ')tgt++;
        if(!*tgt){ add_line("usage: ln -s <target> <link>"); }
        else { char *s=(char*)tgt; *s=0; tgt=s+1; while(*tgt==' ')tgt++;
               if(symlink(a, tgt)==0) add_line("link created"); else add_line("ln -s failed"); }
    } else if(strncmp(cmd,"readlink ",9)==0){
        char buf[256];
        long n=readlink(cmd+9, buf, sizeof buf);
        if(n>0) add_line(buf); else add_line("readlink: not a symlink");
    } else if(strncmp(cmd,"touch ",6)==0){
        int fd=open(cmd+6, 0x40); if(fd>=0){ close(fd); add_line("touched"); } else add_line("touch failed");
    } else if(strcmp(cmd,"ps")==0){
        cmd_ps();
    } else if(strncmp(cmd,"kill ",5)==0){
        long pid=0; const char *p=cmd+5; while(*p>='0'&&*p<='9'){ pid=pid*10+(*p-'0'); p++; }
        if(kill(pid)==0) add_line("killed"); else add_line("kill failed");
    } else if(strcmp(cmd,"clear")==0){
        screen_clear_and_prompt();
    } else if(strcmp(cmd,"colors")==0){
        cmd_colors();
    } else if(strcmp(cmd,"uptime")==0){
        long ms=time_ms(); char buf[32]; put_dec(ms, buf); char line[48]="uptime ms: "; int k=11; for(int i=0;buf[i];i++) line[k++]=buf[i]; line[k]=0; add_line(line);
    } else if(strcmp(cmd,"net")==0){
        unsigned info[5]; if(net_info(info)==0){
            char line[64]; line[0]=0; int k=0; const char *pre="ip "; while(*pre) line[k++]=*pre++;
            char ipb[16]; put_dec((info[0]>>24)&255, ipb); for(int i=0;ipb[i];i++) line[k++]=ipb[i]; line[k++]='.'; put_dec((info[0]>>16)&255, ipb); for(int i=0;ipb[i];i++) line[k++]=ipb[i]; line[k++]='.'; put_dec((info[0]>>8)&255, ipb); for(int i=0;ipb[i];i++) line[k++]=ipb[i]; line[k++]='.'; put_dec(info[0]&255, ipb); for(int i=0;ipb[i];i++) line[k++]=ipb[i]; line[k]=0;
            add_line(line);
        } else add_line("net: no link");
    } else if(strncmp(cmd,"wifi",4)==0){
        const char *sub=cmd[4]==' ' ? cmd+5 : "";
        cmd_wifi(sub);
    } else if(strncmp(cmd,"passwd ",7)==0){
        const char *oldpw=cmd+7; const char *newpw="";
        int k=0; while(oldpw[k] && oldpw[k]!=' ' && k<40) k++;
        if(oldpw[k]==' '){ char *s=(char*)oldpw; s[k]=0; newpw=oldpw+k+1; }
        if(!oldpw[0] || !newpw[0]){ add_line("usage: passwd <old> <new>"); }
        else {
            long r=passwd(oldpw, newpw);
            if(r==0) add_line("password changed");
            else if(r==-2) add_line("passwd: new password too short (min 4)");
            else if(r==-3) add_line("passwd: wrong old password");
            else add_line("passwd: failed");
        }
    } else if(strcmp(cmd,"fsync")==0){
        long r=_sc(SYS_FSYNC, 0,0,0); if(r==0) add_line("fsync: flushed to disk"); else add_line("fsync failed");
    } else if(strcmp(cmd,"reboot")==0){
        add_line("rebooting..."); wm_flip(G_win_id); sleep(300);
        reboot(); add_line("reboot failed");
    } else if(strncmp(cmd,"notify ",7)==0){
        if(notify(cmd+7)==0) add_line("notification sent"); else add_line("notify failed");
    } else if(strcmp(cmd,"dmesg")==0){
        char buf[20*257]; long n=dmesg(buf, 0, 20); if(n>0){ for(int i=0;i<n;i++){ char *ln=&buf[i*257]; add_line(ln); } } else add_line("dmesg empty");
    } else if(strncmp(cmd,"apk",3)==0 || strncmp(cmd,"pkg",3)==0){
        const char *args = cmd[3]==' ' ? cmd+4 : "";
        char tmp[LINE_LEN]; strncpy(tmp, args, LINE_LEN-1); tmp[LINE_LEN-1]=0;
        char *argv[16]; int argc=1;
        argv[0]="apk";
        char *p=tmp;
        while(*p && argc<15){ while(*p==' ')p++; if(!*p)break; argv[argc++]=p; while(*p&&*p!=' ')p++; if(*p){*p=0;p++;} }
        argv[argc]=0;
        apk_main(argc, argv, add_line);
    } else if(strcmp(cmd,"linuxtest")==0){
        /* exec a genuine Linux static binary through the Linux-ABI layer */
        long pid=fork();
        if(pid==0){
            char *argv[]={"/bin/test_linux",0};
            char *envp[]={"HOME=/home/yart","TERM=nyra",0};
            exec("/bin/test_linux",argv,envp);
            klog("nyra: linux exec failed\n"); exit(1);
        } else if(pid>0){ int st=0; waitpid(pid,&st); add_line("linuxtest done (see serial / kernel log for its output)"); }
        else add_line("fork failed");
    } else if(strcmp(cmd,"linuxtest2")==0){
        /* clone+futex (threads) + UDP loopback + execve, through the Linux ABI */
        long pid=fork();
        if(pid==0){
            char *argv[]={"/bin/test_linux2",0};
            char *envp[]={"HOME=/home/yart","TERM=nyra",0};
            exec("/bin/test_linux2",argv,envp);
            klog("nyra: linux exec failed\n"); exit(1);
        } else if(pid>0){ int st=0; waitpid(pid,&st); add_line("linuxtest2 done (threads/sockets/execve)"); }
        else add_line("fork failed");
    } else if(strcmp(cmd,"dynhello")==0){
        /* run a dynamically-linked Linux program (the full PT_INTERP chain) */
        long pid=fork();
        if(pid==0){
            char *argv[]={"/bin/dynhello",0};
            char *envp[]={"HOME=/home/yart","TERM=nyra",0};
            exec("/bin/dynhello",argv,envp);
            klog("nyra: dynhello exec failed\n"); exit(1);
        } else if(pid>0){ int st=0; waitpid(pid,&st); add_line("dynhello done (see serial / kernel log)"); }
        else add_line("fork failed");
    } else if(strcmp(cmd,"tlsdemo")==0){
        /* TLS through the dynamic linker: __thread in program + a .so */
        long pid=fork();
        if(pid==0){
            char *argv[]={"/bin/tlsdemo",0};
            char *envp[]={"HOME=/home/yart","TERM=nyra",0};
            exec("/bin/tlsdemo",argv,envp);
            klog("nyra: tlsdemo exec failed\n"); exit(1);
        } else if(pid>0){ int st=0; waitpid(pid,&st); add_line("tlsdemo done (see serial / kernel log)"); }
        else add_line("fork failed");
    } else if(strcmp(cmd,"ifuncdemo")==0){
        /* IFUNC (STT_GNU_IFUNC) resolution through the dynamic linker */
        long pid=fork();
        if(pid==0){
            char *argv[]={"/bin/ifuncdemo",0};
            char *envp[]={"HOME=/home/yart","TERM=nyra",0};
            exec("/bin/ifuncdemo",argv,envp);
            klog("nyra: ifuncdemo exec failed\n"); exit(1);
        } else if(pid>0){ int st=0; waitpid(pid,&st); add_line("ifuncdemo done (see serial / kernel log)"); }
        else add_line("fork failed");
    } else if(strcmp(cmd,"copydemo")==0){
        /* copy relocation (a .so referencing a program global) */
        long pid=fork();
        if(pid==0){
            char *argv[]={"/bin/copydemo",0};
            char *envp[]={"HOME=/home/yart","TERM=nyra",0};
            exec("/bin/copydemo",argv,envp);
            klog("nyra: copydemo exec failed\n"); exit(1);
        } else if(pid>0){ int st=0; waitpid(pid,&st); add_line("copydemo done (see serial / kernel log)"); }
        else add_line("fork failed");
    } else if(strcmp(cmd,"tlstest")==0){
        /* verify TLS: arch_prctl ARCH_SET_FS -> %fs base round-trip */
        long pid=fork();
        if(pid==0){
            char *argv[]={"/bin/test_tls",0};
            char *envp[]={"HOME=/home/yart","TERM=nyra",0};
            exec("/bin/test_tls",argv,envp);
            klog("nyra: tlstest exec failed\n"); exit(1);
        } else if(pid>0){ int st=0; waitpid(pid,&st); add_line("tlstest done (see serial / kernel log)"); }
        else add_line("fork failed");
    } else if(strcmp(cmd,"exit")==0){
        add_line("bye"); wm_flip(G_win_id); sleep(200); goto do_exit;
    } else if(strcmp(cmd,"jobs")==0){
        if(G_job_count==0){ add_line("no jobs"); }
        else for(int i=0;i<G_job_count;i++) job_status_line(i, G_jobs[i].stopped?"Stopped":"Running");
    } else if(strcmp(cmd,"fg")==0 || strncmp(cmd,"fg ",3)==0){
        int idx=0;
        if(cmd[2]==' '){ idx=0; for(const char*p=cmd+3;*p>='0'&&*p<='9';p++) idx=idx*10+(*p-'0'); idx--; }
        if(idx<0||idx>=G_job_count){ add_line("fg: no such job"); }
        else {
            job_t *j=&G_jobs[idx];
            if(j->stopped){ raise(j->pid, 18); j->stopped=false; }   /* SIGCONT */
            G_fg_pid=j->pid;
            job_status_line(idx, "Foreground");
        }
    } else if(strcmp(cmd,"bg")==0 || strncmp(cmd,"bg ",3)==0){
        int idx=0;
        if(cmd[2]==' '){ idx=0; for(const char*p=cmd+3;*p>='0'&&*p<='9';p++) idx=idx*10+(*p-'0'); idx--; }
        if(idx<0||idx>=G_job_count){ add_line("bg: no such job"); }
        else if(!G_jobs[idx].stopped){ add_line("bg: job already running"); }
        else { raise(G_jobs[idx].pid, 18); G_jobs[idx].stopped=false; job_status_line(idx, "Running &"); }
    } else if(cmd[0]=='/'){
        /* external program: run as a JOB, with real argv (space-separated
         * arguments, like a real shell).  Trailing '&' = background. */
        char prog[LINE_LEN]; strncpy(prog, cmd, LINE_LEN-1); prog[LINE_LEN-1]=0;
        bool bg=false;
        int len=strlen(prog);
        while(len>0 && (prog[len-1]==' ' || prog[len-1]=='\t')){ prog[--len]=0; }
        if(len>0 && prog[len-1]=='&'){ bg=true; prog[--len]=0; }
        while(len>0 && (prog[len-1]==' ' || prog[len-1]=='\t')){ prog[--len]=0; }
        if(!prog[0]){ add_line("usage: /bin/prog [args...] [&]"); return; }
        /* tokenize in place into argv (path + up to 30 args) */
        char *argv[32]; int argc=0;
        char *p=prog;
        while(*p && argc<31){
            while(*p==' '||*p=='\t') p++;
            if(!*p) break;
            argv[argc++]=p;
            while(*p && *p!=' ' && *p!='\t') p++;
            if(*p){ *p=0; p++; }
        }
        argv[argc]=0;
        long pid=fork();
        if(pid==0){
            char *envp[]={"HOME=/home/yart","TERM=nyra",0};
            exec(argv[0], argv, envp);
            klog("nyra: exec failed\n"); exit(1);
        } else if(pid>0){
            int n=job_add(pid, prog);
            if(bg){ char l[LINE_LEN]; int k=0; char nb[8]; put_dec((long)n,nb);
                const char*pre="["; while(*pre)l[k++]=*pre++;
                for(int i=0;nb[i];i++)l[k++]=nb[i];
                const char*p2="] "; while(*p2)l[k++]=*p2++;
                put_dec(pid, nb); for(int i=0;nb[i];i++)l[k++]=nb[i];
                l[k]=0; add_line(l); }
            else { G_fg_pid=pid; job_status_line(n-1, "Foreground"); }
        } else add_line("fork failed");
        return;
    } else {
        add_line("unknown command, type help");
        G_status=1;
    }
    return;
do_exit:
    wm_destroy(G_win_id);
    exit(0);
}

/* ---------------- rendering ---------------- */
static int logical_rows(void){ return G_sb_count + G_cur_r + 1; }
static const tcell_t *line_at(int lr){
    if(lr < G_sb_count) return G_sb[lr];
    lr -= G_sb_count;
    if(lr >= T_ROWS) lr = T_ROWS-1;
    return G_grid[lr];
}
static void line_to_text(int lr, char *out, int cap){
    const tcell_t *l=line_at(lr);
    int k=0;
    for(int c=0;c<T_COLS && k<cap-1;c++) if(l[c].ch && l[c].ch!=' '){ out[k++]=l[c].ch; }
    out[k]=0;
}
static void term_copy_lines(int a, int b){
    if(a<0||b<0) return;
    if(a>b){ int t=a; a=b; b=t; }
    if(b-a > 100) b=a+100;
    char buf[LINE_LEN*100]; int p=0;
    for(int i=a;i<=b && p<(int)sizeof(buf)-2;i++){
        char l[LINE_LEN]; line_to_text(i, l, LINE_LEN);
        for(int j=0;l[j] && p<(int)sizeof(buf)-2;j++) buf[p++]=l[j];
        buf[p++]='\n';
    }
    buf[p]=0;
    clipboard_set(buf);
}
static void term_paste(void){
    char buf[1024];
    long n=clipboard_get(buf, sizeof buf);
    if(n<=0) return;
    for(long i=0;i<n && buf[i];i++){
        if(buf[i]=='\n') continue;      /* paste without newlines */
        if(G_cmd_len<LINE_LEN-1){ cmd_insert(buf[i]); }
    }
}

static void draw_term(void){
    sf_fill(&G_surf, RGB(0x18,0x18,0x1B));
    int total=logical_rows();
    int last=total-1-G_view_scroll; if(last<0) last=0;
    int first=last-T_ROWS+1; if(first<0) first=0;
    for(int lr=first, y=0; lr<=last && y<T_ROWS; lr++, y++){
        const tcell_t *l=line_at(lr);
        int lo=-1, hi=-1;
        if(G_sel_active){
            lo=G_sel_anchor<G_sel_end?G_sel_anchor:G_sel_end;
            hi=G_sel_anchor>G_sel_end?G_sel_anchor:G_sel_end;
            if(lr>=lo && lr<=hi)
                sf_fill_rect(&G_surf, T_PAD_X, T_PAD_Y+y*FONT_H, G_usable_px, FONT_H-2, RGB(0x1E,0x3A,0x5E));
        }
        /* Render the row PROPORTIONALLY, grouped into same-colour runs, so
         * the text looks exactly like the system font (sf_text).  x advances
         * by each glyph's real width. */
        int x = T_PAD_X;
        int i = 0;
        while(i < T_COLS && x < T_PAD_X + G_usable_px){
            char ch = l[i].ch;
            if(!ch) break;                       /* empty tail: done */
            u8 fg = l[i].fg, bg = l[i].bg;
            int j = i; char run[T_COLS+1]; int rn = 0;
            while(j < T_COLS && l[j].ch && l[j].fg==fg && l[j].bg==bg){
                run[rn++] = l[j].ch;
                j++;
            }
            run[rn] = 0;
            int w = sf_text_width(run);
            int gy = T_PAD_Y + y*FONT_H;
            if(bg!=0 && bg<16) sf_fill_rect(&G_surf, x, gy, w, FONT_H, ANSI16[bg]);
            sf_text(&G_surf, x, gy, run, ANSI16[fg]);
            x += w;
            i = j;
        }
    }
    /* block cursor at the edit position (pixel-exact) */
    if(G_cursor_on && G_view_scroll==0 && (G_cursor_blink/20)%2==0){
        int cx, cy; edit_cursor_px(G_edit, &cx, &cy);
        sf_fill_rect_blend(&G_surf, cx, cy+FONT_H-2, 9, 2, RGB(0x60,0xA5,0xFA));
    }
    if(G_view_scroll>0){
        sf_fill_rect(&G_surf, (int)G_info.w-10, 4, 6, 24, RGB(0x3B,0x82,0xF6));
    }
}

int main_entry(int argc, char **argv, char **envp){
    (void)argc; (void)argv;
    int test_exit=0;
    if(envp){ for(int i=0;envp[i];i++){ const char *e=envp[i]; if(e[0]=='Y'&&e[1]=='A'&&e[2]=='R'&&e[3]=='T'&&e[4]=='_'&&e[5]=='T'&&e[6]=='E'&&e[7]=='S'&&e[8]=='T'&&e[9]=='_'&&e[10]=='E'&&e[11]=='X'&&e[12]=='I'&&e[13]=='T'&&e[14]=='='&&e[15]=='1') test_exit=1; } }
    long id=wm_create(WIN_W, WIN_H, &G_info);
    if(id<0){ klog("nyra: wm_create failed\n"); return 1; }
    if(test_exit){
        G_win_id=(int)id;
        G_surf.px=(u32*)(unsigned long)G_info.app_va; G_surf.w=(int)G_info.w; G_surf.h=(int)G_info.h; G_surf.pitch=(int)G_info.w;
        wm_title(G_win_id, "Nyra Terminal [test]");
        sf_fill(&G_surf, RGB(0x0E,0x0E,0x11));
        sf_text(&G_surf, 10,10,"Nyra test round trip", RGB(0xFA,0xFA,0xFA));
        wm_flip(G_win_id); sleep(500); wm_destroy(G_win_id); klog("nyra: test mode round trip OK\n"); return 0;
    }
    G_win_id=(int)id;
    G_surf.px=(u32*)(unsigned long)G_info.app_va;
    G_surf.w=(int)G_info.w; G_surf.h=(int)G_info.h; G_surf.pitch=(int)G_info.w;
    wm_title(G_win_id, "Console");

    /* size the grid from the surface, clamped */
    env_init();
    G_usable_px = (int)G_info.w - 2*T_PAD_X;
    T_COLS = MAX_COLS;                       /* char capacity per row        */
    T_ROWS = ((int)G_info.h - 2*T_PAD_Y)/FONT_H;
    if(T_ROWS<4) T_ROWS=4;
    if(T_ROWS>MAX_ROWS) T_ROWS=MAX_ROWS;

    term_clear_all();
    term_write("\x1b[1;36mNyra Terminal - VT100/ANSI ready.\x1b[0m Type \x1b[1;32mhelp\x1b[0m.");
    term_newline();
    prompt_print();
    draw_term(); wm_flip(G_win_id);

    int mx=(int)(G_info.win_x+G_info.w/2);
    int my=(int)(G_info.win_y+G_info.h/2);
    bool dirty=true;
    int last_blink=-1;
    while(1){
        mouse_ev_t m; while(poll_mouse(&m)){
            int gp[2];
            if(mouse_pos(gp)==0){ mx=gp[0]; my=gp[1]; }
            else { mx+=m.dx; my+=m.dy; }
            if(mx<0){mx=0;} if(my<0){my=0;}
            if(m.dx || m.dy || m.buttons || m.wheel) dirty=true;
            if(m.wheel){
                int max_scroll=logical_rows()-1; if(max_scroll<0) max_scroll=0;
                G_view_scroll += m.wheel*3;
                if(G_view_scroll<0) G_view_scroll=0;
                if(G_view_scroll>max_scroll) G_view_scroll=max_scroll;
            }
            int total=logical_rows();
            int last=total-1-G_view_scroll; if(last<0) last=0;
            int first=last-T_ROWS+1; if(first<0) first=0;
            int row=(my-(int)G_info.win_y-T_PAD_Y)/FONT_H;
            int lr=first+row; if(lr<first) lr=first; if(lr>last) lr=last;
            bool left=(m.buttons & 1)!=0;
            if(left && !G_btn_left){ G_btn_left=true; G_did_drag=false; G_sel_anchor=G_sel_end=lr; G_sel_active=false; }
            else if(left && G_btn_left){ if(lr!=G_sel_end){ G_sel_end=lr; G_did_drag=true; G_sel_active=true; } }
            else if(!left && G_btn_left){
                G_btn_left=false;
                if(G_did_drag) term_copy_lines(G_sel_anchor, G_sel_end);  /* drag = select + copy */
                else term_paste();                                        /* plain click = paste */
                /* Keep the selection HIGHLIGHT visible after the drag (the old
                 * code cleared it here, so text deselected immediately with no
                 * way to copy again - the user's "it just deselects" complaint).
                 * It clears on the next press or on Ctrl+C. */
            }
        }
        int ev; while((ev=poll_key())!=0){
            int ascii=ev&0xFF; int make=!(ev&(1<<16));
            bool ctrl=(ev&(1<<18))!=0;
            if(!make) continue;
            dirty=true;
            G_view_scroll=0;

            if(ctrl && (ascii=='z' || ascii==26)){   /* Ctrl+Z: stop the fg job */
                if(G_fg_pid>0){
                    int ji=job_find(G_fg_pid);
                    raise(G_fg_pid, 19);             /* SIGSTOP */
                    if(ji>=0) G_jobs[ji].stopped=true;
                    if(ji>=0) job_status_line(ji, "Stopped");
                    else add_line("^Z stopped");
                    G_fg_pid=-1;
                }
                continue;
            }
            /* Ctrl+Shift+C = copy, Ctrl+Shift+V = paste (the REAL terminal
             * convention).  Ctrl+C stays "interrupt/cancel" (SIGINT) and is
             * NEVER copy - that is what a real OS terminal does.  The driver
             * reports a shifted key as the UPPERCASE ascii ('C'/'V') plus the
             * KEY_SHIFT flag, so we distinguish them from plain ctrl. */
            if(ctrl && ascii=='C'){
                if(G_sel_active){ term_copy_lines(G_sel_anchor, G_sel_end); G_sel_active=false; G_sel_anchor=G_sel_end=-1; }
                continue;
            }
            if(ctrl && ascii=='V'){ term_paste(); continue; }
            if(ctrl && (ascii=='c' || ascii==3)){
                if(G_fg_pid>0){
                    /* SIGINT the foreground job (real Ctrl+C): the child gets
                     * signal 2, and with no handler it terminates (status 130)
                     * and is reaped by the waitpid loop.  The old code just
                     * printed ^C and cleared G_fg_pid, ORPHANING the child and
                     * leaking its zombie forever. */
                    add_line("^C");
                    raise(G_fg_pid, 2);
                    continue;
                }
                cmd_cancel(); continue;
            }
            if(ctrl && (ascii=='l' || ascii==12)){ screen_clear_and_prompt(); continue; }
            if(ctrl && (ascii=='d' || ascii==4)){ if(G_cmd_len==0){ add_line(""); wm_flip(G_win_id); goto do_exit; } else cmd_delete(); continue; }
            if(ctrl && (ascii=='a' || ascii==1)){ G_edit=0; line_render(); continue; }
            if(ctrl && (ascii=='e' || ascii==5)){ G_edit=G_cmd_len; line_render(); continue; }
            if(ctrl && (ascii=='u' || ascii==21)){ G_cmd_len=0; G_cmd[0]=0; G_edit=0; line_render(); continue; }
            if(ctrl && (ascii=='k' || ascii==11)){ G_cmd[G_edit]=0; G_cmd_len=G_edit; line_render(); continue; }

            if(ascii==0xE3){ if(G_edit>0){ G_edit--; line_render(); } }        /* YK_LEFT  */
            else if(ascii==0xE4){ if(G_edit<G_cmd_len){ G_edit++; line_render(); } } /* YK_RIGHT */
            else if(ascii==0xE5){ G_edit=0; line_render(); }                     /* YK_HOME  */
            else if(ascii==0xE6){ G_edit=G_cmd_len; line_render(); }             /* YK_END   */
            else if(ascii==0xE1){ /* up: history */
                if(G_hist_pos>0){ G_hist_pos--; strncpy(G_cmd, G_history[G_hist_pos], LINE_LEN-1); G_cmd_len=strlen(G_cmd); G_edit=G_cmd_len; line_render(); }
            }
            else if(ascii==0xE2){ /* down: history */
                if(G_hist_pos+1<G_hist_count){ G_hist_pos++; strncpy(G_cmd, G_history[G_hist_pos], LINE_LEN-1); G_cmd_len=strlen(G_cmd); G_edit=G_cmd_len; line_render(); }
                else { G_cmd_len=0; G_cmd[0]=0; G_edit=0; G_hist_pos=G_hist_count; line_render(); }
            }
            else if(ascii==0xE9){ cmd_delete(); }                               /* YK_DEL   */
            else if(ascii==0xE7){ /* pgup: scroll back a page */
                G_view_scroll += T_ROWS-1;
                int ms=logical_rows()-1; if(ms<0) ms=0;
                if(G_view_scroll>ms) G_view_scroll=ms;
            }
            else if(ascii==0xE8){ G_view_scroll -= T_ROWS-1; if(G_view_scroll<0) G_view_scroll=0; }
            else if(ascii>=32 && ascii<127){ cmd_insert((char)ascii); }
            else if(ascii==8 || ascii==127){ cmd_backspace(); }
            else if(ascii==13 || ascii==10){
                if(G_fg_pid>0){ add_line("job still running (Ctrl+Z to stop)"); }
                else {
                    term_newline();
                    char cmd[LINE_LEN]; strncpy(cmd, G_cmd, LINE_LEN-1); cmd[G_cmd_len]=0;
                    execute(cmd);
                    prompt_print();
                }
            }
            else if(ascii==27){ cmd_cancel(); }
        }
        /* foreground job: poll for exit / stop (job control) */
        if(G_fg_pid>0){
            int st=0;
            long r=waitpid_nohang(G_fg_pid, &st);
            if(r==G_fg_pid){
                int ji=job_find(G_fg_pid);
                if(st==-19){
                    if(ji>=0){ G_jobs[ji].stopped=true; job_status_line(ji, "Stopped"); }
                } else {
                    if(ji>=0){ job_status_line(ji, "Done"); job_remove(ji); }
                    else add_line("job done");
                }
                G_fg_pid=-1;
                prompt_print();
                dirty=true;
            }
        }
        /* reap exited BACKGROUND jobs (throttled to ~2x/s).  Without this,
         * a background job that finishes stays a zombie forever (its parent -
         * this shell - is alive, so the kernel orphan reaper never touches it)
         * and `jobs` keeps listing it as "Running".  This is the classic
         * shell zombie leak. */
        {
            static int bg_reap_tick = 0;
            if(++bg_reap_tick >= 40){
                bg_reap_tick = 0;
                for(int i=0;i<G_job_count;){
                    if(G_jobs[i].pid == G_fg_pid){ i++; continue; }
                    int st=0;
                    long r=waitpid_nohang(G_jobs[i].pid, &st);
                    if(r==G_jobs[i].pid){
                        if(st==-19){ G_jobs[i].stopped=true; job_status_line(i, "Stopped"); i++; }
                        else { job_status_line(i, "Done"); job_remove(i); dirty=true; }
                    } else if(r==-1){
                        /* ECHILD: never ours / already gone - drop the slot */
                        job_remove(i); dirty=true;
                    } else i++;
                }
            }
        }
        G_cursor_blink++;
        int blink=(G_cursor_blink/20)%2;
        if(dirty || blink!=last_blink){
            draw_term(); wm_flip(G_win_id);
            dirty=false; last_blink=blink;
        }
        sleep(16);
    }
    return 0;
do_exit:
    wm_destroy(G_win_id);
    exit(0);
    return 0;
}
