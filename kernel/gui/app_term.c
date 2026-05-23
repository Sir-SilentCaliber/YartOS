/* Yart OS - Terminal.
 *
 * Built-in shell that issues real syscalls (open/read/write/mkdir/unlink/
 * chdir/getcwd/getdents/stat) so it does the same thing a userland shell
 * would.  Commands:
 *   help, ver, mem, date, clear
 *   pwd, cd <path>, ls [path]
 *   cat <file>, touch <file>, mkdir <path>, rm <path>, echo <text>
 *   nano <file>            (opens the editor)
 *   open <file>            (opens with editor)
 */
#include <yart/gui.h>
#include <yart/theme.h>
#include <yart/string.h>
#include <yart/mm.h>
#include <yart/console.h>
#include <yart/hal.h>
#include <yart/syscall.h>
#include <yart/fs.h>
#include <yart/task.h>

extern void open_editor(const char *path);

/* Tiny in-kernel shim that reaches the syscall dispatcher directly.
 * For real userland we issue `int 0x80`; in-kernel we just call the
 * functions that handle each syscall. */
extern i64 sys_call(int n, u64 a0, u64 a1, u64 a2);

#define COLS_MAX 256
#define ROWS_MAX 200
#define INPUT_MAX 240

typedef struct {
    char  buf[ROWS_MAX][COLS_MAX];
    int   nrows;
    char  input[INPUT_MAX];
    int   in_len;
    int   scroll;       /* offset from bottom, 0=at bottom */
    bool  prev_left;
} term_state_t;

static term_state_t *g_active_term;

static void tputc(term_state_t *st, char c) {
    if (st->nrows == 0) st->nrows = 1;
    char *line = st->buf[st->nrows - 1];
    int n = strlen(line);
    if (c == '\n' || n >= COLS_MAX - 1) {
        if (st->nrows >= ROWS_MAX) {
            for (int i = 0; i < ROWS_MAX - 1; i++)
                memcpy(st->buf[i], st->buf[i+1], COLS_MAX);
            st->nrows = ROWS_MAX - 1;
        }
        st->buf[st->nrows][0] = 0;
        st->nrows++;
        if (c == '\n') return;
        line = st->buf[st->nrows - 1];
        n = 0;
    }
    line[n] = c;
    line[n+1] = 0;
}

static void tputs(term_state_t *st, const char *s) { while (*s) tputc(st, *s++); }
static void tprintf(term_state_t *st, const char *fmt, ...) {
    char b[256];
    __builtin_va_list ap; __builtin_va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    __builtin_va_end(ap);
    tputs(st, b);
}

/* ---- in-kernel sys_call shim that maps to the real handlers ---- */
i64 sys_call(int n, u64 a0, u64 a1, u64 a2) {
    /* Re-issue an int 0x80 software interrupt so the real dispatcher
       runs.  In-kernel callers go through exactly the same path as
       ring-3 callers. */
    i64 ret;
    register u64 r10 __asm__("r10") = 0;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"((u64)n), "D"(a0), "S"(a1), "d"(a2), "r"(r10)
        : "memory", "rcx", "r11"
    );
    return ret;
}

static int sys_write_  (int fd, const char *b, u64 n) { return (int)sys_call(SYS_WRITE,    fd, (u64)b, n); }
static int sys_open_   (const char *p, int f)         { return (int)sys_call(SYS_OPEN,    (u64)p, f, 0); }
static int sys_close_  (int fd)                       { return (int)sys_call(SYS_CLOSE,   fd, 0, 0); }
static int sys_read_   (int fd, char *b, u64 n)       { return (int)sys_call(SYS_READ,    fd, (u64)b, n); }
static int sys_mkdir_  (const char *p)                { return (int)sys_call(SYS_MKDIR,   (u64)p, 0, 0); }
static int sys_unlink_ (const char *p)                { return (int)sys_call(SYS_UNLINK,  (u64)p, 0, 0); }
static int sys_chdir_  (const char *p)                { return (int)sys_call(SYS_CHDIR,   (u64)p, 0, 0); }
static int sys_getcwd_ (char *b, u64 n)               { return (int)sys_call(SYS_GETCWD,  (u64)b, n, 0); }
static int sys_getdents_(int fd, void *o, u64 n)      { return (int)sys_call(SYS_GETDENTS, fd, (u64)o, n); }

static void cmd_pwd(term_state_t *st, char *args) {
    UNUSED(args);
    char buf[VFS_MAX_PATH] = {0};
    sys_getcwd_(buf, sizeof buf);
    tprintf(st, "%s\n", buf);
}

static void cmd_cd(term_state_t *st, char *args) {
    if (!*args) { sys_chdir_("/home/yart"); return; }
    if (sys_chdir_(args) < 0) tprintf(st, "cd: %s: no such directory\n", args);
}

