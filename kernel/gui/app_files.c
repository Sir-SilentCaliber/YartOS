/* Yart OS - File Manager with context menus, copy/cut/paste, icon picker.
 *
 * Layout:
 *   +-----+--------------------------------------------+
 *   | Side| Path bar  [up] [home] [/etc]               |
 *   | bar +--------------------------------------------+
 *   |     | Icon grid of folder contents               |
 *   |     |                                            |
 *   +-----+--------------------------------------------+
 */
#include <yart/gui.h>
#include <yart/theme.h>
#include <yart/icons.h>
#include <yart/string.h>
#include <yart/mm.h>
#include <yart/fs.h>
#include <yart/menu.h>
#include <yart/hal.h>

extern void open_editor(const char *path);

#define SIDE_W       150
#define ITEM_W       96
#define ITEM_H       80

/* ---------- clipboard ---------- */
static struct {
    char  src_path[VFS_MAX_PATH];
    char  src_name[VFS_MAX_NAME];
    bool  active;
    bool  is_cut;
} g_clipboard;

/* ---------- drag state (shared with desktop.c) ---------- */
static struct {
    bool  active;
    char  path[VFS_MAX_PATH];
    char  name[VFS_MAX_NAME];
    icon_id_t icon;
    bool  is_dir;
    int   start_x, start_y;
} g_file_drag;

bool files_drag_active(void)          { return g_file_drag.active; }
const char *files_drag_name(void)     { return g_file_drag.name; }
const char *files_drag_path(void)     { return g_file_drag.path; }
icon_id_t   files_drag_icon(void)     { return g_file_drag.icon; }
void        files_drag_cancel(void)   { g_file_drag.active = false; }

/* ---------- per-window state ---------- */
typedef struct {
    char     path[VFS_MAX_PATH];
    int      sel;
    int      hover;
    bool     prev_left;
    bool     right_pending;
    int      right_x, right_y;
    /* icon picker state */
    bool     icon_picker;
    int      icon_picker_target;  /* vnode index being changed */
} files_state_t;

typedef struct {
    const char *label;
    icon_id_t   icon;
    const char *path;
} bookmark_t;

static const bookmark_t bookmarks[] = {
    { "Home",      ICON_HOME,       "/home/yart" },
    { "Files",     ICON_FILES,      "/" },
    { "Etc",       ICON_CONFIG,     "/etc" },
    { "Desktop",   ICON_FOLDER,     "/home/yart/Desktop" },
    { "Documents", ICON_FOLDER_DOC, "/home/yart/Documents" },
    { "Pictures",  ICON_FOLDER_PIC, "/home/yart/Pictures" },
    { "Music",     ICON_FOLDER_MUSIC,"/home/yart/Music" },
    { "Downloads", ICON_FOLDER_DL,  "/home/yart/Downloads" },
};
#define N_BOOKMARKS (int)(sizeof bookmarks / sizeof bookmarks[0])

/* ---------- helpers ---------- */
static vnode_t *get_child_at(vnode_t *dir, int idx) {
    int i = 0;
    for (vnode_t *c = dir->child; c; c = c->sibling, i++)
        if (i == idx) return c;
    return 0;
}

static void build_path(files_state_t *st, const char *name, char *out, int cap) {
    if (strcmp(st->path, "/") == 0)
        snprintf(out, cap, "/%s", name);
    else
        snprintf(out, cap, "%s/%s", st->path, name);
}

/* ---------- icon picker overlay ---------- */
static void draw_icon_picker(window_t *w, files_state_t *st) {
    /* darken background */
    int x0 = w->x, y0 = w->y + WIN_TITLE_H;
    int ww = w->w, hh = w->h - WIN_TITLE_H;
    /* modal panel centered in the window */
    int pw = 340, ph = 260;
    int px = x0 + (ww - pw) / 2;
    int py = y0 + (hh - ph) / 2;
    draw_rounded_rect(px, py, pw, ph, 8, TH_PANEL);
    draw_rounded_rect_outline(px, py, pw, ph, 8, TH_ACCENT_DIM);
    draw_text(px + 16, py + 12, "Change Icon", TH_ACCENT, 0);

    int cx, cy; cursor_get_pos(&cx, &cy);
    int gx = px + 16, gy = py + 40;
    int cols = 8, cell = 36;
    for (int i = 0; i < ICON_COUNT; i++) {
        int col = i % cols, row = i / cols;
        int ix = gx + col * cell;
        int iy = gy + row * cell;
        if (iy + cell > py + ph - 8) break;
        bool hover = (cx >= ix && cx < ix + cell && cy >= iy && cy < iy + cell);
        if (hover) {
            draw_rounded_rect(ix, iy, cell, cell, 4, TH_ACCENT_BG);
            draw_rounded_rect_outline(ix, iy, cell, cell, 4, TH_ACCENT);
        }
        draw_icon_sized(ix + 2, iy + 2, (icon_id_t)i, 32);
    }
    /* close button */
    draw_text(px + pw - 80, py + ph - 24, "[Close]", TH_TEXT_DIM, 0);
}

