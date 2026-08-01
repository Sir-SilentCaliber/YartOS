/* Yart OS - syscall dispatcher.
 *
 * ABI:  int 0x80
 *   rax = number   rdi = arg0   rsi = arg1   rdx = arg2   r10 = arg3
 *   ret in rax
 *
 * File descriptors, cwd and pid live in the *current scheduler task*; a
 * syscall always runs under the task that issued it.  Kernel-mode callers
 * (the in-kernel terminal re-entering via int 0x80) are trusted and skip
 * user-pointer validation.
 */
#include <yart/types.h>
#include <yart/console.h>
#include <yart/hal.h>
#include <yart/io.h>
#include <yart/string.h>
#include <yart/fs.h>
#include <yart/syscall.h>
#include <yart/user.h>
#include <yart/mm.h>
#include <yart/sched.h>
#include <yart/session.h>
#include <yart/cpu.h>   /* stac/clac for SMAP */   /* session_auth() for doas */

static char g_klog_line[256];
static bool g_sys_from_user;   /* set per syscall from the frame CS */

/* ---------- user pointer validation ---------- */
#define USER_BUF_MAX   (1u << 20)
#define USER_STR_MAX   256

static bool uptr(u64 p, u64 len) {
    if (!g_sys_from_user) return true;              /* kernel caller: trusted */
    if (len > USER_BUF_MAX) return false;
    return vmm_user_range_ok(p, len);
}

/* Permission helper: kernel-mode callers (trusted) always pass; user callers
 * are checked against the current task's effective uid. */
static task_t *cur(void) { return sched_current(); }

static bool perm_ok(vnode_t *v, int want) {
    if (!g_sys_from_user) return true;
    task_t *t = cur();
    u32 groups[8];
    int ng = t ? sched_current_groups(groups, 8) : 1;
    return vfs_check_perm(v, t ? t->euid : 0, groups, ng, want);
}
static u32 cur_euid(void) { task_t *t = cur(); return t ? t->euid : 0; }
static bool ustr(u64 p, u64 max) {
    if (!g_sys_from_user) return true;
    if (max > USER_STR_MAX) return false;
    return vmm_user_str_ok(p, max);
}

/* Validate a user string AND copy it into a kernel buffer, so all the deep
 * vfs path lookups read kernel memory (SMAP-safe). */
static bool copy_user_str(u64 us, char *dst, int cap) {
    if (!ustr(us, cap)) return false;
    stac();
    for (int i = 0; i < cap; i++) {
        char c = ((const char *)us)[i];
        dst[i] = c;
        if (!c) break;
    }
    clac();
    return true;
}

vnode_t *task_cwd(void) { task_t *t = cur(); return t ? t->cwd : NULL; }
void     task_set_cwd(vnode_t *v) {
    task_t *t = cur();
    if (t && v && v->type == VN_DIR) { vnode_ref(v); vnode_unref(t->cwd); t->cwd = v; }
}
int      task_getpid(void) { task_t *t = cur(); return t ? (int)t->pid : 0; }

/* SMP: which CPU is executing this syscall (0 = BSP, 1.. = APs). */
static long sys_getcpu(void) {
    cpu_local_t *c = get_cpu_local();
    return c ? (long)c->cpu_id : 0;
}

static fd_entry_t *fd_get(int fd) {
    task_t *t = cur(); if (!t) return NULL;
    if (fd < 0 || fd >= MAX_FD || !t->fds[fd].in_use) return NULL;
    return &t->fds[fd];
}
static int alloc_fd(void) {
    task_t *t = cur(); if (!t) return -1;
    for (int i = 3; i < MAX_FD; i++) if (!t->fds[i].in_use) return i;
    return -1;
}

/* ---------- syscall handlers ---------- */
static i64 sys_write(int fd, const char *buf, u64 n) {
    if (!uptr((u64)buf, n)) return -1;
    if (fd == 1 || fd == 2) {
        size_t k = n < sizeof g_klog_line - 1 ? n : sizeof g_klog_line - 1;
        stac();
        memcpy(g_klog_line, buf, k);
        clac();
        g_klog_line[k] = 0;
        kputs(g_klog_line);
        return (i64)n;
    }
    fd_entry_t *f = fd_get(fd);
    if (!f) return -1;
    if (!perm_ok(f->vn, PERM_W)) return -1;     /* write permission        */
    stac();
    int w = vfs_write(f->vn, buf, f->pos, n);
    clac();
    if (w > 0) f->pos += w;
    return w;
}