static void cmd_ls(term_state_t *st, char *args) {
    const char *path = *args ? args : ".";
    int fd = sys_open_(path, O_RDONLY);
    if (fd < 0) { tprintf(st, "ls: %s: not found\n", path); return; }
    yart_dirent_t entries[64];
    int n = sys_getdents_(fd, entries, 64);
    sys_close_(fd);
    if (n < 0) { tprintf(st, "ls: not a directory\n"); return; }
    for (int i = 0; i < n; i++) {
        if (entries[i].type == VN_DIR)
            tprintf(st, "  %s/\n", entries[i].name);
        else
            tprintf(st, "  %s    %lu\n", entries[i].name, entries[i].size);
    }
    if (n == 0) tprintf(st, "(empty)\n");
}

static void cmd_cat(term_state_t *st, char *args) {
    if (!*args) { tputs(st, "usage: cat <file>\n"); return; }
    int fd = sys_open_(args, O_RDONLY);
    if (fd < 0) { tprintf(st, "cat: %s: not found\n", args); return; }
    char buf[129];
    int r;
    while ((r = sys_read_(fd, buf, 128)) > 0) {
        buf[r] = 0;
        tputs(st, buf);
    }
    sys_close_(fd);
    /* ensure newline */
    int last = st->nrows - 1;
    if (last >= 0 && st->buf[last][0]) tputc(st, '\n');
}

static void cmd_touch(term_state_t *st, char *args) {
    if (!*args) { tputs(st, "usage: touch <file>\n"); return; }
    int fd = sys_open_(args, O_RDWR | O_CREAT);
    if (fd < 0) { tprintf(st, "touch: %s: failed\n", args); return; }
    sys_close_(fd);
}

static void cmd_mkdir(term_state_t *st, char *args) {
    if (!*args) { tputs(st, "usage: mkdir <path>\n"); return; }
    if (sys_mkdir_(args) < 0) tprintf(st, "mkdir: %s: failed\n", args);
}

static void cmd_rm(term_state_t *st, char *args) {
    if (!*args) { tputs(st, "usage: rm <path>\n"); return; }
    if (sys_unlink_(args) < 0) tprintf(st, "rm: %s: failed\n", args);
}

static void cmd_echo(term_state_t *st, char *args) {
    /* echo "text" > file ?  simple: no redirection yet */
    tputs(st, args);
    tputc(st, '\n');
}

static void cmd_clear(term_state_t *st, char *args) {
    UNUSED(args);
    st->nrows = 0;
    st->scroll = 0;
}

static void cmd_help(term_state_t *st, char *args) {
    UNUSED(args);
    tputs(st,
        "Yart shell - built-in commands\n"
        "  help, ver, mem, date, clear\n"
        "  pwd, cd <path>, ls [path]\n"
        "  cat <file>, touch <file>, mkdir <path>, rm <path>\n"
        "  echo <text>, nano <file>, open <file>\n"
        "All commands go through real syscalls.\n");
}

static void cmd_ver(term_state_t *st, char *args) {
    UNUSED(args);
    tputs(st, "Yart OS " YART_VERSION " (slate-amber)\n");
}

static void cmd_mem(term_state_t *st, char *args) {
    UNUSED(args);
    tprintf(st, "memory: %lu / %lu MiB used\n",
            pmm_used_pages() * PAGE_SIZE / MB(1),
            pmm_total_pages() * PAGE_SIZE / MB(1));
}

static void cmd_date(term_state_t *st, char *args) {
    UNUSED(args);
    rtc_time_t t; rtc_read(&t);
    tprintf(st, "%04u-%02u-%02u %02u:%02u:%02u UTC\n",
            t.year, t.month, t.day, t.hour, t.minute, t.second);
}

static void cmd_nano(term_state_t *st, char *args) {
    if (!*args) { tputs(st, "usage: nano <file>\n"); return; }
    open_editor(args);
}

static void cmd_open(term_state_t *st, char *args) {
    cmd_nano(st, args);
}

typedef struct { const char *name; void (*fn)(term_state_t *, char *); } cmd_t;
static const cmd_t cmds[] = {
    {"help", cmd_help}, {"ver", cmd_ver}, {"mem", cmd_mem},
    {"date", cmd_date}, {"clear", cmd_clear},
    {"pwd", cmd_pwd}, {"cd", cmd_cd}, {"ls", cmd_ls},
    {"cat", cmd_cat}, {"touch", cmd_touch}, {"mkdir", cmd_mkdir},
    {"rm", cmd_rm}, {"echo", cmd_echo}, {"nano", cmd_nano},
    {"open", cmd_open},
};
#define N_CMDS (int)(sizeof cmds / sizeof cmds[0])

/* Simple Levenshtein distance for short strings. */
static int edit_distance(const char *a, const char *b) {
    int len_a = strlen(a);
    int len_b = strlen(b);
    if (len_a > 15 || len_b > 15) return 100;
    int d[16][16];
    for (int i = 0; i <= len_a; i++) d[i][0] = i;
    for (int j = 0; j <= len_b; j++) d[0][j] = j;
    for (int i = 1; i <= len_a; i++) {
        for (int j = 1; j <= len_b; j++) {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            int del = d[i - 1][j] + 1;
            int ins = d[i][j - 1] + 1;
            int sub = d[i - 1][j - 1] + cost;
            int min = del < ins ? del : ins;
            min = min < sub ? min : sub;
            d[i][j] = min;
        }
    }
    return d[len_a][len_b];
}

