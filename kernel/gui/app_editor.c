#include <yart/drivers.h>
/* Yart OS - Editor (a tiny nano-alike).
 *
 * Loads a file via syscalls, lets you edit it in a single text buffer,
 * Ctrl+S to save, Ctrl+Q to close.  Word wrap is intentionally absent;
 * lines extend off-screen and are clipped.
 */
#include <yart/gui.h>
#include <yart/theme.h>
#include <yart/string.h>
#include <yart/mm.h>
#include <yart/syscall.h>
#include <yart/fs.h>
#include <yart/hal.h>

extern i64 sys_call(int n, u64 a0, u64 a1, u64 a2);

#define EDIT_BUF (32 * 1024)

typedef struct {
    char path[VFS_MAX_PATH];
    char buf[EDIT_BUF];
    int  size;
    int  caret;     /* byte offset */
    int  scroll;    /* top line */
    bool dirty;
    int  saved_msg_until;
    char saved_msg[64];
} ed_t;

static int line_start(ed_t *e, int pos) {
    while (pos > 0 && e->buf[pos - 1] != '\n') pos--;
    return pos;
}

static int line_end(ed_t *e, int pos) {
    while (pos < e->size && e->buf[pos] != '\n') pos++;
    return pos;
}

static int line_index(ed_t *e, int pos) {
    int n = 0;
    for (int i = 0; i < pos && i < e->size; i++) if (e->buf[i] == '\n') n++;
    return n;
}

static int line_count(ed_t *e) {
    int n = 1;
    for (int i = 0; i < e->size; i++) if (e->buf[i] == '\n') n++;
    return n;
}

static void ed_load(ed_t *e) {
    int fd = (int)sys_call(SYS_OPEN, (u64)e->path, O_RDWR | O_CREAT, 0);
    if (fd < 0) { e->size = 0; return; }
    int n = (int)sys_call(SYS_READ, fd, (u64)e->buf, sizeof e->buf - 1);
    if (n < 0) n = 0;
    e->size = n;
    e->buf[n] = 0;
    e->caret = 0;
    e->dirty = false;
    sys_call(SYS_CLOSE, fd, 0, 0);
}

static void ed_save(ed_t *e) {
    int fd = (int)sys_call(SYS_OPEN, (u64)e->path, O_RDWR | O_CREAT | O_TRUNC, 0);
    if (fd < 0) {
        snprintf(e->saved_msg, sizeof e->saved_msg, "save failed");
    } else {
        int w = (int)sys_call(SYS_WRITE, fd, (u64)e->buf, e->size);
        sys_call(SYS_CLOSE, fd, 0, 0);
        if (w == e->size) {
            snprintf(e->saved_msg, sizeof e->saved_msg, "wrote %d bytes", w);
            e->dirty = false;
        } else {
            snprintf(e->saved_msg, sizeof e->saved_msg, "short write (%d/%d)", w, e->size);
        }
    }
    e->saved_msg_until = 200;
}

static void ed_insert(ed_t *e, char c) {
    if (e->size + 1 >= (int)sizeof e->buf) return;
    memmove(&e->buf[e->caret + 1], &e->buf[e->caret], e->size - e->caret);
    e->buf[e->caret++] = c;
    e->size++;
    e->buf[e->size] = 0;
    e->dirty = true;
}

static void ed_backspace(ed_t *e) {
    if (e->caret == 0) return;
    memmove(&e->buf[e->caret - 1], &e->buf[e->caret], e->size - e->caret);
    e->caret--;
    e->size--;
    e->buf[e->size] = 0;
    e->dirty = true;
}