static i64 sys_read(int fd, char *buf, u64 n) {
    if (!uptr((u64)buf, n)) return -1;
    fd_entry_t *f = fd_get(fd);
    if (!f) return -1;
    if (fd <= 2) return 0;
    if (!perm_ok(f->vn, PERM_R)) return -1;     /* read permission         */
    stac();
    int r = vfs_read(f->vn, buf, f->pos, n);
    clac();
    if (r > 0) f->pos += r;
    return r;
}

static i64 sys_open(const char *path, int flags) {
    char kpath[VFS_MAX_PATH];
    if (!copy_user_str((u64)path, kpath, sizeof kpath)) return -1;
    path = kpath;
    task_t *t = cur();
    vnode_t *v = vfs_lookup_at(t ? t->cwd : NULL, path);
    if (!v && (flags & O_CREAT)) {
        char dirbuf[VFS_MAX_PATH] = {0}, basebuf[VFS_MAX_NAME] = {0};
        const char *slash = NULL;
        for (const char *p = path; *p; p++) if (*p == '/') slash = p;
        if (slash) {
            size_t l = slash - path;
            if (l == 0) l = 1;
            if (l >= sizeof dirbuf) return -1;
            memcpy(dirbuf, path, l);
            dirbuf[l] = 0;
            strncpy(basebuf, slash + 1, sizeof basebuf - 1);
        } else {
            strncpy(dirbuf, ".", sizeof dirbuf - 1);
            strncpy(basebuf, path, sizeof basebuf - 1);
        }
        vnode_t *d = vfs_lookup_at(t ? t->cwd : NULL, dirbuf);
        if (!d) return -1;
        if (!perm_ok(d, PERM_W)) return -1;     /* need write on the dir  */
        v = vfs_create(d, basebuf, VN_FILE);
    }
    if (!v) return -1;
    /* access check on the opened node: read for O_RDONLY/O_RDWR, write for
     * O_WRONLY/O_RDWR/O_TRUNC/O_APPEND */
    {
        int need = 0;
        if ((flags & 3) != O_WRONLY) need |= PERM_R;
        if (flags & (O_WRONLY | O_RDWR | O_TRUNC | O_APPEND)) need |= PERM_W;
        if (need && !perm_ok(v, need)) return -1;
    }
    if (v->type == VN_FILE && (flags & O_TRUNC)) vfs_truncate(v, 0);
    int fd = alloc_fd();
    if (fd < 0) return -1;
    vnode_ref(v);                        /* fd holds its own reference      */
    fd_entry_t *f = &cur()->fds[fd];
    f->vn     = v;
    f->pos    = (flags & O_APPEND) ? v->size : 0;
    f->flags  = flags;
    f->in_use = true;
    return fd;
}

static i64 sys_close(int fd) {
    fd_entry_t *f = fd_get(fd);
    if (!f || fd < 3) return -1;
    vnode_unref(f->vn);                  /* release the fd's reference      */
    f->in_use = false;
    f->vn = NULL;
    return 0;
}

static i64 sys_lseek(int fd, i64 off, int whence) {
    fd_entry_t *f = fd_get(fd);
    if (!f) return -1;
    u64 base;
    switch (whence) {
    case 0: base = 0; break;
    case 1: base = f->pos; break;
    case 2: base = f->vn ? f->vn->size : 0; break;
    default: return -1;
    }
    f->pos = base + off;
    return (i64)f->pos;
}

