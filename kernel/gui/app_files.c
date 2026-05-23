/* Yart OS - File Manager (GNOME Files-ish).
 *
 * Layout:
 *   +-----+--------------------------------------------+
 *   | Side| Path bar  [up] [home] [/etc]               |
 *   | bar +--------------------------------------------+
 *   |     | Icon grid of folder contents               |
 *   |     |                                            |
 *   +-----+--------------------------------------------+
 *
 * Everything here goes through syscalls (vfs_lookup_at, vfs_create, ...
 * via the in-kernel API; we do the same operations user-space would do).
 */
#include <yart/gui.h>
#include <yart/theme.h>
#include <yart/icons.h>
#include <yart/string.h>
#include <yart/mm.h>
#include <yart/fs.h>
#include <yart/task.h>

extern void open_editor(const char *path);

#define SIDE_W       150
#define ITEM_W       96
#define ITEM_H       80

typedef struct {
    char     path[VFS_MAX_PATH];
    int      sel;
    int      hover;
    bool     prev_left;
} files_state_t;

typedef struct {
    const char *label;
    icon_id_t   icon;
    const char *path;
} bookmark_t;

static const bookmark_t bookmarks[] = {
    { "Home",     ICON_HOME,   "/home/yart" },
    { "Files",    ICON_FILES,  "/" },
    { "Etc",      ICON_CONFIG, "/etc" },
    { "Tmp",      ICON_FOLDER, "/tmp" },
    { "Bin",      ICON_FOLDER, "/bin" },
};
#define N_BOOKMARKS (int)(sizeof bookmarks / sizeof bookmarks[0])

static void files_paint(window_t *w) {
    files_state_t *st = w->ud;
    int x = w->x, y = w->y + WIN_TITLE_H;
    int W = w->w, H = w->h - WIN_TITLE_H;

    /* ---- sidebar ---- */
    draw_rect(x, y, SIDE_W, H, TH_WIN_BG_ALT);
    draw_vline(x + SIDE_W - 1, y, H, TH_WIN_BORDER);
    draw_text(x + 12, y + 12, "Places", TH_TEXT_DIM, 0);
    int sy = y + 36;
    int cx, cy; cursor_get_pos(&cx, &cy);
    for (int i = 0; i < N_BOOKMARKS; i++) {
        int by = sy + i * 30;
        bool hover = (cx >= x && cx < x + SIDE_W && cy >= by && cy < by + 28);
        bool active = strcmp(bookmarks[i].path, st->path) == 0;
        if (hover || active)
            draw_rect(x + 4, by - 2, SIDE_W - 8, 26,
                      active ? TH_ACCENT_BG : TH_PANEL_HI);
        draw_icon_sized(x + 8, by - 2, bookmarks[i].icon, 22);
        draw_text(x + 38, by + 4, bookmarks[i].label,
                  active ? TH_ACCENT : TH_TEXT, 0);
    }

    /* ---- main area ---- */
    int mx = x + SIDE_W;
    int my = y;
    int mw = W - SIDE_W;
    /* path bar */
    int pby = my;
    draw_rect(mx, pby, mw, 36, TH_WIN_BG);
    draw_hline(mx, pby + 35, mw, TH_WIN_BORDER);
    /* up button */
    int up_x = mx + 8, up_w = 26;
    bool up_hover = (cx >= up_x && cx < up_x + up_w && cy >= pby + 6 && cy < pby + 30);
    draw_rounded_rect(up_x, pby + 6, up_w, 24, 4,
                      up_hover ? TH_PANEL_HI : TH_WIN_BG_ALT);
    draw_rounded_rect_outline(up_x, pby + 6, up_w, 24, 4, TH_WIN_BORDER);
    draw_text(up_x + 9, pby + 10, "<", TH_TEXT, 0);
    /* path */
    draw_text(up_x + up_w + 12, pby + 11, st->path, TH_TEXT, 0);

    /* contents */
    vnode_t *dir = vfs_lookup(st->path);
    if (!dir || dir->type != VN_DIR) {
        draw_text(mx + 12, my + 50, "(not a directory)", TH_ERR, 0);
        return;
    }
    int gx = mx + 16, gy = my + 50;
    int cols = (mw - 24) / ITEM_W;
    if (cols < 1) cols = 1;
    int idx = 0;
    st->hover = -1;
    for (vnode_t *c = dir->child; c; c = c->sibling, idx++) {
        int col = idx % cols, row = idx / cols;
        int ix = gx + col * ITEM_W;
        int iy = gy + row * ITEM_H;
        if (iy + ITEM_H > my + H - 8) break;
        bool hover = (cx >= ix && cx < ix + ITEM_W &&
                      cy >= iy && cy < iy + ITEM_H);
        if (hover) st->hover = idx;
        if (hover || idx == st->sel)
            draw_rounded_rect(ix + 2, iy + 2, ITEM_W - 4, ITEM_H - 4, 6,
                              idx == st->sel ? TH_ACCENT_BG : TH_PANEL_HI);
        icon_id_t ic = (c->type == VN_DIR) ? ICON_FOLDER : ICON_FILE;
        draw_icon(ix + (ITEM_W - 32) / 2, iy + 8, ic);
        /* truncate name */
        char buf[16];
        int n = strlen(c->name); if (n > 13) n = 13;
        memcpy(buf, c->name, n); buf[n] = 0;
        if (n == 13) { buf[10]='.'; buf[11]='.'; buf[12]='.'; }
        int tw = text_width(buf);
        draw_text(ix + (ITEM_W - tw) / 2, iy + ITEM_H - 22, buf,
                  idx == st->sel ? TH_ACCENT : TH_TEXT, 0);
    }
    /* status line */
    char status[80];
    snprintf(status, sizeof status, "%lu items", vfs_count_children(dir));
    draw_text(mx + 12, my + H - 18, status, TH_TEXT_DIM, 0);
}