static void ed_paint(window_t *w) {
    ed_t *e = w->ud;
    int x = w->x, y = w->y + WIN_TITLE_H;
    int W = w->w, H = w->h - WIN_TITLE_H;

    /* status bar at top */
    draw_rect(x, y, W, 22, TH_EDITOR_GUTTER);
    char top[120];
    snprintf(top, sizeof top, " %s%s",
             e->path[0] ? e->path : "(unnamed)",
             e->dirty ? " *" : "");
    draw_text(x + 8, y + 3, top, TH_TEXT, 0);
    const char *help = "^S save  ^Q quit";
    draw_text(x + W - text_width(help) - 10, y + 3, help, TH_TEXT_DIM, 0);
    draw_hline(x, y + 21, W, TH_WIN_BORDER);

    /* text area */
    int tx = x + 44;
    int ty = y + 26;
    int avail_h = H - 50;
    int rows = avail_h / FONT_H;
    int lines = line_count(e);

    /* keep caret visible */
    int caret_line = line_index(e, e->caret);
    if (caret_line < e->scroll) e->scroll = caret_line;
    if (caret_line >= e->scroll + rows) e->scroll = caret_line - rows + 1;
    if (e->scroll < 0) e->scroll = 0;

    /* gutter */
    draw_rect(x, y + 22, 38, H - 22, TH_EDITOR_GUTTER);
    draw_vline(x + 38, y + 22, H - 22, TH_WIN_BORDER);

    /* draw lines */
    int p = 0, ln = 0;
    while (p < e->size && ln < e->scroll) {
        if (e->buf[p++] == '\n') ln++;
    }
    int draw_y = ty;
    int caret_x = -1, caret_y = -1;
    for (int r = 0; r < rows; r++) {
        char num[8];
        snprintf(num, sizeof num, "%4d", e->scroll + r + 1);
        if (e->scroll + r < lines)
            draw_text(x + 4, draw_y, num, TH_EDITOR_LINE, 0);
        int line_p = p;
        int col = 0;
        while (p < e->size && e->buf[p] != '\n') {
            char ch = e->buf[p];
            if (ch == '\t') ch = ' ';
            int gx = tx + col * FONT_W;
            if (gx + FONT_W < x + W - 4)
                draw_char(gx, draw_y, ch, TH_EDITOR_FG, 0xFF000000);
            if (p == e->caret) { caret_x = gx; caret_y = draw_y; }
            p++; col++;
        }
        /* if caret sits at end-of-line on this row */
        if (e->caret == p && line_p <= e->caret &&
            (p == e->size || e->buf[p] == '\n')) {
            caret_x = tx + col * FONT_W;
            caret_y = draw_y;
        }
        if (p < e->size && e->buf[p] == '\n') p++;
        draw_y += FONT_H;
        if (p >= e->size) break;
    }
    /* caret */
    if (caret_x >= 0 && ((pit_ticks() / 30) & 1)) {
        draw_vline(caret_x, caret_y, FONT_H, TH_EDITOR_CARET);
        draw_vline(caret_x + 1, caret_y, FONT_H, TH_EDITOR_CARET);
    }

    /* bottom status */
    int sy = y + H - 22;
    draw_rect(x, sy, W, 22, TH_EDITOR_GUTTER);
    draw_hline(x, sy, W, TH_WIN_BORDER);
    char st[120];
    snprintf(st, sizeof st, "Ln %d  Col %d  %d bytes",
             caret_line + 1,
             e->caret - line_start(e, e->caret) + 1,
             e->size);
    draw_text(x + 8, sy + 3, st, TH_TEXT_DIM, 0);
    if (e->saved_msg_until > 0) {
        draw_text(x + W - text_width(e->saved_msg) - 10, sy + 3,
                  e->saved_msg, TH_ACCENT, 0);
        e->saved_msg_until--;
    }
}

static void ed_on_key(window_t *w, int sc, char ch, u32 mods) {
    ed_t *e = w->ud;

    /* --- Ctrl combos go FIRST and always return so the printable letter
       does not slip into the buffer (this was the Stage-9 Ctrl+S bug) --- */
    if (mods & KEY_CTRL) {
        if (ch == 's' || ch == 'S') { ed_save(e); return; }
        if (ch == 'q' || ch == 'Q') { window_close_requested(w); return; }
        /* swallow any other ctrl-letter */
        if (ch >= ' ' && ch < 127) return;
    }

    /* arrow keys + navigation */
    switch (sc) {
    case 0x4B:                                       /* left */
        if (e->caret > 0) e->caret--; return;
    case 0x4D:                                       /* right */
        if (e->caret < e->size) e->caret++; return;
    case 0x48: {                                     /* up */
        int col = e->caret - line_start(e, e->caret);
        int prev_end = line_start(e, e->caret) - 1;
        if (prev_end < 0) return;
        int prev_start = line_start(e, prev_end);
        int prev_len = prev_end - prev_start;
        e->caret = prev_start + (col < prev_len ? col : prev_len);
        return;
    }
    case 0x50: {                                     /* down */
        int col = e->caret - line_start(e, e->caret);
        int next_start = line_end(e, e->caret) + 1;
        if (next_start > e->size) return;
        int next_end = line_end(e, next_start);
        int next_len = next_end - next_start;
        e->caret = next_start + (col < next_len ? col : next_len);
        return;
    }
    case 0x47: e->caret = line_start(e, e->caret); return;  /* home */
    case 0x4F: e->caret = line_end(e, e->caret);   return;  /* end */
    }

    if (ch == 0) return;
    if (ch == '\b' || ch == 8) { ed_backspace(e); return; }
    if (ch == '\n') { ed_insert(e, '\n'); return; }
    if (ch >= ' ' && ch < 127) { ed_insert(e, ch); return; }
}

void open_editor(const char *path) {
    ed_t *e = kzalloc(sizeof *e);
    if (path) strncpy(e->path, path, sizeof e->path - 1);
    else      strncpy(e->path, "/tmp/untitled.txt", sizeof e->path - 1);
    ed_load(e);

    char title[100];
    snprintf(title, sizeof title, "Editor - %s", e->path);
    int w = 720, h = 460;
    int x = 60 + (g_fb.width - w) / 2 - 40;
    int y = 100;
    window_t *win = window_create(title, ICON_EDITOR, x, y, w, h, ed_paint);
    if (!win) { kfree(e); return; }
    win->ud = e;
    win->on_key = ed_on_key;
    win->bg = TH_EDITOR_BG;
    win->min_w = 480; win->min_h = 280;
    win->flags |= WIN_ANIM;
}