static i64 sys_getdents(int fd, yart_dirent_t *out, u64 cnt) {
    if (cnt > 256) return -1;
    if (!uptr((u64)out, cnt * sizeof(yart_dirent_t))) return -1;
    fd_entry_t *f = fd_get(fd);
    if (!f || fd < 3) return -1;
    if (f->vn->type != VN_DIR) return -1;
    if (!perm_ok(f->vn, PERM_R)) return -1;       /* need read on the dir */
    u64 idx = f->pos;
    u64 i = 0;
    u64 written = 0;
    stac();                                       /* filling a user buffer */
    for (vnode_t *c = f->vn->child; c; c = c->sibling, i++) {
        if (i < idx) continue;
        if (written >= cnt) break;
        yart_dirent_t *d = &out[written++];
        d->type = c->type;
        d->reclen = sizeof *d;
        d->size = c->size;
        strncpy(d->name, c->name, sizeof d->name - 1);
        d->name[sizeof d->name - 1] = 0;
    }
    clac();
    f->pos += written;
    return (i64)written;
}

static i64 sys_mkdir(const char *path) {
    char kpath[VFS_MAX_PATH];
    if (!copy_user_str((u64)path, kpath, sizeof kpath)) return -1;
    path = kpath;
    char dirbuf[VFS_MAX_PATH] = {0}, basebuf[VFS_MAX_NAME] = {0};
    const char *slash = NULL;
    for (const char *p = path; *p; p++) if (*p == '/') slash = p;
    if (slash) {
        size_t l = slash - path;
        if (l == 0) l = 1;
        memcpy(dirbuf, path, l); dirbuf[l] = 0;
        strncpy(basebuf, slash + 1, sizeof basebuf - 1);
    } else {
        strncpy(dirbuf, ".", sizeof dirbuf - 1);
        strncpy(basebuf, path, sizeof basebuf - 1);
    }
    vnode_t *d = vfs_lookup_at(cur()->cwd, dirbuf);
    if (!d) return -1;
    if (!perm_ok(d, PERM_W)) return -1;           /* need write on parent */
    return vfs_create(d, basebuf, VN_DIR) ? 0 : -1;
}

static i64 sys_unlink(const char *path) {
    char kpath[VFS_MAX_PATH];
    if (!copy_user_str((u64)path, kpath, sizeof kpath)) return -1;
    path = kpath;
    task_t *t = cur();
    vnode_t *v = vfs_lookup_at(t->cwd, path);
    if (!v || !v->parent) return -1;
    if (!perm_ok(v->parent, PERM_W)) return -1;   /* need write on parent */
    /* sticky dir: only the dir owner, the file owner, or root may delete */
    if ((v->parent->mode & PERM_STICKY) && t->euid != 0 &&
        t->euid != v->parent->uid && t->euid != v->uid)
        return -1;
    return vfs_unlink(v);
}

/* setuid: change the task's real+effective uid (and gid).  Root only, the
 * way a real OS lets root switch to another user (e.g. to drop to "demo"). */
/* rename(old, new): move a file/dir.  Checks write on both parents and
 * sticky-bit rules, copies the node (data + owner + mode) to the new name,
 * then unlinks the old one.  Works with the disk FS (delete journaled). */
/* brk(addr): set the program break (classic sbrk).  Growing allocates
 * demand-paged heap pages in the mmap arena (below the mmap cursor); it
 * cannot move above the arena end.  Returns the new break.  This is what
 * a real malloc() calls to grow the heap. */
/* sigaction(sig, handler): install (or clear) a user signal handler.
 * handler=0 clears it (default = terminate).  Only user tasks. */
static i64 sys_sigaction(u32 sig, u64 handler) {
    if (!g_sys_from_user) return -1;
    task_t *t = cur();
    if (!t || sig == 0 || sig >= 8) return -1;
    if (sig == 9) return -1;                 /* SIGKILL can't be caught  */
    t->sig_handlers[sig] = handler;
    kprintf("syscall: task %d sigaction(%u, %p)\n", t->pid, sig,
            (void *)handler);
    return 0;
}

/* raise(pid, sig): send a signal to another process. */
/* fsync(fd): force all dirty files to disk immediately.  This is the
 * POSIX durability guarantee - after fsync returns, the data is on the
 * disk, not just in RAM. */
/* setgid: change primary gid (root only; also seeds the supplementary list). */
static i64 sys_setgid(u32 gid) {
    task_t *t = cur();
    if (!t || !g_sys_from_user) return -1;
    if (t->euid != 0) return -1;
    t->gid = gid;
    t->supp_gid_count = 0;
    kprintf("syscall: task %d setgid to %u\n", t->pid, gid);
    return 0;
}

