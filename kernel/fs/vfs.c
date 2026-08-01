/* Yart OS - writable VFS over an initial USTAR initrd.
 *
 * Each vnode owns its data on the kernel heap.  USTAR contents are copied
 * in at boot so the in-memory tree is mutable from then on.  This is the
 * substrate that user-space programs hit through open/read/write/mkdir/
 * unlink syscalls.
 */
#include <yart/fs.h>
#include <yart/blk.h>
#include <yart/icons.h>
#include <yart/mm.h>
#include <yart/string.h>
#include <yart/console.h>
#include <yart/hal.h>
#include <yart/sched.h>   /* sched_current_euid() for file ownership */

typedef struct PACKED {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char type;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
} ustar_hdr_t;

static vnode_t *root_node;

static u64 now_epoch(void) {
    rtc_time_t t; rtc_read(&t);
    return ((u64)t.year * 10000 + t.month * 100 + t.day) * 1000000ULL +
           (u64)(t.hour * 10000 + t.minute * 100 + t.second);
}

static u64 oct(const char *s, size_t n) {
    u64 v = 0;
    for (size_t i = 0; i < n && s[i] && s[i] != ' '; i++) {
        if (s[i] < '0' || s[i] > '7') break;
        v = v * 8 + (s[i] - '0');
    }
    return v;
}

static vnode_t *mknode(const char *name, vnode_type_t t) {
    vnode_t *v = kzalloc(sizeof *v);
    strncpy(v->name, name, VFS_MAX_NAME - 1);
    v->type = t;
    v->mtime = now_epoch();
    v->icon  = (icon_id_t)-1;    /* -1 = auto-detect from extension */
    v->refs  = 1;                /* owned by the tree until unlinked  */
    v->uid   = sched_current_euid();  /* creator owns the file (0=boot) */
    v->gid   = sched_current_egid();
    v->mode  = (t == VN_DIR) ? 0755u : 0644u;
    {
        u16 um = sched_current_umask();
        v->mode &= (u16)~um;                 /* umask strips bits at create */
    }
    return v;
}

bool vfs_check_perm(vnode_t *v, u32 euid, const u32 *groups, int ngroups, int want) {
    if (!v) return false;
    if (euid == 0) return true;              /* root bypasses everything  */
    /* ACL: an explicit per-user grant is checked FIRST and overrides the
     * mode bits (POSIX ACL semantics for the named-user class) */
    for (int i = 0; i < v->acl_count && i < VFS_ACL_MAX; i++)
        if (v->acl[i].uid == euid)
            return (v->acl[i].mask & want) == want;
    int bits;
    if (v->uid == euid)        bits = (v->mode >> 6) & PERM_RWX;  /* owner */
    else {
        bool in_group = false;
        for (int g = 0; g < ngroups; g++)
            if (v->gid == groups[g]) { in_group = true; break; }
        bits = in_group ? ((v->mode >> 3) & PERM_RWX)          /* group */
                        : ((v->mode >> 0) & PERM_RWX);         /* other */
    }
    return (bits & want) == want;
}

void vnode_ref(vnode_t *v) { if (v) v->refs++; }

void vnode_unref(vnode_t *v) {
    if (!v || v->refs == 0) return;
    if (--v->refs == 0) {
        if (v->data) kfree(v->data);
        kfree(v);
    }
}

static void attach(vnode_t *parent, vnode_t *child) {
    child->parent  = parent;
    /* keep alphabetical-ish: append at tail so order matches insertion */
    if (!parent->child) {
        parent->child = child;
    } else {
        vnode_t *t = parent->child;
        while (t->sibling) t = t->sibling;
        t->sibling = child;
    }
}

static vnode_t *find_child(vnode_t *p, const char *name) {
    if (!p || p->type != VN_DIR) return NULL;
    for (vnode_t *c = p->child; c; c = c->sibling)
        if (strcmp(c->name, name) == 0) return c;
    return NULL;
}

static vnode_t *find_or_make_dir(vnode_t *p, const char *name) {
    vnode_t *e = find_child(p, name);
    if (e && e->type == VN_DIR) return e;
    vnode_t *d = mknode(name, VN_DIR);
    attach(p, d);
    return d;
}

vnode_t *vfs_create(vnode_t *parent, const char *name, vnode_type_t t) {
    if (!parent || parent->type != VN_DIR) return NULL;
    if (find_child(parent, name)) return NULL;
    vnode_t *n = mknode(name, t);
    n->dirty = true;                       /* needs persisting to disk     */
    n->dirty_blocks = 0xFFFFFFFFu;
    attach(parent, n);
    return n;
}