/* ---------- context menus ---------- */
/* forward decls for menu callbacks */
static void files_navigate(files_state_t *st, const char *target);

static void cm_select_all(void *ud) {
    /* not a real multi-select yet, just toast */
    toast("Select All");
}

static void cm_new_file(void *ud) {
    window_t *w = (window_t *)ud;
    files_state_t *st = w->ud;
    vnode_t *dir = vfs_lookup(st->path);
    if (!dir) return;
    char name[32]; int n = 0;
    for (int i = 1; i < 999; i++) {
        snprintf(name, sizeof name, "New File %d.txt", i);
        if (!vfs_lookup_at(dir, name)) { n = i; break; }
    }
    if (!n) return;
    vnode_t *f = vfs_create(dir, name, VN_FILE);
    if (f) { toast("Created %s", name); wm_dirty(); }
}

static void cm_new_folder(void *ud) {
    window_t *w = (window_t *)ud;
    files_state_t *st = w->ud;
    vnode_t *dir = vfs_lookup(st->path);
    if (!dir) return;
    char name[32]; int n = 0;
    for (int i = 1; i < 999; i++) {
        snprintf(name, sizeof name, "New Folder %d", i);
        if (!vfs_lookup_at(dir, name)) { n = i; break; }
    }
    if (!n) return;
    vnode_t *d = vfs_create(dir, name, VN_DIR);
    if (d) { toast("Created %s", name); wm_dirty(); }
}

static void cm_paste(void *ud) {
    window_t *w = (window_t *)ud;
    files_state_t *st = w->ud;
    if (!g_clipboard.active) { toast("Nothing to paste"); return; }
    vnode_t *dir = vfs_lookup(st->path);
    if (!dir) return;
    vnode_t *src = vfs_lookup(g_clipboard.src_path);
    if (!src) { toast("Source not found"); g_clipboard.active = false; return; }
    /* copy: create new node with same content */
    vnode_t *dst = vfs_create(dir, src->name, src->type);
    if (!dst) { toast("Paste failed"); return; }
    if (src->size > 0 && src->data) {
        dst->data = kzalloc(src->size);
        if (dst->data) {
            memcpy(dst->data, src->data, src->size);
            dst->size = src->size;
            dst->cap = src->size;
        }
    }
    dst->icon = src->icon;
    if (g_clipboard.is_cut) {
        vfs_unlink(src);
        g_clipboard.active = false;
        toast("Moved %s", dst->name);
    } else {
        toast("Copied %s", dst->name);
    }
    wm_dirty();
}

static void cm_copy(void *ud) {
    files_state_t *st = (files_state_t *)((window_t *)ud)->ud;
    vnode_t *dir = vfs_lookup(st->path);
    if (!dir || st->sel < 0) return;
    vnode_t *c = get_child_at(dir, st->sel);
    if (!c) return;
    g_clipboard.active = true;
    g_clipboard.is_cut = false;
    build_path(st, c->name, g_clipboard.src_path, VFS_MAX_PATH);
    strncpy(g_clipboard.src_name, c->name, VFS_MAX_NAME);
    toast("Copied %s", c->name);
}

static void cm_cut(void *ud) {
    files_state_t *st = (files_state_t *)((window_t *)ud)->ud;
    vnode_t *dir = vfs_lookup(st->path);
    if (!dir || st->sel < 0) return;
    vnode_t *c = get_child_at(dir, st->sel);
    if (!c) return;
    g_clipboard.active = true;
    g_clipboard.is_cut = true;
    build_path(st, c->name, g_clipboard.src_path, VFS_MAX_PATH);
    strncpy(g_clipboard.src_name, c->name, VFS_MAX_NAME);
    toast("Cut %s", c->name);
}

