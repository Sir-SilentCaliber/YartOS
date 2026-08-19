#pragma once
#include <yart/types.h>

#define VFS_MAX_NAME 96
#define VFS_MAX_PATH 256

/* Permission bits (octal-style, low 9 bits of mode): owner / group / other,
 * each rwx (4=r, 2=w, 1=x).  uid 0 (root) bypasses all checks. */
#define PERM_R 4
#define PERM_W 2
#define PERM_X 1
#define PERM_RWX 7
#define PERM_STICKY 01000  /* only owner/root may delete/rename in a sticky dir */
#define VFS_ACL_MAX 8      /* per-file access-control entries           */
typedef struct { u32 uid; u16 mask; } vfs_acl_entry_t; /* mask = PERM_* bits */

typedef enum { VN_FILE = 1, VN_DIR = 2, VN_SYMLINK = 3 } vnode_type_t;

typedef int icon_id_t;   /* icons live in ring-3 now; kernel stores int */

typedef struct vnode {
    char         name[VFS_MAX_NAME];
    vnode_type_t type;
    u32          ino;         /* on-disk inode number (0 = not yet persisted) */
    size_t       size;        /* bytes for files, 0 for dirs              */
    size_t       cap;         /* allocated capacity for files             */
    void        *data;        /* heap-owned for files                     */
    u64          mtime;        /* unix-ish epoch from RTC                  */
    icon_id_t    icon;        /* custom icon override (-1 = auto-detect)  */
    u32          refs;        /* 1 while attached to the tree + 1 per open fd */
    u32          uid;         /* owning user (0 = root)                   */
    u32          gid;         /* group (stored; not enforced yet)         */
    u16          mode;        /* 9 permission bits (0644 default)         */
    vfs_acl_entry_t acl[VFS_ACL_MAX]; /* explicit per-user grants          */
    u8           acl_count;
    bool         dirty;       /* needs persisting to disk by blkfs_sync()  */
    u32          dirty_b0;    /* first dirty 512B block (half-open range,  */
    u32          dirty_b1;    /* supports files > 32 blocks via indirect) */
    struct vnode *parent;
    struct vnode *child;       /* first child if dir                       */
    struct vnode *sibling;     /* next sibling                             */
} vnode_t;

/* Reference counting: vfs_unlink() detaches a node from the tree and drops
 * the tree's reference; the node is only freed once every open fd (which
 * holds its own reference) has closed.  This prevents the use-after-free
 * where rm'ing a file corrupted a still-open file descriptor. */
void vnode_ref(vnode_t *v);
void vnode_unref(vnode_t *v);

/* Coarse VFS tree lock (recursive, IRQ-safe).  Every public vfs_* entry
 * takes it; syscall paths may nest (e.g. blkfs_sync -> vfs_path_of). */
void vfs_lock(void);
void vfs_unlock(void);

void     vfs_init(void *initrd, size_t size);
vnode_t *vfs_root(void);
vnode_t *vfs_lookup(const char *path);
vnode_t *vfs_lookup_at(vnode_t *cwd, const char *path);
/* No-follow variants: return the symlink node itself instead of its target
 * (used by unlink/rename/stat, which must act on the link, not the target). */
vnode_t *vfs_lookup_nofollow(const char *path);
vnode_t *vfs_lookup_at_nofollow(vnode_t *cwd, const char *path);
/* Create a symlink whose target is `target`. */
vnode_t *vfs_symlink(vnode_t *parent, const char *name, const char *target);

/* file ops */
int  vfs_read   (vnode_t *v, void *buf, size_t off, size_t n);
int  vfs_write  (vnode_t *v, const void *buf, size_t off, size_t n);
int  vfs_truncate(vnode_t *v, size_t n);

/* directory ops */
vnode_t *vfs_create(vnode_t *parent, const char *name, vnode_type_t t);
int      vfs_unlink(vnode_t *v);
int      vfs_mkdir_p(const char *path);     /* mkdir -p relative to /     */

/* helpers */
void  vfs_dump(void);
int   vfs_path_of(vnode_t *v, char *out, size_t cap);
size_t vfs_count_children(vnode_t *dir);
vnode_t *vfs_find_child(vnode_t *parent, const char *name);  /* NULL if absent */

/* Permission check: does `euid` have all `want` bits (PERM_R/W/X) on `v`?
 * uid 0 always passes; owner bits apply when v->uid == euid, otherwise the
 * "other" bits (group support comes later). */
bool vfs_check_perm(vnode_t *v, u32 euid, const u32 *groups, int ngroups, int want);
