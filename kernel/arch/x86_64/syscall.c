/* Yart OS - syscall dispatcher + per-task file-descriptor table.
 *
 * ABI:  int 0x80
 *   rax = number   rdi = arg0   rsi = arg1   rdx = arg2   r10 = arg3
 *   ret in rax
 *
 * The file-descriptor table is global for now (single task), but wired
 * exactly the same way a per-task struct would work, so the real switch
 * later is a one-line refactor.
 */
#include <yart/types.h>
#include <yart/console.h>
#include <yart/hal.h>
#include <yart/io.h>
#include <yart/string.h>
#include <yart/fs.h>
#include <yart/syscall.h>

#define MAX_FD 32

typedef struct {
    vnode_t *vn;
    u64      pos;
    u32      flags;
    bool     in_use;
} fd_t;

typedef struct {
    fd_t     fds[MAX_FD];
    vnode_t *cwd;
    int      pid;
} task_t;

static task_t g_task;
static char   g_klog_line[256];   /* small staging buffer for kputs */

void task_init(void) {
    memset(&g_task, 0, sizeof g_task);
    g_task.cwd = vfs_root();
    g_task.pid = 1;
    /* fd 0/1/2 are conceptually stdin/stdout/stderr but writes go to the
       kernel console; reads return 0.  We still mark them used so an
       open() returns >= 3 like POSIX. */
    g_task.fds[0].in_use = g_task.fds[1].in_use = g_task.fds[2].in_use = true;
}

vnode_t *task_cwd(void) { return g_task.cwd; }
void     task_set_cwd(vnode_t *v) { if (v && v->type == VN_DIR) g_task.cwd = v; }

static int alloc_fd(void) {
    for (int i = 3; i < MAX_FD; i++) if (!g_task.fds[i].in_use) return i;
    return -1;
}

/* ---------- syscall handlers ---------- */
static i64 sys_write(int fd, const char *buf, u64 n) {
    if (!buf) return -1;
    if (fd == 1 || fd == 2) {
        size_t k = n < sizeof g_klog_line - 1 ? n : sizeof g_klog_line - 1;
        memcpy(g_klog_line, buf, k);
        g_klog_line[k] = 0;
        kputs(g_klog_line);
        return (i64)n;
    }
    if (fd < 0 || fd >= MAX_FD || !g_task.fds[fd].in_use) return -1;
    fd_t *f = &g_task.fds[fd];
    int w = vfs_write(f->vn, buf, f->pos, n);
    if (w > 0) f->pos += w;
    return w;
}

static i64 sys_read(int fd, char *buf, u64 n) {
    if (fd < 0 || fd >= MAX_FD || !g_task.fds[fd].in_use) return -1;
    if (fd <= 2) return 0;
    fd_t *f = &g_task.fds[fd];
    int r = vfs_read(f->vn, buf, f->pos, n);
    if (r > 0) f->pos += r;
    return r;
}

static i64 sys_open(const char *path, int flags) {
    if (!path) return -1;
    vnode_t *v = vfs_lookup_at(g_task.cwd, path);
    if (!v && (flags & O_CREAT)) {
        /* split into dir + base */
        char dirbuf[VFS_MAX_PATH] = {0}, basebuf[VFS_MAX_NAME] = {0};
        const char *slash = NULL;
        for (const char *p = path; *p; p++) if (*p == '/') slash = p;
        if (slash) {
            size_t l = slash - path;
            if (l == 0) l = 1;     /* root */
            if (l >= sizeof dirbuf) return -1;
            memcpy(dirbuf, path, l);
            dirbuf[l] = 0;
            strncpy(basebuf, slash + 1, sizeof basebuf - 1);
        } else {
            strncpy(dirbuf, ".", sizeof dirbuf - 1);
            strncpy(basebuf, path, sizeof basebuf - 1);
        }
        vnode_t *d = vfs_lookup_at(g_task.cwd, dirbuf);
        if (!d) return -1;
        v = vfs_create(d, basebuf, VN_FILE);
    }
    if (!v) return -1;
    if (v->type == VN_FILE && (flags & O_TRUNC)) vfs_truncate(v, 0);
    int fd = alloc_fd();
    if (fd < 0) return -1;
    g_task.fds[fd].vn     = v;
    g_task.fds[fd].pos    = (flags & O_APPEND) ? v->size : 0;
    g_task.fds[fd].flags  = flags;
    g_task.fds[fd].in_use = true;
    return fd;
}