static void files_navigate(files_state_t *st, const char *target) {
    if (target[0] == '/') {
        strncpy(st->path, target, sizeof st->path - 1);
        st->path[sizeof st->path - 1] = 0;
    } else if (strcmp(target, "..") == 0) {
        if (strcmp(st->path, "/") == 0) return;
        char *slash = NULL;
        for (char *p = st->path; *p; p++) if (*p == '/') slash = p;
        if (slash == st->path) slash[1] = 0;
        else *slash = 0;
    } else {
        char tmp[VFS_MAX_PATH];
        if (strcmp(st->path, "/") == 0) snprintf(tmp, sizeof tmp, "/%s", target);
        else snprintf(tmp, sizeof tmp, "%s/%s", st->path, target);
        strncpy(st->path, tmp, sizeof st->path - 1);
    }
    st->sel = -1;
}

static void files_on_key(window_t *w, int sc, char ch, u32 mods) {
    (void)mods;
    files_state_t *st = w->ud;
    if (sc == 0x0E /* backspace */ || ch == 8) {
        files_navigate(st, "..");
    } else if (ch == '\n') {
        if (st->sel < 0) return;
        vnode_t *dir = vfs_lookup(st->path);
        if (!dir) return;
        int idx = 0;
        for (vnode_t *c = dir->child; c; c = c->sibling, idx++) {
            if (idx != st->sel) continue;
            if (c->type == VN_DIR) files_navigate(st, c->name);
            else {
                char p[VFS_MAX_PATH];
                snprintf(p, sizeof p, "%s%s%s",
                         st->path, strcmp(st->path,"/")==0?"":"/", c->name);
                open_editor(p);
            }
            return;
        }
    }
}

/* hack: routed via desktop_handle_mouse -> active window's paint can't
   easily handle clicks.  We reuse the per-frame state via a tiny wrapper
   that the desktop knows nothing about: track cursor + click edges in
   files state itself by sampling each paint. */
static void files_pre_render(window_t *w) {
    files_state_t *st = w->ud;
    int cx, cy; cursor_get_pos(&cx, &cy);
    /* primary input: detect click on bookmarks / up button / items by
       diffing buttons across paints isn't possible w/o a real input
       hook; instead, rely on desktop_handle_mouse global state to call
       us through a "mouse" callback.  Simplest: poll button state via
       extern. */
    UNUSED(st); UNUSED(cx); UNUSED(cy);
}

/* The compositor doesn't have a per-window mouse callback yet; we add
   the hook directly into desktop's input path.  See on_files_click()
   exported below. */
extern int g_files_click_x, g_files_click_y;
extern bool g_files_click_pending;

static void files_paint_with_clicks(window_t *w) {
    files_state_t *st = w->ud;

    files_paint(w);

    if (!g_files_click_pending) return;
    int cx = g_files_click_x, cy = g_files_click_y;
    g_files_click_pending = false;

    int x = w->x, y = w->y + WIN_TITLE_H;
    int W = w->w, H = w->h - WIN_TITLE_H;

    /* sidebar bookmark click */
    int sy = y + 36;
    for (int i = 0; i < N_BOOKMARKS; i++) {
        int by = sy + i * 30;
        if (cx >= x && cx < x + SIDE_W && cy >= by && cy < by + 28) {
            files_navigate(st, bookmarks[i].path);
            return;
        }
    }

    int mx = x + SIDE_W;
    int my = y;
    int mw = W - SIDE_W;

    /* up button */
    int up_x = mx + 8;
    if (cx >= up_x && cx < up_x + 26 && cy >= my + 6 && cy < my + 30) {
        files_navigate(st, "..");
        return;
    }

    /* item grid */
    int gx = mx + 16, gy = my + 50;
    int cols = (mw - 24) / ITEM_W; if (cols < 1) cols = 1;
    vnode_t *dir = vfs_lookup(st->path);
    if (!dir) return;
    int idx = 0;
    for (vnode_t *c = dir->child; c; c = c->sibling, idx++) {
        int col = idx % cols, row = idx / cols;
        int ix = gx + col * ITEM_W;
        int iy = gy + row * ITEM_H;
        if (iy + ITEM_H > my + H - 8) break;
        if (cx >= ix && cx < ix + ITEM_W && cy >= iy && cy < iy + ITEM_H) {
            if (st->sel == idx) {
                /* double-ish click: open it */
                if (c->type == VN_DIR) files_navigate(st, c->name);
                else {
                    char p[VFS_MAX_PATH];
                    snprintf(p, sizeof p, "%s%s%s",
                             st->path, strcmp(st->path,"/")==0?"":"/", c->name);
                    open_editor(p);
                }
                st->sel = -1;
            } else {
                st->sel = idx;
            }
            return;
        }
    }
    /* clicked empty space */
    st->sel = -1;
    UNUSED(files_pre_render);
}

void open_files(const char *path) {
    files_state_t *st = kzalloc(sizeof *st);
    if (!path || !*path) path = "/home/yart";
    strncpy(st->path, path, sizeof st->path - 1);
    st->sel = -1;
    char title[80];
    snprintf(title, sizeof title, "Files - %s", st->path);
    int w = 560, h = 380;
    int x = g_fb.width - w - 30;
    int y = 50;
    window_t *win = window_create(title, ICON_FILES, x, y, w, h, files_paint_with_clicks);
    if (!win) { kfree(st); return; }
    win->ud = st;
    win->on_key = files_on_key;
    win->min_w = 480; win->min_h = 320;
}