static void cm_delete(void *ud) {
    files_state_t *st = (files_state_t *)((window_t *)ud)->ud;
    vnode_t *dir = vfs_lookup(st->path);
    if (!dir || st->sel < 0) return;
    vnode_t *c = get_child_at(dir, st->sel);
    if (!c) return;
    char name[VFS_MAX_NAME];
    strncpy(name, c->name, VFS_MAX_NAME);
    if (vfs_unlink(c) == 0) {
        toast("Deleted %s", name);
        st->sel = -1;
        wm_dirty();
    } else {
        toast("Cannot delete %s", name);
    }
}

static void cm_open(void *ud) {
    files_state_t *st = (files_state_t *)((window_t *)ud)->ud;
    vnode_t *dir = vfs_lookup(st->path);
    if (!dir || st->sel < 0) return;
    vnode_t *c = get_child_at(dir, st->sel);
    if (!c) return;
    if (c->type == VN_DIR) {
        files_navigate(st, c->name);
    } else {
        char p[VFS_MAX_PATH];
        build_path(st, c->name, p, VFS_MAX_PATH);
        open_editor(p);
    }
}

static void cm_change_icon(void *ud) {
    files_state_t *st = (files_state_t *)((window_t *)ud)->ud;
    st->icon_picker = true;
    st->icon_picker_target = st->sel;
}

static void cm_properties(void *ud) {
    files_state_t *st = (files_state_t *)((window_t *)ud)->ud;
    vnode_t *dir = vfs_lookup(st->path);
    if (!dir || st->sel < 0) return;
    vnode_t *c = get_child_at(dir, st->sel);
    if (!c) return;
    toast("%s  %s  %lu bytes", c->name,
          c->type == VN_DIR ? "Folder" : "File", (unsigned long)c->size);
}

/* open context menu for empty space */
static void open_bg_menu(int x, int y, window_t *win) {
    static menu_item_t items[4];
    strncpy(items[0].label, "Select All",    MENU_LABEL_LEN); items[0].on_click = cm_select_all;  items[0].ud = win; items[0].separator = false; items[0].disabled = false;
    strncpy(items[1].label, "New File",       MENU_LABEL_LEN); items[1].on_click = cm_new_file;   items[1].ud = win; items[1].separator = false; items[1].disabled = false;
    strncpy(items[2].label, "New Folder",     MENU_LABEL_LEN); items[2].on_click = cm_new_folder; items[2].ud = win; items[2].separator = false; items[2].disabled = false;
    strncpy(items[3].label, "Paste",          MENU_LABEL_LEN); items[3].on_click = cm_paste;      items[3].ud = win; items[3].separator = false; items[3].disabled = !g_clipboard.active;
    menu_open(x, y, items, 4);
}

/* open context menu for a selected item */
static void open_item_menu(int x, int y, window_t *win) {
    static menu_item_t items[7];
    strncpy(items[0].label, "Open",           MENU_LABEL_LEN); items[0].on_click = cm_open;         items[0].ud = win; items[0].separator = false; items[0].disabled = false;
    strncpy(items[1].label, "Copy",           MENU_LABEL_LEN); items[1].on_click = cm_copy;         items[1].ud = win; items[1].separator = false; items[1].disabled = false;
    strncpy(items[2].label, "Cut",            MENU_LABEL_LEN); items[2].on_click = cm_cut;          items[2].ud = win; items[2].separator = false; items[2].disabled = false;
    strncpy(items[3].label, "Delete",         MENU_LABEL_LEN); items[3].on_click = cm_delete;       items[3].ud = win; items[3].separator = false; items[3].disabled = false;
    items[4].label[0] = 0; items[4].separator = true; items[4].disabled = false; items[4].on_click = 0;
    strncpy(items[5].label, "Change Icon",    MENU_LABEL_LEN); items[5].on_click = cm_change_icon;  items[5].ud = win; items[5].separator = false; items[5].disabled = false;
    strncpy(items[6].label, "Properties",     MENU_LABEL_LEN); items[6].on_click = cm_properties;   items[6].ud = win; items[6].separator = false; items[6].disabled = false;
    menu_open(x, y, items, 7);
}