/* umask: set the process file-creation mask; returns the old value. */
static i64 sys_umask(u16 mask) {
    if (!g_sys_from_user) return -1;
    return (i64)sched_set_umask(mask);
}

/* acl(path, uid, mask): give a specific user explicit permissions on a
 * file (mask 0 removes the entry).  Owner or root only.  This is the ACL
 * the classic mode bits can't express ("let uid 2001 read this one file"). */
static i64 sys_acl(const char *path, u32 uid, u16 mask) {
    char kpath[VFS_MAX_PATH];
    if (!copy_user_str((u64)path, kpath, sizeof kpath)) return -1;
    task_t *t = cur();
    vnode_t *v = vfs_lookup_at(t->cwd, kpath);
    if (!v) return -1;
    if (g_sys_from_user) {
        u32 euid = t ? t->euid : 0;
        if (euid != 0 && v->uid != euid) return -1;   /* owner or root */
    }
    for (int i = 0; i < v->acl_count && i < VFS_ACL_MAX; i++) {
        if (v->acl[i].uid == uid) {
            if (mask == 0) {                          /* remove           */
                v->acl[i] = v->acl[v->acl_count - 1];
                v->acl_count--;
            } else {
                v->acl[i].mask = mask & PERM_RWX;
            }
            v->dirty = true;
            return 0;
        }
    }
    if (mask == 0) return 0;                          /* nothing to remove */
    if (v->acl_count >= VFS_ACL_MAX) return -1;
    v->acl[v->acl_count].uid = uid;
    v->acl[v->acl_count].mask = mask & PERM_RWX;
    v->acl_count++;
    v->dirty = true;
    kprintf("syscall: acl %s uid %u mask %o\n", kpath, uid, mask & 7);
    return 0;
}

static i64 sys_fsync(int fd) {
    (void)fd;                          /* whole-FS sync (simple + honest) */
    if (!g_sys_from_user) return -1;
    extern int blkfs_sync(void);
    extern bool blkfs_active(void);
    if (!blkfs_active()) return -1;
    int n = blkfs_sync();
    kprintf("fsync: flushed %d file(s) to disk\n", n);
    return 0;
}

static i64 sys_raise(u32 pid, u32 sig) {
    if (!g_sys_from_user) return -1;
    return (i64)sched_signal(pid, sig);
}

static i64 sys_brk(u64 addr) {
    if (!g_sys_from_user) return -1;
    task_t *t = cur();
    if (!t) return -1;
    if (addr == 0) return (i64)t->brk;              /* query only */
    if (addr < t->brk_base) return -1;
    if (addr > USER_MMAP_END - PAGE_SIZE) return -1;
    if (addr > t->brk) {
        u64 start = PAGE_ALIGN_UP(t->brk);
        u64 end   = PAGE_ALIGN_UP(addr);
        if (end > start) {
            u64 pages = (end - start) / PAGE_SIZE;
            if (sched_mem_used() + pages > sched_mem_limit()) return -1;
            if (vmm_user_reserve(start, pages, PTE_RW | PTE_US | PTE_NX,
                                 VMM_USER_LAZY) != 0)
                return -1;
        }
    }
    t->brk = addr;
    kprintf("brk: pid %d break -> %p\n", t->pid, (void *)addr);
    return (i64)t->brk;
}