static i64 sys_close(int fd) {
    if (fd < 3 || fd >= MAX_FD || !g_task.fds[fd].in_use) return -1;
    g_task.fds[fd].in_use = false;
    g_task.fds[fd].vn = NULL;
    return 0;
}

static i64 sys_lseek(int fd, i64 off, int whence) {
    if (fd < 0 || fd >= MAX_FD || !g_task.fds[fd].in_use) return -1;
    fd_t *f = &g_task.fds[fd];
    u64 base;
    switch (whence) {
    case 0: base = 0; break;            /* SEEK_SET */
    case 1: base = f->pos; break;       /* SEEK_CUR */
    case 2: base = f->vn ? f->vn->size : 0; break;  /* SEEK_END */
    default: return -1;
    }
    f->pos = base + off;
    return (i64)f->pos;
}

static i64 sys_getdents(int fd, yart_dirent_t *out, u64 cnt) {
    if (fd < 3 || fd >= MAX_FD || !g_task.fds[fd].in_use) return -1;
    fd_t *f = &g_task.fds[fd];
    if (f->vn->type != VN_DIR) return -1;
    u64 idx = f->pos;
    u64 i = 0;
    u64 written = 0;
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
    f->pos += written;
    return (i64)written;
}

static i64 sys_mkdir(const char *path) {
    if (!path) return -1;
    /* split */
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
    vnode_t *d = vfs_lookup_at(g_task.cwd, dirbuf);
    if (!d) return -1;
    return vfs_create(d, basebuf, VN_DIR) ? 0 : -1;
}

static i64 sys_unlink(const char *path) {
    vnode_t *v = vfs_lookup_at(g_task.cwd, path);
    return vfs_unlink(v);
}

static i64 sys_stat(const char *path, yart_stat_t *out) {
    vnode_t *v = vfs_lookup_at(g_task.cwd, path);
    if (!v || !out) return -1;
    out->type  = v->type;
    out->mode  = 0644;
    out->size  = v->size;
    out->mtime = v->mtime;
    return 0;
}

static i64 sys_getcwd(char *out, u64 cap) {
    return vfs_path_of(g_task.cwd, out, cap);
}

static i64 sys_chdir(const char *path) {
    vnode_t *v = vfs_lookup_at(g_task.cwd, path);
    if (!v || v->type != VN_DIR) return -1;
    g_task.cwd = v;
    return 0;
}

static i64 sys_truncate(const char *path, u64 n) {
    vnode_t *v = vfs_lookup_at(g_task.cwd, path);
    return vfs_truncate(v, n);
}

static i64 sys_klog(const char *s) {
    if (!s) return -1;
    kputs("[user] ");
    kputs(s);
    return 0;
}

static i64 sys_time(void) {
    rtc_time_t t; rtc_read(&t);
    return ((i64)t.year * 10000 + t.month * 100 + t.day) * 1000000ULL +
           (i64)(t.hour * 10000 + t.minute * 100 + t.second);
}

#include <yart/user.h>
static void sys_exit(i64 status) {
    kprintf("syscall: task %d exit(%ld)\n", g_task.pid, status);
    user_return(status);
    /* if we somehow got here (no user task), spin */
    for (;;) hlt();
}

/* ---------- dispatcher ---------- */
static void syscall_handler(cpu_regs_t *r) {
    u64 a0 = r->rdi, a1 = r->rsi, a2 = r->rdx;
    switch (r->rax) {
    case SYS_EXIT:     sys_exit((i64)a0); break;
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
    case SYS_GETPID:   r->rax = (u64)g_task.pid; break;
    case SYS_TIME:     r->rax = (u64)sys_time(); break;
    case SYS_YIELD:    __asm__ volatile ("sti; hlt"); r->rax = 0; break;
    case SYS_TRUNCATE: r->rax = (u64)sys_truncate((const char *)a0, a1); break;
    case SYS_KLOG:     r->rax = (u64)sys_klog    ((const char *)a0); break;
    default:
        kprintf("syscall: bad #%lu\n", r->rax);
        r->rax = (u64)-1;
    }
}

void syscall_install(void) {
    irq_register(0x80, syscall_handler);
    task_init();
    kprintf("syscall: dispatcher up, %d slots\n", SYS_MAX);
}