static void insert_path(const char *path, vnode_type_t type,
                        const void *data, size_t size) {
    vnode_t *cur = root_node;
    char part[VFS_MAX_NAME];
    const char *s = path;
    while (*s == '/') s++;
    /* strip leading "./" components produced by tar */
    while (s[0] == '.' && (s[1] == 0 || s[1] == '/')) {
        s++; while (*s == '/') s++;
    }
    while (*s) {
        size_t i = 0;
        while (*s && *s != '/' && i < VFS_MAX_NAME - 1) part[i++] = *s++;
        part[i] = 0;
        if (i == 0) break;
        bool last = (*s == 0) || (*s == '/' && *(s+1) == 0);
        if (*s == '/') s++;
        if (strcmp(part, ".") == 0) continue;   /* skip nested ./ */
        if (!last || type == VN_DIR) {
            cur = find_or_make_dir(cur, part);
        } else {
            vnode_t *f = find_child(cur, part);
            if (!f) { f = mknode(part, VN_FILE); attach(cur, f); }
            if (size) {
                f->data = kmalloc(size);
                f->cap  = size;
                f->size = size;
                memcpy(f->data, data, size);
            }
            return;
        }
    }
}

void vfs_init(void *initrd, size_t total) {
    root_node = mknode("/", VN_DIR);
    /* synthesize the basic FHS skeleton so users can cd somewhere sane */
    find_or_make_dir(root_node, "bin");
    find_or_make_dir(root_node, "etc");
    find_or_make_dir(root_node, "home");
    find_or_make_dir(find_or_make_dir(root_node, "home"), "yart");
    find_or_make_dir(root_node, "tmp");

    if (!initrd || total < 512) {
        kprintf("vfs: no initrd, empty FS skeleton ready\n");
        return;
    }
    u8 *p = initrd;
    u8 *end = p + total;
    int count = 0;
    while (p + 512 <= end) {
        ustar_hdr_t *h = (ustar_hdr_t *)p;
        if (h->name[0] == 0) break;
        if (memcmp(h->magic, "ustar", 5) != 0) break;
        u64 sz = oct(h->size, sizeof h->size);
        char path[256] = {0};
        if (h->prefix[0]) {
            size_t pl = strlen(h->prefix);
            if (pl > sizeof path - 2) pl = sizeof path - 2;
            memcpy(path, h->prefix, pl);
            path[pl] = '/';
            strncpy(path + pl + 1, h->name, sizeof path - pl - 2);
        } else {
            strncpy(path, h->name, sizeof path - 1);
        }
        vnode_type_t t = (h->type == '5' || path[strlen(path) - 1] == '/')
                         ? VN_DIR : VN_FILE;
        insert_path(path, t, p + 512, sz);
        count++;
        u64 blocks = (sz + 511) / 512;
        p += 512 + blocks * 512;
    }
    kprintf("vfs: imported %d entries from initrd\n", count);
}

vnode_t *vfs_root(void) { return root_node; }

vnode_t *vfs_lookup_at(vnode_t *cwd, const char *path) {
    vnode_t *cur = (path && path[0] == '/') ? root_node : (cwd ? cwd : root_node);
    if (!path || !*path || (path[0] == '/' && path[1] == 0)) return cur;
    char part[VFS_MAX_NAME];
    const char *s = path;
    if (*s == '/') s++;
    while (*s) {
        size_t i = 0;
        while (*s && *s != '/' && i < VFS_MAX_NAME - 1) part[i++] = *s++;
        part[i] = 0;
        while (*s == '/') s++;
        if (i == 0) continue;
        if (strcmp(part, ".") == 0) continue;
        if (strcmp(part, "..") == 0) {
            if (cur->parent) cur = cur->parent;
            continue;
        }
        vnode_t *next = find_child(cur, part);
        if (!next) return NULL;
        cur = next;
    }
    return cur;
}

vnode_t *vfs_lookup(const char *path) { return vfs_lookup_at(NULL, path); }

int vfs_read(vnode_t *v, void *buf, size_t off, size_t n) {
    if (!v || v->type != VN_FILE) return -1;
    if (off >= v->size) return 0;
    if (off + n > v->size) n = v->size - off;
    if (n) memcpy(buf, (u8 *)v->data + off, n);
    return (int)n;
}

static int ensure_cap(vnode_t *v, size_t need) {
    if (need <= v->cap) return 0;
    size_t newcap = v->cap ? v->cap : 64;
    while (newcap < need) newcap *= 2;
    void *nb = kmalloc(newcap);
    if (!nb) return -1;
    if (v->data && v->size) memcpy(nb, v->data, v->size);
    if (v->data) kfree(v->data);
    v->data = nb;
    v->cap  = newcap;
    return 0;
}