static i64 sys_rename(const char *oldp, const char *newp) {
    char kold[VFS_MAX_PATH], knew[VFS_MAX_PATH];
    if (!copy_user_str((u64)oldp, kold, sizeof kold)) return -1;
    if (!copy_user_str((u64)newp, knew, sizeof knew)) return -1;
    task_t *t = cur();
    vnode_t *v = vfs_lookup_at(t->cwd, kold);
    if (!v) return -1;
    if (!perm_ok(v->parent, PERM_W)) return -1;
    if ((v->parent->mode & PERM_STICKY) && t->euid != 0 &&
        t->euid != v->parent->uid && t->euid != v->uid)
        return -1;
    /* split new path */
    char dir[VFS_MAX_PATH], base[VFS_MAX_NAME];
    const char *slash = NULL;
    for (const char *p = knew; *p; p++) if (*p == '/') slash = p;
    if (slash && slash != knew) {
        size_t l = slash - knew;
        if (l >= sizeof dir) return -1;
        memcpy(dir, knew, l); dir[l] = 0;
        strncpy(base, slash + 1, sizeof base - 1);
    } else { strncpy(dir, ".", sizeof dir - 1); strncpy(base, knew, sizeof base - 1); }
    vnode_t *np = vfs_lookup_at(t->cwd, dir);
    if (!np || np->type != VN_DIR) return -1;
    if (!perm_ok(np, PERM_W)) return -1;
    if (vfs_lookup_at(np, base)) return -1;        /* target exists */
    vnode_t *nv = vfs_create(np, base, v->type);
    if (!nv) return -1;
    if (v->type == VN_FILE && v->size) {
        nv->data = kzalloc(v->size);
        if (nv->data) { memcpy(nv->data, v->data, v->size); nv->size = v->size; nv->cap = v->size; }
    }
    nv->uid = v->uid; nv->gid = v->gid; nv->mode = v->mode; nv->dirty = true;
    return vfs_unlink(v) == 0 ? 0 : -1;
}

static i64 sys_setuid(u32 uid) {
    task_t *t = cur();
    if (!t || !g_sys_from_user) return -1;
    if (t->euid != 0) return -1;                /* only root can setuid   */
    t->uid = uid;
    t->euid = uid;
    t->gid = uid;                               /* per-user group         */
    kprintf("syscall: task %d setuid to %u\n", t->pid, uid);
    return 0;
}

static i64 sys_stat(const char *path, yart_stat_t *out) {
    char kpath[VFS_MAX_PATH];
    if (!copy_user_str((u64)path, kpath, sizeof kpath)) return -1;
    path = kpath;
    if (!uptr((u64)out, sizeof(yart_stat_t))) return -1;
    vnode_t *v = vfs_lookup_at(cur()->cwd, path);
    if (!v) return -1;
    stac();
    out->type  = v->type;
    out->mode  = v->mode;                  /* real permission bits now */
    out->size  = v->size;
    out->mtime = v->mtime;
    clac();
    return 0;
}

static i64 sys_getcwd(char *out, u64 cap) {
    if (!uptr((u64)out, cap)) return -1;
    stac();
    int r = vfs_path_of(cur()->cwd, out, cap);
    clac();
    return r;
}

static i64 sys_chdir(const char *path) {
    char kpath[VFS_MAX_PATH];
    if (!copy_user_str((u64)path, kpath, sizeof kpath)) return -1;
    path = kpath;
    vnode_t *v = vfs_lookup_at(cur()->cwd, path);
    if (!v || v->type != VN_DIR) return -1;
    if (!perm_ok(v, PERM_X)) return -1;           /* need execute (traverse) */
    vnode_ref(v);
    vnode_unref(cur()->cwd);
    cur()->cwd = v;
    return 0;
}

static i64 sys_truncate(const char *path, u64 n) {
    char kpath[VFS_MAX_PATH];
    if (!copy_user_str((u64)path, kpath, sizeof kpath)) return -1;
    path = kpath;
    vnode_t *v = vfs_lookup_at(cur()->cwd, path);
    if (!v) return -1;
    if (!perm_ok(v, PERM_W)) return -1;
    return vfs_truncate(v, n);
}

static i64 sys_klog(const char *s) {
    char kbuf[256];
    if (!copy_user_str((u64)s, kbuf, sizeof kbuf)) return -1;
    kputs("[user] ");
    kputs(kbuf);
    return 0;
}

static i64 sys_time(void) {
    rtc_time_t t; rtc_read(&t);
    return ((i64)t.year * 10000 + t.month * 100 + t.day) * 1000000ULL +
           (i64)(t.hour * 10000 + t.minute * 100 + t.second);
}

static i64 sys_waitpid(u32 pid, int *status_out) {
    if (status_out && !uptr((u64)status_out, sizeof(int))) return -1;
    if (status_out) { stac(); }
    i64 r = (i64)sched_waitpid(pid, status_out);
    if (status_out) { clac(); }
    return r;
}

static i64 sys_kill(u32 pid) {
    return (i64)sched_kill(pid);
}