/* ---------- paint ---------- */
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
    draw_rect(mx, my, mw, 36, TH_WIN_BG);
    draw_hline(mx, my + 35, mw, TH_WIN_BORDER);
    int up_x = mx + 8, up_w = 26;
    bool up_hover = (cx >= up_x && cx < up_x + up_w && cy >= my + 6 && cy < my + 30);
    draw_rounded_rect(up_x, my + 6, up_w, 24, 4,
                      up_hover ? TH_PANEL_HI : TH_WIN_BG_ALT);
    draw_rounded_rect_outline(up_x, my + 6, up_w, 24, 4, TH_WIN_BORDER);
    draw_text(up_x + 9, my + 10, "<", TH_TEXT, 0);
    /* path text */
    draw_text(up_x + up_w + 12, my + 11, st->path, TH_TEXT, 0);

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
        /* pick icon: custom override or auto-detect by extension */
        icon_id_t ic;
        if (c->icon >= 0 && c->icon < ICON_COUNT)
            ic = (icon_id_t)c->icon;
        else
            ic = icon_for_file(c->name, c->type == VN_DIR);
        draw_icon(ix + (ITEM_W - 32) / 2, iy + 8, ic);
        /* truncate name */
        char buf[16];
        int n = (int)strlen(c->name); if (n > 13) n = 13;
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

    /* icon picker overlay */
    if (st->icon_picker)
        draw_icon_picker(w, st);
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
    st->icon_picker = false;
}

static void files_on_key(window_t *w, int sc, char ch, u32 mods) {
    (void)mods;
    files_state_t *st = w->ud;
    if (st->icon_picker) {
        if (sc == 0x01 || ch == 27) { st->icon_picker = false; return; }
        return;
    }
    if (sc == 0x0E || ch == 8) {
        files_navigate(st, "..");
    } else if (ch == '\n') {
        if (st->sel < 0) return;
        vnode_t *dir = vfs_lookup(st->path);
        if (!dir) return;
        vnode_t *c = get_child_at(dir, st->sel);
        if (!c) return;
        if (c->type == VN_DIR) files_navigate(st, c->name);
        else {
            char p[VFS_MAX_PATH];
            build_path(st, c->name, p, VFS_MAX_PATH);
            open_editor(p);
        }
    }
}

/* click routing via desktop */
extern int  g_files_click_x, g_files_click_y;
extern bool g_files_click_pending;
extern int  g_files_right_x, g_files_right_y;
extern bool g_files_right_pending;

/* from desktop.c */
extern void desktop_begin_file_drop(const char *name, const char *path, icon_id_t icon);

static void files_handle_click(window_t *w) {
    files_state_t *st = w->ud;
    int cx = g_files_click_x, cy = g_files_click_y;
    g_files_click_pending = false;

    int x = w->x, y = w->y + WIN_TITLE_H;
    int W = w->w, H = w->h - WIN_TITLE_H;
    int mx = x + SIDE_W, my = y, mw = W - SIDE_W;

    /* ---- icon picker click ---- */
    if (st->icon_picker) {
        int pw = 340, ph = 260;
        int px = x + (W - pw) / 2;
        int py = y + (H - ph) / 2;
        int gx = px + 16, gy = py + 40;
        int cols = 8, cell = 36;
        for (int i = 0; i < ICON_COUNT; i++) {
            int col = i % cols, row = i / cols;
            int ix = gx + col * cell;
            int iy = gy + row * cell;
            if (iy + cell > py + ph - 8) break;
            if (cx >= ix && cx < ix + cell && cy >= iy && cy < iy + cell) {
                /* set icon on target vnode */
                vnode_t *dir = vfs_lookup(st->path);
                if (dir) {
                    vnode_t *c = get_child_at(dir, st->icon_picker_target);
                    if (c) {
                        c->icon = (icon_id_t)i;
                        toast("Icon changed for %s", c->name);
                    }
                }
                st->icon_picker = false;
                return;
            }
        }
        /* close button area */
        if (cx >= px + pw - 80 && cy >= py + ph - 24) {
            st->icon_picker = false;
        }
        return;
    }

    /* ---- sidebar bookmark click ---- */
    int sy = y + 36;
    for (int i = 0; i < N_BOOKMARKS; i++) {
        int by = sy + i * 30;
        if (cx >= x && cx < x + SIDE_W && cy >= by && cy < by + 28) {
            files_navigate(st, bookmarks[i].path);
            return;
        }
    }

    /* ---- up button ---- */
    int up_x = mx + 8;
    if (cx >= up_x && cx < up_x + 26 && cy >= my + 6 && cy < my + 30) {
        files_navigate(st, "..");
        return;
    }

    /* ---- item grid ---- */
    int gx = mx + 16, gy2 = my + 50;
    int cols = (mw - 24) / ITEM_W; if (cols < 1) cols = 1;
    vnode_t *dir = vfs_lookup(st->path);
    if (!dir) return;
    int idx = 0;
    for (vnode_t *c = dir->child; c; c = c->sibling, idx++) {
        int col = idx % cols, row = idx / cols;
        int ix = gx + col * ITEM_W;
        int iy = gy2 + row * ITEM_H;
        if (iy + ITEM_H > my + H - 8) break;
        if (cx >= ix && cx < ix + ITEM_W && cy >= iy && cy < iy + ITEM_H) {
            if (st->sel == idx) {
                /* double-ish click: open it */
                if (c->type == VN_DIR) files_navigate(st, c->name);
                else {
                    char p[VFS_MAX_PATH];
                    build_path(st, c->name, p, VFS_MAX_PATH);
                    open_editor(p);
                }
                st->sel = -1;
            } else {
                st->sel = idx;
            }
            /* start potential drag to desktop (drop only happens on release over bare desktop) */
            {
                char p[VFS_MAX_PATH];
                build_path(st, c->name, p, VFS_MAX_PATH);
                icon_id_t ic = (c->icon >= 0 && c->icon < ICON_COUNT) ? (icon_id_t)c->icon :
                               icon_for_file(c->name, c->type == VN_DIR);
                desktop_begin_file_drop(c->name, p, ic);
            }
            return;
        }
    }
    /* clicked empty space */
    st->sel = -1;
}