int vfs_write(vnode_t *v, const void *buf, size_t off, size_t n) {
    if (!v || v->type != VN_FILE) return -1;
    if (ensure_cap(v, off + n) < 0) return -1;
    if (off > v->size) memset((u8 *)v->data + v->size, 0, off - v->size);
    memcpy((u8 *)v->data + off, buf, n);
    if (off + n > v->size) v->size = off + n;
    v->mtime = now_epoch();
    v->dirty = true;
    /* mark the affected 512-byte blocks dirty (incremental disk sync) */
    {
        u32 b0 = (u32)(off / BLK_SECTOR_SIZE);
        u32 b1 = (u32)((off + n + BLK_SECTOR_SIZE - 1) / BLK_SECTOR_SIZE);
        for (u32 b = b0; b < b1 && b < 32; b++) v->dirty_blocks |= (1u << b);
    }
    return (int)n;
}

int vfs_truncate(vnode_t *v, size_t n) {
    if (!v || v->type != VN_FILE) return -1;
    if (n > v->size) {
        if (ensure_cap(v, n) < 0) return -1;
        memset((u8 *)v->data + v->size, 0, n - v->size);
    }
    v->size = n;
    v->mtime = now_epoch();
    v->dirty = true;
    v->dirty_blocks = 0xFFFFFFFFu;   /* truncate: rewrite every block */
    return 0;
}

int vfs_unlink(vnode_t *v) {
    if (!v || !v->parent) return -1;
    /* don't allow non-empty dir delete */
    if (v->type == VN_DIR && v->child) return -1;
    {
        char pth[VFS_MAX_PATH];
        if (vfs_path_of(v, pth, sizeof pth) > 0)
            blkfs_note_delete(pth);        /* remove from disk on next sync */
    }
    vnode_t *p = v->parent;
    if (p->child == v) p->child = v->sibling;
    else {
        for (vnode_t *c = p->child; c; c = c->sibling)
            if (c->sibling == v) { c->sibling = v->sibling; break; }
    }
    v->sibling = NULL;
    v->parent  = NULL;          /* detached: no longer reachable by lookup */
    vnode_unref(v);             /* drop the tree's reference; freed only
                                   when the last open fd closes it too */
    return 0;
}

int vfs_mkdir_p(const char *path) {
    vnode_t *cur = root_node;
    const char *s = path;
    if (*s == '/') s++;
    char part[VFS_MAX_NAME];
    while (*s) {
        size_t i = 0;
        while (*s && *s != '/' && i < VFS_MAX_NAME - 1) part[i++] = *s++;
        part[i] = 0;
        while (*s == '/') s++;
        if (i == 0) continue;
        vnode_t *e = find_child(cur, part);
        if (!e) e = vfs_create(cur, part, VN_DIR);
        else if (e->type != VN_DIR) return -1;
        cur = e;
    }
    return 0;
}

int vfs_path_of(vnode_t *v, char *out, size_t cap) {
    if (!v) return -1;
    if (v == root_node) { strncpy(out, "/", cap); return 1; }
    char tmp[VFS_MAX_PATH] = {0};
    int pos = sizeof tmp - 1;
    tmp[pos] = 0;
    for (vnode_t *c = v; c && c != root_node; c = c->parent) {
        size_t l = strlen(c->name);
        if (pos - (int)l - 1 < 0) return -1;
        pos -= l;
        memcpy(&tmp[pos], c->name, l);
        pos--;
        tmp[pos] = '/';
    }
    size_t len = sizeof tmp - 1 - pos;
    if (len + 1 > cap) return -1;
    memcpy(out, &tmp[pos], len);
    out[len] = 0;
    return (int)len;
}

size_t vfs_count_children(vnode_t *dir) {
    if (!dir || dir->type != VN_DIR) return 0;
    size_t n = 0;
    for (vnode_t *c = dir->child; c; c = c->sibling) n++;
    return n;
}

static void dump_rec(vnode_t *v, int depth) {
    for (int i = 0; i < depth; i++) kprintf("  ");
    kprintf("%s%s  (%lu B)\n", v->name, v->type == VN_DIR ? "/" : "", v->size);
    if (v->type == VN_DIR) for (vnode_t *c = v->child; c; c = c->sibling) dump_rec(c, depth + 1);
}
void vfs_dump(void) { dump_rec(root_node, 0); }