/* mmap: give the process a block of dynamic memory (demand-paged, writable,
 * non-executable) in the mmap arena.  This is what lets a program grow its
 * memory at runtime - the foundation of malloc() and of big allocations
 * like "give me 64 MiB".  Returns the VA, or -1. */
static i64 sys_mmap(u64 len) {
    if (!g_sys_from_user) return -1;
    task_t *t = cur();
    if (!t) return -1;
    if (len == 0 || len > USER_MMAP_END - USER_MMAP_BASE) return -1;
    len = PAGE_ALIGN_UP(len);
    u64 pages = len / PAGE_SIZE;

    for (int wrap = 0; wrap < 2; wrap++) {
        u64 cand = PAGE_ALIGN_UP(t->mmap_next);
        if (cand < USER_MMAP_BASE) cand = USER_MMAP_BASE;
        while (cand + len <= USER_MMAP_END) {
            if (vmm_user_reserve(cand, pages, PTE_RW | PTE_US | PTE_NX,
                                 VMM_USER_LAZY) == 0) {
                /* GUARD PAGE: advance past one extra unmapped page so the
                 * next region starts a page later - a 1-page overflow past
                 * this region faults instead of silently touching the next
                 * allocation (or unmapped memory) */
                t->mmap_next = cand + len + PAGE_SIZE;
                kprintf("mmap: pid %d got %lu bytes at %p (demand-paged)\n",
                        t->pid, len, (void *)cand);
                return (i64)cand;
            }
            cand += PAGE_SIZE;         /* this spot is taken, try next    */
        }
        t->mmap_next = USER_MMAP_BASE; /* wrap around the arena once      */
    }
    return -1;                          /* arena full / no gap             */
}

/* munmap: release a block previously returned by mmap(). */
static i64 sys_munmap(u64 addr) {
    if (!g_sys_from_user) return -1;
    if (addr < USER_MMAP_BASE || addr >= USER_MMAP_END) return -1;
    if (vmm_user_release(addr) == 0) return 0;
    return -1;
}

/* doas: elevate the effective uid to root, but ONLY if this task belongs to
 * an admin account AND the caller supplies that account's correct password.
 * Root (euid 0) never needs it.  This is the "sudo" analogue - named after
 * OpenBSD's doas. */
static i64 sys_doas(const char *password) {
    char kpw[64];
    if (!copy_user_str((u64)password, kpw, sizeof kpw)) return -1;
    task_t *t = cur();
    if (!t || !g_sys_from_user) return -1;
    if (t->euid == 0) return 0;                    /* already root         */
    if (!t->elev_allowed) return -1;               /* not an admin account */
    if (t->account[0] == 0) return -1;
    if (!session_auth(t->account, kpw)) return -1;
    t->euid = 0;
    kprintf("syscall: task %d '%s' elevated to root via doas\n",
            t->pid, t->name);
    return 0;
}

/* drop: give up elevated privileges (euid back to the real uid). */
static i64 sys_drop(void) {
    task_t *t = cur();
    if (!t || !g_sys_from_user) return -1;
    t->euid = t->uid;
    return 0;
}

/* chmod: change permission bits.  Owner or root only. */
static i64 sys_chmod(const char *path, u16 mode) {
    char kpath[VFS_MAX_PATH];
    if (!copy_user_str((u64)path, kpath, sizeof kpath)) return -1;
    task_t *t = cur();
    vnode_t *v = vfs_lookup_at(t ? t->cwd : NULL, kpath);
    if (!v) return -1;
    if (g_sys_from_user) {
        u32 euid = t ? t->euid : 0;
        if (euid != 0 && v->uid != euid) return -1;   /* not the owner */
    }
    v->mode = mode & 0777u;
    v->dirty = true;
    return 0;
}

/* ---------- dispatcher ---------- */

static bool g_user_seg_checked;
static void check_user_segments(cpu_regs_t *r) {
    if (g_user_seg_checked) return;
    g_user_seg_checked = true;
    if ((r->cs & 3) == 3 && (r->ss & 3) == 3 &&
        r->cs == USER_CS && r->ss == USER_DS) {
        kprintf("gdt: user segment check OK (cs=%lx ss=%lx)\n", r->cs, r->ss);
    } else {
        kprintf("gdt: !! user segment check FAILED (cs=%lx ss=%lx)\n",
                r->cs, r->ss);
    }
}