/* handle right-click routed from desktop */
static void files_handle_rightclick(window_t *w) {
    files_state_t *st = w->ud;
    int cx = g_files_right_x, cy = g_files_right_y;
    g_files_right_pending = false;

    int x = w->x, y = w->y + WIN_TITLE_H;
    int W = w->w, H = w->h - WIN_TITLE_H;
    int mx = x + SIDE_W, my = y, mw = W - SIDE_W;

    /* check if right-click landed on an item */
    int gx = mx + 16, gy2 = my + 50;
    int cols = (mw - 24) / ITEM_W; if (cols < 1) cols = 1;
    vnode_t *dir = vfs_lookup(st->path);
    if (!dir) return;
    int idx = 0;
    for (vnode_t *c = dir->child; c; c = c->sibling, idx++) {
        int col = idx % cols, row = idx / cols;
        int ix = gx + col * ITEM_W;
        int iy = gy2 + row * ITEM_H;
        if (iy + ITEM_H > my + H - 8) break;
        if (cx >= ix && cx < ix + ITEM_W && cy >= iy && cy < iy + ITEM_H) {
            st->sel = idx;
            open_item_menu(cx, cy, w);
            return;
        }
    }
    /* empty space right-click */
    if (cx >= mx && cx < mx + mw && cy >= my && cy < my + H) {
        st->sel = -1;
        open_bg_menu(cx, cy, w);
    }
}

/* public entry: called each paint frame */
/* (globals defined in desktop.c and shared via externs) */

static void files_paint_entry(window_t *w) {
    files_paint(w);

    if (g_files_click_pending) {
        /* check if click is within our window body */
        int cx = g_files_click_x, cy = g_files_click_y;
        if (cx >= w->x && cx < w->x + w->w &&
            cy >= w->y + WIN_TITLE_H && cy < w->y + w->h)
            files_handle_click(w);
        else
            g_files_click_pending = false;
    }
    if (g_files_right_pending) {
        int cx = g_files_right_x, cy = g_files_right_y;
        if (cx >= w->x && cx < w->x + w->w &&
            cy >= w->y + WIN_TITLE_H && cy < w->y + w->h)
            files_handle_rightclick(w);
        else
            g_files_right_pending = false;
    }

    /* drag initiation is handled through desktop_handle_mouse */
}

void open_files(const char *path) {
    files_state_t *st = kzalloc(sizeof *st);
    if (!path || !*path) path = "/home/yart";
    strncpy(st->path, path, sizeof st->path - 1);
    st->sel = -1;
    st->icon_picker = false;
    char title[80];
    snprintf(title, sizeof title, "Files - %s", st->path);
    int w = 560, h = 380;
    int x = g_fb.width - w - 30;
    int y = 50;
    window_t *win = window_create(title, ICON_FILES, x, y, w, h, files_paint_entry);
    if (!win) { kfree(st); return; }
    win->ud = st;
    win->on_key = files_on_key;
    win->min_w = 480; win->min_h = 320;
}
