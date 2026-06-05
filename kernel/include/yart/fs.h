#pragma once
#include <yart/types.h>

#define VFS_MAX_NAME 96
#define VFS_MAX_PATH 256

typedef enum { VN_FILE = 1, VN_DIR = 2 } vnode_type_t;

/* icon_id_t is defined in icons.h (the real enum with all ICON_* values).
 * Include it here so vnode can use the proper type in any include order. */
#include <yart/icons.h>

typedef struct vnode {
    char         name[VFS_MAX_NAME];
    vnode_type_t type;
    size_t       size;        /* bytes for files, 0 for dirs              */
    size_t       cap;         /* allocated capacity for files             */
    void        *data;        /* heap-owned for files                     */
    u64          mtime;        /* unix-ish epoch from RTC                  */
    icon_id_t    icon;        /* custom icon override (-1 = auto-detect)  */
    struct vnode *parent;
    struct vnode *child;       /* first child if dir                       */
    struct vnode *sibling;     /* next sibling                             */
} vnode_t;

void     vfs_init(void *initrd, size_t size);
vnode_t *vfs_root(void);
vnode_t *vfs_lookup(const char *path);
vnode_t *vfs_lookup_at(vnode_t *cwd, const char *path);

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