static void syscall_handler(cpu_regs_t *r) {
    check_user_segments(r);
    g_sys_from_user = ((r->cs & 3) == 3);
    u64 a0 = r->rdi, a1 = r->rsi, a2 = r->rdx;
    switch (r->rax) {
    case SYS_EXIT:     sched_exit((int)a0); r->rax = 0; break;
    case SYS_WRITE:    r->rax = (u64)sys_write   ((int)a0, (const char *)a1, a2); break;
    case SYS_READ:     r->rax = (u64)sys_read    ((int)a0, (char *)a1, a2); break;
    case SYS_OPEN:     r->rax = (u64)sys_open    ((const char *)a0, (int)a1); break;
    case SYS_CLOSE:    r->rax = (u64)sys_close   ((int)a0); break;
    case SYS_LSEEK:    r->rax = (u64)sys_lseek   ((int)a0, (i64)a1, (int)a2); break;
    case SYS_GETDENTS: r->rax = (u64)sys_getdents((int)a0, (yart_dirent_t *)a1, a2); break;
    case SYS_MKDIR:    r->rax = (u64)sys_mkdir   ((const char *)a0); break;
    case SYS_UNLINK:   r->rax = (u64)sys_unlink  ((const char *)a0); break;
    case SYS_STAT:     r->rax = (u64)sys_stat    ((const char *)a0, (yart_stat_t *)a1); break;
    case SYS_GETCWD:   r->rax = (u64)sys_getcwd  ((char *)a0, a1); break;
    case SYS_CHDIR:    r->rax = (u64)sys_chdir   ((const char *)a0); break;
    case SYS_GETPID:   r->rax = (u64)task_getpid(); break;
    case SYS_TIME:     r->rax = (u64)sys_time(); break;
    case SYS_YIELD:    sched_yield(); r->rax = 0; break;
    case SYS_TRUNCATE: r->rax = (u64)sys_truncate((const char *)a0, a1); break;
    case SYS_KLOG:     r->rax = (u64)sys_klog    ((const char *)a0); break;
    case SYS_FORK: {
        task_t *c = sched_fork(cur(), r);
        r->rax = c ? (u64)c->pid : (u64)-1;   /* parent gets the child pid */
        break;
    }
    case SYS_WAITPID:  r->rax = (u64)sys_waitpid((u32)a0, (int *)a1); break;
    case SYS_DOAS:     r->rax = (u64)sys_doas((const char *)a0); break;
    case SYS_CHMOD:    r->rax = (u64)sys_chmod((const char *)a0, (u16)a1); break;
    case SYS_DROP:     r->rax = (u64)sys_drop(); break;
    case SYS_KILL:     r->rax = (u64)sys_kill((u32)a0); break;
    case SYS_MMAP:     r->rax = (u64)sys_mmap(a0); break;
    case SYS_MUNMAP:   r->rax = (u64)sys_munmap(a0); break;
    case SYS_SETUID:   r->rax = (u64)sys_setuid((u32)a0); break;
    case SYS_RENAME:   r->rax = (u64)sys_rename((const char *)a0, (const char *)a1); break;
    case SYS_BRK:      r->rax = (u64)sys_brk(a0); break;
    case SYS_SIGACTION: r->rax = (u64)sys_sigaction((u32)a0, a1); break;
    case SYS_RAISE:    r->rax = (u64)sys_raise((u32)a0, (u32)a1); break;
    case SYS_FSYNC:    r->rax = (u64)sys_fsync((int)a0); break;
    case SYS_SETGID:   r->rax = (u64)sys_setgid((u32)a0); break;
    case SYS_UMASK:    r->rax = (u64)sys_umask((u16)a0); break;
    case SYS_ACL:      r->rax = (u64)sys_acl((const char *)a0, (u32)a1, (u16)a2); break;
    case SYS_GETCPU:   r->rax = (u64)sys_getcpu(); break;
    default:
        kprintf("syscall: bad #%lu\n", r->rax);
        r->rax = (u64)-1;
    }
}

void syscall_install(void) {
    irq_register(0x80, syscall_handler);
    kprintf("syscall: dispatcher up, %d slots\n", SYS_MAX);
}