static void term_run(term_state_t *st, char *line) {
    /* split first token */
    char *args = line;
    while (*args && *args != ' ') args++;
    char savec = *args;
    *args = 0;
    char *cmd = line;
    char *rest = (savec ? args + 1 : args);
    while (*rest == ' ') rest++;
    if (!*cmd) return;
    for (int i = 0; i < N_CMDS; i++) {
        if (strcmp(cmd, cmds[i].name) == 0) {
            cmds[i].fn(st, rest);
            return;
        }
    }
    tprintf(st, "yart: %s: command not found\n", cmd);

    /* Suggest closest built-in command, Ubuntu-style. */
    int best_dist = 100;
    const char *best_cmd = NULL;
    for (int i = 0; i < N_CMDS; i++) {
        int dist = edit_distance(cmd, cmds[i].name);
        if (dist < best_dist && dist > 0) {
            best_dist = dist;
            best_cmd = cmds[i].name;
        }
    }
    if (best_cmd && best_dist <= 2) {
        tprintf(st, "    did you mean: '%s'\n", best_cmd);
    }
}

static void term_paint(window_t *w) {
    term_state_t *st = w->ud;
    g_active_term = st;
    int x = w->x + 6, y = w->y + WIN_TITLE_H + 6;
    int W = w->w - 12, H = w->h - WIN_TITLE_H - 12;
    draw_rect(x, y, W, H, TH_EDITOR_BG);
    draw_rounded_rect_outline(x, y, W, H, 4, TH_WIN_BORDER);

    int prompt_h = FONT_H + 4;
    int gap = 2;
    int max_content_h = H - 12; /* 6px top + 6px bottom padding */

    /* Decide whether the prompt follows content or we scroll. */
    int content_h = st->nrows * FONT_H + gap + prompt_h;
    int top_row = 0;
    if (content_h > max_content_h) {
        int rows_that_fit = (max_content_h - gap - prompt_h) / FONT_H;
        top_row = MAX(0, st->nrows - rows_that_fit);
    }

    int ty = y + 6;
    for (int i = top_row; i < st->nrows; i++) {
        draw_text(x + 8, ty, st->buf[i], TH_EDITOR_FG, 0xFF000000);
        ty += FONT_H;
    }

    /* prompt + input – drawn directly after buffered content */
    int py = ty + gap;
    if (py + prompt_h > y + H - 6) {
        py = y + H - 6 - prompt_h;
    }
    if (py < y + 6) {
        py = y + 6;
    }

    char cwd[VFS_MAX_PATH] = {0};
    sys_getcwd_(cwd, sizeof cwd);
    char prompt[VFS_MAX_PATH + 8];
    snprintf(prompt, sizeof prompt, "%s$ ", cwd);
    draw_rect(x + 1, py - 2, W - 2, prompt_h, TH_EDITOR_GUTTER);
    int px = x + 8;
    draw_text(px, py, prompt, TH_ACCENT, 0xFF000000);
    px += text_width(prompt);
    draw_text(px, py, st->input, TH_EDITOR_FG, 0xFF000000);
    /* caret */
    if ((pit_ticks() / 50) & 1) {
        int cx = px + st->in_len * FONT_W;
        draw_rect(cx, py, 8, FONT_H, TH_EDITOR_CARET);
    }
}

static void term_on_key(window_t *w, int sc, char ch, u32 mods) {
    (void)mods;
    term_state_t *st = w->ud;
    UNUSED(sc);
    if (ch == '\n') {
        st->input[st->in_len] = 0;
        char cwd[VFS_MAX_PATH] = {0};
        sys_getcwd_(cwd, sizeof cwd);
        char echo[VFS_MAX_PATH + INPUT_MAX + 8];
        snprintf(echo, sizeof echo, "%s$ %s", cwd, st->input);
        tputs(st, echo);
        tputc(st, '\n');
        char copy[INPUT_MAX];
        memcpy(copy, st->input, sizeof copy);
        term_run(st, copy);
        st->in_len = 0;
        st->input[0] = 0;
    } else if (ch == '\b' || ch == 8) {
        if (st->in_len) st->input[--st->in_len] = 0;
    } else if (ch >= ' ' && ch < 127 && st->in_len < INPUT_MAX - 1) {
        st->input[st->in_len++] = ch;
        st->input[st->in_len] = 0;
    }
}

void open_terminal(void) {
    term_state_t *st = kzalloc(sizeof *st);
    int w = 600, h = 300;
    int x = (g_fb.width - w) / 2;
    int y = g_fb.height - h - 100;
    window_t *win = window_create("Terminal", ICON_TERM, x, y, w, h, term_paint);
    if (!win) { kfree(st); return; }
    win->ud = st;
    win->on_key = term_on_key;
    win->bg = TH_EDITOR_BG;
    win->min_w = 360; win->min_h = 220;
    win->flags |= WIN_ANIM;
    tputs(st, "Yart OS " YART_VERSION " - type 'help' to begin.\n");
}
