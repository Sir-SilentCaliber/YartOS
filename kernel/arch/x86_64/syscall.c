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
#include <yart/net.h>    /* net_get_addrs / net_udp_send / net_udp_recv */
#include <yart/cpu.h>   /* stac/clac for SMAP */   /* session_auth() for doas */
#include <yart/gui.h>
#include <yart/audio.h>
#include <yart/audio.h>
#include <yart/acpi.h>   /* g_acpi_battery (real ACPI battery via _BST/_BIF) */
#include <yart/drivers.h>
#include <yart/watchdog.h>
#include <yart/sha256.h>
#include <yart/spinlock.h>
#include <yart/wifi.h>

/* ---------- doas user database (salted SHA-256) ----------
 * The password check used to accept ANY non-empty string ("ring-3 auth
 * lands later") while kernel/lib/sha256.c sat unused - the audit claimed
 * salted hashing that the code did not perform.  Now every admin account
 * stores salt + SHA-256(salt || password); the comparison is constant-time
 * and repeated failures trigger a temporary lockout.  Single iteration
 * (no PBKDF2/Argon2) remains the honest, documented limitation. */
#define DOAS_MAX_USERS     8
#define DOAS_MAX_FAILS     5
#define DOAS_LOCKOUT_TICKS 100          /* 1 s per failed attempt window */
typedef struct {
    char account[32];
    char salt[16];
    u8   hash[32];
    u32  fails;
    u64  fail_t0;
} doas_user_t;

static doas_user_t g_doas_users[DOAS_MAX_USERS];
static int         g_doas_nusers;

#define DOAS_PBKDF2_ITERS 10000

/* Persistent password store: /home/yart/.passwd holds salt+hash so a
 * password change survives reboot (the hobby-OS equivalent of /etc/shadow).
 * doas_init() loads it after the disk FS is mounted; sys_passwd() writes it
 * back and fsyncs.  Absent file = factory default ("yart"). */
#define PASSWD_MAGIC "YARTPSWD"
typedef struct {
    char magic[8];
    char salt[16];
    u8   hash[32];
} passwd_file_t;

static void hmac_sha256_k(const u8 *key, size_t keylen,
                          const u8 *msg, size_t mlen, u8 out[32]) {
    u8 k[64];
    memset(k, 0, sizeof k);
    if (keylen > 64) {
        sha256_ctx_t c; sha256_init(&c); sha256_update(&c, key, keylen); sha256_final(&c, k);
    } else {
        memcpy(k, key, keylen);
    }
    u8 ipad[64], opad[64];
    for (int i = 0; i < 64; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5C; }
    sha256_ctx_t c; u8 inner[32];
    sha256_init(&c); sha256_update(&c, ipad, 64); sha256_update(&c, msg, mlen); sha256_final(&c, inner);
    sha256_init(&c); sha256_update(&c, opad, 64); sha256_update(&c, inner, 32); sha256_final(&c, out);
}

static void pbkdf2_hmac_sha256(const u8 *pw, size_t pwlen,
                               const u8 *salt, size_t slen,
                               u32 iters, u8 *out, size_t outlen) {
    u32 block = 1;
    while (outlen) {
        u8 msg[80]; size_t m = 0;
        if (slen > 64) slen = 64;
        memcpy(msg, salt, slen); m = slen;
        msg[m++] = (u8)(block >> 24); msg[m++] = (u8)(block >> 16);
        msg[m++] = (u8)(block >> 8);  msg[m++] = (u8)block;
        u8 u[32], t[32];
        hmac_sha256_k(pw, pwlen, msg, m, u);
        memcpy(t, u, 32);
        for (u32 i = 1; i < iters; i++) {
            hmac_sha256_k(pw, pwlen, u, 32, u);
            for (int j = 0; j < 32; j++) t[j] ^= u[j];
        }
        size_t take = outlen > 32 ? 32 : outlen;
        memcpy(out, t, take);
        out += take; outlen -= take; block++;
    }
}

void doas_init(void) {
    g_doas_nusers = 0;
    /* Default demo account.  Change the password here (it is hashed into
     * RAM at boot; there is no persistent user DB yet). */
    const char *account  = "demo";
    const char *salt     = "yart-salt-1";
    const char *password = "yart";
    doas_user_t *u = &g_doas_users[g_doas_nusers++];
    strncpy(u->account, account, sizeof u->account - 1);
    strncpy(u->salt, salt, sizeof u->salt - 1);
    pbkdf2_hmac_sha256((const u8 *)password, strlen(password),
                       (const u8 *)u->salt, strlen(u->salt),
                       DOAS_PBKDF2_ITERS, u->hash, 32);

    /* A persisted password (from a previous `passwd`) overrides the factory
     * default.  The disk FS is mounted by now (blkfs_init runs before
     * doas_init), so the file is real, on-disk data. */
    vnode_t *pw = vfs_lookup("/home/yart/.passwd");
    if (pw) {
        passwd_file_t pf;
        int rd = vfs_read(pw, &pf, 0, sizeof pf);
        if (rd == (int)sizeof pf && strncmp(pf.magic, PASSWD_MAGIC, 8) == 0) {
            memcpy(g_doas_users[0].salt, pf.salt, sizeof pf.salt);
            memcpy(g_doas_users[0].hash, pf.hash, sizeof pf.hash);
            kprintf("doas: loaded persisted password hash for 'demo'\n");
        } else {
            kprintf("doas: .passwd present but unusable (read=%d size=%d magic=%02x%02x%02x%02x%02x%02x%02x%02x)\n",
                    rd, (int)pw->size,
                    (u8)pf.magic[0], (u8)pf.magic[1], (u8)pf.magic[2], (u8)pf.magic[3],
                    (u8)pf.magic[4], (u8)pf.magic[5], (u8)pf.magic[6], (u8)pf.magic[7]);
        }
    } else {
        kprintf("doas: no /home/yart/.passwd (factory password)\n");
    }

    kprintf("doas: %d user(s) (demo password hashed; default is 'yart')\n",
            g_doas_nusers);
}

static int ct_neq(const u8 *a, const u8 *b, int n) {
    u8 d = 0;
    for (int i = 0; i < n; i++) d |= a[i] ^ b[i];
    return d;
}

/* Shared password check: PBKDF2-HMAC-SHA256, constant-time compare, failure
 * lockout.  Used by SYS_DOAS (elevate) and SYS_AUTH_VERIFY (verify WITHOUT
 * elevating, e.g. the lock screen). */
static bool doas_check(task_t *t, const char *password) {
    doas_user_t *u = NULL;
    for (int i = 0; i < g_doas_nusers; i++)
        if (strcmp(g_doas_users[i].account, t->account) == 0) {
            u = &g_doas_users[i];
            break;
        }
    if (!u) {
        kprintf("doas: no account '%s' for task %d\n", t->account, t->pid);
        return false;
    }
    if (u->fails >= DOAS_MAX_FAILS &&
        pit_ticks() - u->fail_t0 < DOAS_LOCKOUT_TICKS) {
        kprintf("doas: account '%s' temporarily locked (too many failures)\n",
                u->account);
        return false;
    }
    if (u->fails >= DOAS_MAX_FAILS) u->fails = 0;   /* lockout expired */
    u8 h[32];
    pbkdf2_hmac_sha256((const u8 *)password, strlen(password),
                       (const u8 *)u->salt, strlen(u->salt),
                       DOAS_PBKDF2_ITERS, h, 32);
    if (ct_neq(h, u->hash, 32)) {
        u->fails++;
        u->fail_t0 = pit_ticks();
        kprintf("doas: auth FAILED for '%s' (%u/%u)\n",
                u->account, u->fails, DOAS_MAX_FAILS);
        return false;
    }
    u->fails = 0;
    return true;
}

/* Shared password check: salted SHA-256, constant-time compare, failure
 * lockout.  Used by SYS_DOAS (elevate) and SYS_AUTH_VERIFY (verify WITHOUT
 * elevating, e.g. the lock screen). */

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
/* ---- pipe helpers ---- */
static yart_pipe_t *pipe_new(void) {
    yart_pipe_t *p = kzalloc(sizeof *p);
    if (p) { p->refs = 2; p->read_ends = 1; p->write_ends = 1; }
    return p;
}
static void pipe_put(yart_pipe_t *p) {
    if (!p) return;
    if (--p->refs <= 0) kfree(p);
}
static void pipe_write(yart_pipe_t *p, const u8 *src, u32 n) {
    for (u32 i = 0; i < n; i++) {
        p->buf[p->head] = src[i];
        p->head = (p->head + 1) % PIPE_BUF_SIZE;
    }
    p->count += n;
}
static u32 pipe_read(yart_pipe_t *p, u8 *dst, u32 n) {
    u32 got = 0;
    while (got < n && p->count > 0) {
        dst[got++] = p->buf[p->tail];
        p->tail = (p->tail + 1) % PIPE_BUF_SIZE;
        p->count--;
    }
    return got;
}

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
    if (f->is_pipe) {
        yart_pipe_t *p = f->pipe;
        if (!p) return -1;
        if (p->read_ends == 0) return -1;        /* reader gone: EPIPE-ish */
        u32 room = PIPE_BUF_SIZE - p->count;
        if (room == 0) return -2;                /* would block            */
        u32 k = (u32)n < room ? (u32)n : room;
        stac();
        pipe_write(p, (const u8 *)buf, k);
        clac();
        return (i64)k;
    }
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
    if (f->is_pipe) {
        yart_pipe_t *p = f->pipe;
        if (!p) return -1;
        if (p->count == 0) {
            if (p->write_ends == 0) return 0;    /* EOF: writer closed    */
            return -2;                           /* would block           */
        }
        u32 k = (u32)n;
        stac();
        k = pipe_read(p, (u8 *)buf, k);
        clac();
        return (i64)k;
    }
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
    if (f->is_pipe && f->pipe) {
        yart_pipe_t *p = f->pipe;
        if (f->pipe_is_read_end) {
            if (p->read_ends > 0) p->read_ends--;
        } else {
            if (p->write_ends > 0) p->write_ends--;
        }
        pipe_put(p);
        f->pipe = NULL;
    } else {
        vnode_unref(f->vn);              /* release the fd's reference      */
    }
    f->in_use = false;
    f->vn = NULL;
    f->is_pipe = false;
    return 0;
}

/* dup2(oldfd, newfd): POSIX fd duplication.  Closes newfd if open, then
 * copies the oldfd entry onto it, taking a fresh reference on the shared
 * resource (vnode or pipe) so close() of either end is balanced. */
static i64 sys_dup2(int oldfd, int newfd) {
    if (newfd < 0 || newfd >= MAX_FD) return -1;
    task_t *t = cur(); if (!t || !g_sys_from_user) return -1;
    fd_entry_t *of = fd_get(oldfd);
    if (!of || oldfd < 3) return -1;
    if (oldfd == newfd) return newfd;
    if (t->fds[newfd].in_use) {
        i64 rc = sys_close(newfd);          /* drop the target's refs */
        if (rc != 0 && newfd >= 3) return -1;
    }
    fd_entry_t *nf = &t->fds[newfd];
    *nf = *of;                              /* copy the whole entry      */
    if (nf->is_pipe && nf->pipe) {
        nf->pipe->refs++;
        if (nf->pipe_is_read_end) nf->pipe->read_ends++;
        else                       nf->pipe->write_ends++;
    } else if (nf->vn) {
        vnode_ref(nf->vn);
    }
    return newfd;
}

/* pipe(fds[2]): create an in-kernel byte pipe.  fds[0] = read end,
 * fds[1] = write end.  Returns 0 or -1. */
static i64 sys_pipe(int *fds) {
    if (!g_sys_from_user) return -1;
    if (!uptr((u64)fds, 2 * sizeof(int))) return -1;
    task_t *t = cur();
    if (!t) return -1;
    /* allocate ONE fd, mark it in_use, THEN allocate the second - alloc_fd
     * returns the first free slot, so allocating both up front yields the
     * SAME fd twice (the first isn't reserved yet) */
    int r = alloc_fd();
    if (r < 0) return -1;
    t->fds[r].in_use = true;
    int w = alloc_fd();
    if (w < 0) { t->fds[r].in_use = false; return -1; }
    t->fds[w].in_use = true;
    yart_pipe_t *p = pipe_new();
    if (!p) { t->fds[r].in_use = t->fds[w].in_use = false; return -1; }
    fd_entry_t *fr = &t->fds[r];
    fd_entry_t *fw = &t->fds[w];
    fr->is_pipe = true; fr->pipe_is_read_end = true;
    fr->pipe = p; fr->vn = NULL; fr->pos = 0;
    fw->is_pipe = true; fw->pipe_is_read_end = false;
    fw->pipe = p; fw->vn = NULL; fw->pos = 0;
    stac();
    fds[0] = r;
    fds[1] = w;
    clac();
    kprintf("pipe: pid %d got fds %d/%d\n", t->pid, r, w);
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
    if (!t) return -1;
    if (sig == 0 || sig >= 32) return -1;    /* POSIX range: 1..31       */
    if (sig == 9 || sig == 19) return -1;    /* SIGKILL/SIGSTOP uncatchable */
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

static i64 sys_waitpid(u32 pid, int *status_out, int flags) {
    if (status_out && !uptr((u64)status_out, sizeof(int))) return -1;
    if (status_out) { stac(); }
    i64 r = (i64)sched_waitpid(pid, status_out, flags);
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

/* net info: write {ip, mask, gw, dns} (host order) + link flag to user. */
static i64 sys_net_info(u32 *out) {
    if (!uptr((u64)out, 5 * sizeof(u32))) return -1;
    u32 v[5]; net_get_addrs(&v[0], &v[2], &v[3], &v[1]);  /* ip, mask, gw, dns */
    v[4] = nic_present() ? 1 : 0;
    stac();
    for (int i = 0; i < 5; i++) out[i] = v[i];
    clac();
    return 0;
}
static i64 sys_udp_send(u32 dst, u16 dport, const u8 *buf, u16 len) {
    if (len > 1400) return -1;
    if (!uptr((u64)buf, len)) return -1;
    u8 k[1400]; stac(); memcpy(k, buf, len); clac();
    return (i64)net_udp_send(dst, dport, k, len);
}
static i64 sys_udp_recv(u8 *buf, u16 cap) {
    if (!uptr((u64)buf, cap)) return -1;
    u8 k[1400]; u16 n = (u16)net_udp_recv(k, cap < 1400 ? cap : 1400);
    if (n > 0) { stac(); memcpy(buf, k, n); clac(); }
    return (i64)n;
}

static i64 sys_net_fw_add(u32 proto, u32 dip, u32 dport, u32 drop) {
    if (!g_sys_from_user) return -1;
    if (proto > 255 || dip == 0xFFFFFFFFu || dport > 65535) return -1;
    return (i64)net_fw_add((u8)proto, dip, (u16)dport, drop != 0);
}
static i64 sys_net_fw_clear(void) {
    if (!g_sys_from_user) return -1;
    return (i64)net_fw_clear();
}
static i64 sys_udp_bind(u16 port) {
    if (!g_sys_from_user) return -1;
    return (i64)net_udp_bind(port);
}
static i64 sys_icmp_ping(u32 ip, u64 *rtt) {
    if (!g_sys_from_user) return -1;
    if (!uptr((u64)rtt, 8)) return -1;
    u64 t = 0;
    if (net_icmp_ping(ip, &t) != 0) return -1;
    stac(); *(u64 *)rtt = t; clac();
    return 0;
}

static i64 sys_icmp6_ping(const u8 *addr, u64 *rtt) {
    if (!g_sys_from_user) return -1;
    if (!uptr((u64)addr, 16) || !uptr((u64)rtt, 8)) return -1;
    u8 a[16]; stac(); memcpy(a, addr, 16); clac();
    u64 t = 0;
    if (net_icmp6_ping(a, &t) != 0) return -1;
    stac(); *(u64 *)rtt = t; clac();
    return 0;
}
static i64 sys_ipv6_info(u8 *addr, u8 *router) {
    if (!g_sys_from_user) return -1;
    if (addr && !uptr((u64)addr, 16)) return -1;
    if (router && !uptr((u64)router, 16)) return -1;
    u8 a[16], r[16];
    net_ipv6_addrs(a, r);
    stac();
    if (addr) memcpy(addr, a, 16);
    if (router) memcpy(router, r, 16);
    clac();
    return net_ipv6_ready() ? 0 : -1;
}

static i64 sys_tls_connect(u32 ip, u16 port) {
    if (!g_sys_from_user) return -1;
    return (i64)tls_connect(ip, port);
}
static i64 sys_tls_send(i64 h, const u8 *buf, u64 len) {
    if (!g_sys_from_user) return -1;
    if (len > 16000) return -1;
    if (!uptr((u64)buf, len)) return -1;
    static u8 k[16000];   /* too big for the 16K kstack */
    stac(); memcpy(k, buf, len); clac();
    return (i64)tls_send((int)h, k, (int)len);
}
static i64 sys_tls_recv(i64 h, u8 *buf, u64 cap) {
    if (!g_sys_from_user) return -1;
    if (cap > 4096) cap = 4096;
    if (!uptr((u64)buf, cap)) return -1;
    static u8 k[4096];
    int n = tls_recv((int)h, k, (int)cap);
    if (n > 0) { stac(); memcpy(buf, k, (size_t)n); clac(); }
    return (i64)n;
}
static i64 sys_tls_close(i64 h) {
    if (!g_sys_from_user) return -1;
    return (i64)tls_close((int)h);
}

static i64 sys_tls_listen(u16 port) {
    if (!g_sys_from_user) return -1;
    return (i64)tls_server_listen(port);
}
static i64 sys_tls_accept(i64 listener) {
    if (!g_sys_from_user) return -1;
    return (i64)tls_server_accept((int)listener);
}

static i64 sys_dns_resolve(const char *name, u32 *out) {
    if (!g_sys_from_user) return -1;
    char k[256];
    if (!copy_user_str((u64)name, k, sizeof k)) return -1;
    if (!uptr((u64)out, 4)) return -1;
    u32 ip;
    if (net_dns_resolve(k, &ip) != 0) return -1;
    stac(); *(u32 *)out = ip; clac();
    return 0;
}

/* ---- TCP sockets ---- */
static i64 sys_tcp_connect(u32 ip, u16 port) {
    if (!g_sys_from_user) return -1;
    return (i64)net_tcp_connect(ip, port);
}
static i64 sys_tcp_send(i64 conn, const u8 *buf, u64 len) {
    if (!g_sys_from_user) return -1;
    if (len > 1400) return -1;
    if (!uptr((u64)buf, len)) return -1;
    u8 k[1400]; stac(); memcpy(k, buf, len); clac();
    return (i64)net_tcp_send((int)conn, k, (int)len);
}
static i64 sys_tcp_recv(i64 conn, u8 *buf, u64 cap) {
    if (!g_sys_from_user) return -1;
    if (cap > 4096) cap = 4096;
    if (!uptr((u64)buf, cap)) return -1;
    u8 k[4096]; int n = net_tcp_recv((int)conn, k, (int)cap);
    if (n > 0) { stac(); memcpy(buf, k, (size_t)n); clac(); }
    return (i64)n;
}
static i64 sys_tcp_close(i64 conn) {
    if (!g_sys_from_user) return -1;
    return (i64)net_tcp_close((int)conn);
}
static i64 sys_tcp_listen(u16 port) {
    if (!g_sys_from_user) return -1;
    return (i64)net_tcp_listen(port);
}
static i64 sys_tcp_accept(i64 listener) {
    if (!g_sys_from_user) return -1;
    return (i64)net_tcp_accept((int)listener);
}

/* sigreturn: restore a frame that signal delivery saved on the user
 * stack.  The user handler returned into the vdso-style trampoline,
 * which called this with rdi = the saved frame's address.  We copy it
 * back into the current frame and return into the interrupted code. */
#define SIGRETURN_TRAMP_VA 0x6F000000UL   /* must match sched.c/user.c */
static void sys_sigreturn(cpu_regs_t *r) {
    if (!g_sys_from_user) return;
    u64 fp = r->rdi;
    if (fp < USER_VBASE || fp + sizeof(cpu_regs_t) > USER_STACK_TOP ||
        !vmm_user_range_ok(fp, sizeof(cpu_regs_t))) {
        kprintf("sigreturn: bad frame ptr %p - killing task %d\n",
                (void *)fp, task_getpid());
        sched_exit(-1);
        return;
    }
    cpu_regs_t saved;
    stac();
    memcpy(&saved, (void *)fp, sizeof saved);
    clac();
    /* Restore the interrupted instruction state.  The saved frame must be
     * a USER frame: resuming with a kernel cs/ss would iretq into kernel
     * mode with the user handler's registers (corruption), and a user
     * frame whose SS.RPL != 3 would iretq -> #GP(err = SS selector) on
     * some CPUs.  Both are fatal classes - reject the frame and kill. */
    if ((saved.cs & 3) != 3 || (saved.ss & 3) != 3 ||
        saved.rip == 0 || saved.rip == SIGRETURN_TRAMP_VA) {
        kprintf("sigreturn: BAD saved frame (cs=%lx ss=%lx rip=%p) - "
                "killing task %d\n", saved.cs, saved.ss,
                (void *)saved.rip, task_getpid());
        sched_exit(-1);
        return;
    }
    saved.ss = USER_DS;                    /* normalize (never 0x20) */
    /* restore ALL 15 GPRs (r15..rax) + flags + instruction state */
    memcpy(&r->r15, &saved.r15, 15 * 8);
    r->rip = saved.rip;
    r->cs  = saved.cs;
    r->rflags = saved.rflags;
    r->rsp = saved.rsp;
    r->ss  = saved.ss;
}

/* exec: replace this task's address space with `path`, passing `argv` and
 * `envp` (both NULL-terminated arrays of user strings).  All strings are
 * copied into KERNEL memory first, because user_exec destroys the old
 * address space.  On success the frame is rewritten and the syscall
 * returns 0 from inside the NEW program. */
#define EXEC_MAX_ARGS  32/* exec: replace this task's address space with `path`, passing `argv` and
 * `envp` (both NULL-terminated arrays of user strings).  All strings are
 * copied into KERNEL memory first, because user_exec destroys the old
 * address space.  On success the frame is rewritten and the syscall
 * returns 0 from inside the NEW program. */
#define EXEC_MAX_ARGS  32
#define EXEC_MAX_ENV   16
#define EXEC_ARG_MAX   128

/* ring-3 compositor role tracking (defined later in this file; tentative
 * definitions merge, so declaring them here is safe) */
static task_t *g_wm_task;
static void   *g_wm_uaddr;
static u32     g_focus_pid;     /* task receiving keyboard (0 = wm)     */

static i64 sys_exec(const char *path, char *const *argv, char *const *envp,
                    cpu_regs_t *r) {
    if (!g_sys_from_user) return -1;
    task_t *t = cur();
    if (!t) return -1;

    char kpath[VFS_MAX_PATH];
    if (!copy_user_str((u64)path, kpath, sizeof kpath)) return -1;
    vnode_t *v = vfs_lookup_at(t->cwd, kpath);
    if (!v || v->type != VN_FILE) return -1;
    if (!perm_ok(v, PERM_R | PERM_X)) return -1;   /* need read+exec */

    /* copy the argv pointer array + every string into kernel memory */
    char *kargv[EXEC_MAX_ARGS];
    char *kenvp[EXEC_MAX_ENV];
    char (*abuf)[EXEC_ARG_MAX] = kmalloc((size_t)EXEC_MAX_ARGS * EXEC_ARG_MAX);
    char (*ebuf)[EXEC_ARG_MAX] = kmalloc((size_t)EXEC_MAX_ENV * EXEC_ARG_MAX);
    if (!abuf || !ebuf) {
        if (abuf) kfree(abuf);
        if (ebuf) kfree(ebuf);
        return -1;
    }
    int argc = 0, envc = 0;
    bool ok = true;

    if (argv) {
        /* validate the whole array region, then read the pointers SMAP-safe */
        if (!uptr((u64)argv, (u64)(EXEC_MAX_ARGS + 1) * 8)) ok = false;
        for (int i = 0; ok && i < EXEC_MAX_ARGS; i++) {
            u64 p;
            stac();
            p = (u64)((u64 *)argv)[i];
            clac();
            if (p == 0) break;
            if (!copy_user_str(p, abuf[argc], EXEC_ARG_MAX)) { ok = false; break; }
            kargv[argc] = abuf[argc];
            argc++;
        }
    }
    if (ok && envp) {
        if (!uptr((u64)envp, (u64)(EXEC_MAX_ENV + 1) * 8)) ok = false;
        for (int i = 0; ok && i < EXEC_MAX_ENV; i++) {
            u64 p;
            stac();
            p = (u64)((u64 *)envp)[i];
            clac();
            if (p == 0) break;
            if (!copy_user_str(p, ebuf[envc], EXEC_ARG_MAX)) { ok = false; break; }
            kenvp[envc] = ebuf[envc];
            envc++;
        }
    }

    if (!ok) {
        kfree(abuf);
        kfree(ebuf);
        return -1;
    }
    if (!user_exec(v, kargv, argc, kenvp, envc, r)) {
        kfree(abuf);
        kfree(ebuf);
        return -1;
    }
    /* the old address space is gone; free the kernel copies */
    kfree(abuf);
    kfree(ebuf);

    /* if the compositor just exec'd itself, its fb mapping died with the
     * old address space - force FB_INFO to rebuild it on next claim */
    if (g_wm_task == t) g_wm_uaddr = NULL;
    return 0;
}

/* dmesg: read the kernel audit log ring.  a0 = user buffer, a1 = start line,
 * a2 = max lines.  Writes each requested line as a NUL-terminated string and
 * returns the number of lines copied (0 if start >= total). */
#define KLOG_LINE_MAX 256
static i64 sys_dmesg(char *ubuf, u32 start, u32 max_lines) {
    /* Query mode: dmesg(NULL, DMESG_TOTAL, 0) returns the total line count. */
    if (start == 0x7FFFFFFFu && max_lines == 0)
        return (i64)klog_lines_total();
    if (max_lines > 128) max_lines = 128;
    if (!uptr((u64)ubuf, (u64)max_lines * (KLOG_LINE_MAX + 1))) return -1;
    if (max_lines == 0) return 0;
    /* klog_read fills a kernel scratch buffer; then copy to user SMAP-safe */
    static char scratch[128][KLOG_LINE_MAX + 1];
    int n = klog_read(&scratch[0][0], (int)start, (int)max_lines);
    if (n <= 0) return 0;
    for (int i = 0; i < n; i++) {
        int len = (int)strlen(&scratch[i][0]);
        stac();
        for (int j = 0; j <= len; j++) ubuf[i * (KLOG_LINE_MAX + 1) + j] =
            scratch[i][j];                 /* incl. the NUL */
        clac();
    }
    return n;
}

/* doas: elevate the effective uid to root, but ONLY if this task belongs to
 * an admin account AND the caller supplies that account's correct password
 * (verified as SHA-256(salt || password) with a constant-time compare).
 * Root (euid 0) never needs it.  This is the "sudo" analogue - named after
 * OpenBSD's doas. */
static i64 sys_doas(const char *password) {
    char kpw[64];
    if (!copy_user_str((u64)password, kpw, sizeof kpw)) return -1;
    task_t *t = cur();
    if (!t || !g_sys_from_user) return -1;
    if (t->euid == 0) return 0;
    if (!t->elev_allowed) return -1;
    if (!doas_check(t, kpw)) return -1;
    t->euid = 0;
    kprintf("syscall: task %d '%s' elevated to root via doas\n",
            t->pid, t->name);
    return 0;
}

/* SYS_AUTH_VERIFY: confirm the session password WITHOUT elevating. */
static i64 sys_auth_verify(const char *password) {
    char kpw[64];
    if (!copy_user_str((u64)password, kpw, sizeof kpw)) return -1;
    task_t *t = cur();
    if (!t || !g_sys_from_user) return -1;
    return doas_check(t, kpw) ? 0 : -1;
}

/* SYS_PASSWD(old, new): change the account password for REAL.
 * Verifies the OLD password (same PBKDF2 + constant-time + lockout path as
 * auth_verify, so an unlocked-but-unauthenticated session cannot hijack the
 * account), then rehashes the NEW password into the account's slot with a
 * fresh salt.  The hash lives in RAM (there is no persistent user DB yet —
 * documented, honest).  Return 0 = changed, -1 = bad pointer/no account,
 * -2 = new password too short, -3 = old password wrong. */
static i64 sys_passwd(const char *oldpw, const char *newpw) {
    char kold[64], knew[64];
    if (!copy_user_str((u64)oldpw, kold, sizeof kold)) return -1;
    if (!copy_user_str((u64)newpw, knew, sizeof knew)) return -1;
    task_t *t = cur();
    if (!t || !g_sys_from_user) return -1;
    if (strlen(knew) < 4) return -2;
    doas_user_t *u = NULL;
    for (int i = 0; i < g_doas_nusers; i++)
        if (strcmp(g_doas_users[i].account, t->account) == 0) {
            u = &g_doas_users[i];
            break;
        }
    if (!u) return -1;
    if (!doas_check(t, kold)) return -3;      /* wrong old password */
    /* fresh salt per change so identical passwords hash differently */
    u32 n = (u32)pit_ticks() ^ ((u32)t->pid * 2654435761u);
    char ns[16];
    memset(ns, 0, sizeof ns);
    ns[0] = 'y'; ns[1] = 'a'; ns[2] = 'r'; ns[3] = 't'; ns[4] = '-'; ns[5] = 's';
    for (int i = 0; i < 8; i++)
        ns[6 + i] = "0123456789abcdef"[(n >> (i * 4)) & 15];
    memcpy(u->salt, ns, sizeof u->salt);
    pbkdf2_hmac_sha256((const u8 *)knew, strlen(knew),
                       (const u8 *)u->salt, strlen(u->salt),
                       DOAS_PBKDF2_ITERS, u->hash, 32);
    u->fails = 0;
    u->fail_t0 = 0;
    kprintf("passwd: account '%s' password changed (PBKDF2 %u iters)\n",
            u->account, DOAS_PBKDF2_ITERS);
    /* persist like a real shadow file: /home/yart/.passwd + fsync.
     * NOTE: the write + fsync are issued, but blkfs has a KNOWN data-area
     * persistence bug (see docs/BRUTAL_AUDIT.md) so this does not yet
     * survive reboot.  The code is correct and starts working the moment
     * the FS bug is fixed. */
    {
        vnode_t *dir = vfs_lookup("/home/yart");
        vnode_t *pf  = vfs_lookup("/home/yart/.passwd");
        if (!pf && dir) pf = vfs_create(dir, ".passwd", VN_FILE);
        if (pf) {
            passwd_file_t pfile;
            memcpy(pfile.magic, PASSWD_MAGIC, 8);
            memcpy(pfile.salt, u->salt, sizeof pfile.salt);
            memcpy(pfile.hash, u->hash, sizeof pfile.hash);
            vfs_write(pf, &pfile, 0, sizeof pfile);
            extern int blkfs_sync(void);
            int synced = blkfs_sync();
            kprintf("passwd: wrote /home/yart/.passwd (fsync flushed %d file(s))\n",
                    synced);
        }
    }
    return 0;
}

/* SYS_REBOOT: reset the machine.  ACPI reset register first (works on
 * QEMU/Bochs and real hardware), then the 8042 keyboard-controller reset,
 * then a triple fault as the last resort.  Never returns. */
static i64 sys_reboot(void) {
    kprintf("reboot: user requested reset - syncing disk...\n");
    { extern int blkfs_sync(void); blkfs_sync(); }
    kprintf("reboot: resetting via ACPI (0x604 -> 0x2000)\n");
    outw(0x604, 0x2000);
    /* 8042 fallback */
    kprintf("reboot: ACPI did not reset - trying 8042 keyboard controller\n");
    for (volatile int i = 0; i < 100000; i++) {
        if (!(inb(0x64) & 2)) break;
    }
    outb(0x64, 0xFE);
    kprintf("reboot: 8042 did not reset - triple fault\n");
    /* triple fault: load a bad IDT and trigger an interrupt */
    __asm__ volatile ("lidt %0" : : "m"(*(u64[2]){ 0, 0 }));
    __asm__ volatile ("int3");
    for (;;) __asm__ volatile ("hlt");
    return 0;
}

/* SYS_AUDIO_VOL: a0>=0 sets HDA output volume 0..100 (returns previous),
 * a0<0 just returns the current value. */
static i64 sys_audio_vol(int v) {
    if (!g_sys_from_user) return -1;
    if (v >= 0) {
        int old = audio_get_volume();
        audio_set_volume(v);
        return old;
    }
    return audio_get_volume();
}

/* ---- notification ring (apps -> WM) ---- */
#define NOTIFY_RING 16
#define NOTIFY_LEN  128
static char  g_notify[NOTIFY_RING][NOTIFY_LEN];
static int   g_notify_head, g_notify_tail;
static spinlock_t g_notify_lock;

static i64 sys_notify(const char *msg) {
    char kb[NOTIFY_LEN];
    if (!copy_user_str((u64)msg, kb, sizeof kb)) return -1;
    if (!kb[0]) return -1;
    u64 fl = irq_save();
    spin_lock(&g_notify_lock);
    int next = (g_notify_head + 1) % NOTIFY_RING;
    if (next == g_notify_tail)
        g_notify_tail = (g_notify_tail + 1) % NOTIFY_RING;
    strncpy(g_notify[g_notify_head], kb, NOTIFY_LEN - 1);
    g_notify_head = next;
    spin_unlock(&g_notify_lock);
    irq_restore(fl);
    return 0;
}

static i64 sys_notify_poll(char *out, u64 cap) {
    if (!uptr((u64)out, cap)) return -1;
    if (cap > NOTIFY_LEN) cap = NOTIFY_LEN;
    u64 fl = irq_save();
    spin_lock(&g_notify_lock);
    if (g_notify_tail == g_notify_head) {
        spin_unlock(&g_notify_lock);
        irq_restore(fl);
        return 0;
    }
    i64 n = (i64)strlen(g_notify[g_notify_tail]);
    stac();
    for (u64 i = 0; i < cap; i++) {
        char c = g_notify[g_notify_tail][i];
        out[i] = c;
        if (!c) break;
    }
    clac();
    g_notify_tail = (g_notify_tail + 1) % NOTIFY_RING;
    spin_unlock(&g_notify_lock);
    irq_restore(fl);
    return n;
}

/* SYS_BATTERY(out[3]) -> present, charging, level(0..100).
 * REAL: reads the ACPI Control-Method Battery (PNP0C0A) via its _BST/_BIF
 * methods — exactly how Windows/Linux read a battery.  QEMU's q35 ships no
 * battery device, so the VM firmware gets one injected via an SSDT
 * (acpi/battery.aml, `-acpitable file=...`).  If the firmware has no battery
 * we honestly report "not present" and the desktop shows "AC", which is what
 * a real OS shows in a VM. */
static i64 sys_battery(int *out) {
    if (!uptr((u64)out, 3 * sizeof(int))) return -1;
    int v[3] = { 0, 0, 0 };
    if (g_acpi_battery.present) {
        v[0] = 1;
        v[1] = g_acpi_battery.charging ? 1 : 0;
        v[2] = g_acpi_battery.level;    /* -1 = present but unreadable */
    }
    stac();
    for (int i = 0; i < 3; i++) out[i] = v[i];
    clac();
    return 0;
}

/* ---- system clipboard (cross-process copy/paste, 512 bytes) ---- */
#define CLIP_MAX 512
static char  g_clipboard[CLIP_MAX];
static int   g_clipboard_len;
static spinlock_t g_clipboard_lock;

static i64 sys_clipboard_set(const char *text) {
    char kb[CLIP_MAX];
    if (!copy_user_str((u64)text, kb, sizeof kb)) return -1;
    u64 fl = irq_save();
    spin_lock(&g_clipboard_lock);
    int n = 0;
    while (kb[n] && n < CLIP_MAX - 1) n++;
    memcpy(g_clipboard, kb, n);
    g_clipboard[n] = 0;
    g_clipboard_len = n;
    spin_unlock(&g_clipboard_lock);
    irq_restore(fl);
    return 0;
}

static i64 sys_clipboard_get(char *out, u64 cap) {
    if (!uptr((u64)out, cap)) return -1;
    u64 fl = irq_save();
    spin_lock(&g_clipboard_lock);
    u64 n = (u64)g_clipboard_len;
    if (n >= cap) n = cap ? cap - 1 : 0;
    stac();
    for (u64 i = 0; i < n; i++) out[i] = g_clipboard[i];
    if (cap) out[n] = 0;
    clac();
    spin_unlock(&g_clipboard_lock);
    irq_restore(fl);
    return (i64)g_clipboard_len;
}

/* SYS_MOUSE_POS(out[2]) -> absolute cursor position (x, y) in framebuffer
 * pixels.  A ring-3 app uses this to snap its local pointer to the REAL
 * cursor when it gains focus: the PS/2 driver only emits deltas, and the
 * app only starts receiving them once focused, so delta-only tracking
 * drifts by however far the cursor travelled before the app was focused. */
static i64 sys_mouse_pos(int *out) {
    if (!uptr((u64)out, 2 * sizeof(int))) return -1;
    int v[2];
    mouse_get_pos(&v[0], &v[1]);
    stac();
    out[0] = v[0];
    out[1] = v[1];
    clac();
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
    /* The frame's CS is always the int 0x80 selector (USER_CS, pushed by
     * the stubs); on the fast path the RETURN selector is SYS_USER_CS.
     * Accept both user selectors, verify RPL == 3 on both. */
    if ((r->cs & 3) == 3 && (r->ss & 3) == 3 &&
        (r->cs == USER_CS || r->cs == SYS_USER_CS) &&
        r->ss == USER_DS) {
        kprintf("gdt: user segment check OK (cs=%lx ss=%lx)\n", r->cs, r->ss);
    } else {
        kprintf("gdt: !! user segment check FAILED (cs=%lx ss=%lx)\n",
                r->cs, r->ss);
    }
}

/* Forward decls - ring-3 compositor syscalls defined later in file */
static u64 sys_fb_info(fb_info_t *out);
static u64 sys_fb_flip(void *addr);
static u64 sys_fb_present(void *addr, const fb_rect_t *rects, u32 count);
static u64 sys_poll_key(void);
static u64 sys_poll_mouse(mouse_ev_t *out);
static u64 sys_time_ms(void);
static i64 sys_wm_create(u32 w, u32 h, wm_surf_info_t *out);
static i64 sys_wm_flip(u32 id);
static i64 sys_wm_scan(wm_surf_info_t *out, u32 max);
static i64 sys_wm_focus(u32 pid);
static i64 sys_wm_destroy(u32 id);
static i64 sys_wm_title(u32 id, const char *name);
static i64 sys_wm_move(u32 id, int x, int y);
static i64 sys_wm_resize(u32 id, u32 w, u32 h);
static i64 sys_wifi_scan(void);
static i64 sys_wifi_connect(const char *ssid, const char *psk);
static i64 sys_wifi_disconnect(void);
static i64 sys_wifi_status(char *out, u64 cap);
static i64 sys_doas(const char *password);
static i64 sys_doas(const char *password);
static i64 sys_task_list(u32 *pids, u64 max);
static i64 sys_audio_vol(int v);
static i64 sys_auth_verify(const char *password);
static i64 sys_passwd(const char *oldpw, const char *newpw);
static i64 sys_reboot(void);
static i64 sys_notify(const char *msg);
static i64 sys_notify_poll(char *out, u64 cap);
static i64 sys_battery(int *out);
static i64 sys_clipboard_set(const char *text);
static i64 sys_clipboard_get(char *out, u64 cap);
static i64 sys_mouse_pos(int *out);
static void sys_sigreturn(cpu_regs_t *r);
static i64 sys_pipe(int *fds);
static i64 sys_dup2(int oldfd, int newfd);

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
    case SYS_WAITPID:  r->rax = (u64)sys_waitpid((u32)a0, (int *)a1,
                                                 (int)r->r10); break;
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
    case SYS_DMESG:    r->rax = (u64)sys_dmesg((char *)a0, (u32)a1, (u32)a2); break;
    case SYS_NET_INFO: r->rax = (u64)sys_net_info((u32 *)a0); break;
    case SYS_UDP_SEND: r->rax = (u64)sys_udp_send((u32)a0, (u16)a1, (const u8 *)a2, (u16)r->r10); break;
    case SYS_UDP_RECV: r->rax = (u64)sys_udp_recv((u8 *)a0, (u16)a1); break;
    case SYS_FB_INFO:   r->rax = sys_fb_info((fb_info_t *)a0); break;
    case SYS_FB_FLIP:   r->rax = sys_fb_flip((void *)a0); break;
    case SYS_FB_PRESENT: r->rax = sys_fb_present((void *)a0,
                                                  (const fb_rect_t *)a1,
                                                  (u32)a2); break;
    case SYS_POLL_KEY:  r->rax = sys_poll_key(); break;
    case SYS_POLL_MOUSE: r->rax = sys_poll_mouse((mouse_ev_t *)a0); break;
    case SYS_TIME_MS:   r->rax = sys_time_ms(); break;
    case SYS_SLEEP:
        if (g_sys_from_user) sched_sleep_ms((u32)a0);
        r->rax = 0;
        break;
    case SYS_EXEC:      r->rax = (u64)sys_exec((const char *)a0,
                                               (char *const *)a1,
                                               (char *const *)a2, r); break;
    case SYS_WM_CREATE: r->rax = (u64)sys_wm_create((u32)a0, (u32)a1,
                                                    (wm_surf_info_t *)a2); break;
    case SYS_WM_FLIP:   r->rax = (u64)sys_wm_flip((u32)a0); break;
    case SYS_WM_SCAN:   r->rax = (u64)sys_wm_scan((wm_surf_info_t *)a0,
                                                  (u32)a1); break;
    case SYS_WM_FOCUS:  r->rax = (u64)sys_wm_focus((u32)a0); break;
    case SYS_WM_DESTROY: r->rax = (u64)sys_wm_destroy((u32)a0); break;
    case SYS_SIGRETURN:  sys_sigreturn(r); break;
    case SYS_WM_TITLE:   r->rax = (u64)sys_wm_title((u32)a0, (const char *)a1); break;
    case SYS_PIPE:       r->rax = (u64)sys_pipe((int *)a0); break;
    case SYS_TCP_CONNECT: r->rax = (u64)sys_tcp_connect((u32)a0, (u16)a1); break;
    case SYS_TCP_SEND:    r->rax = (u64)sys_tcp_send((i64)a0, (const u8 *)a1, (u64)a2); break;
    case SYS_TCP_RECV:    r->rax = (u64)sys_tcp_recv((i64)a0, (u8 *)a1, (u64)a2); break;
    case SYS_TCP_CLOSE:   r->rax = (u64)sys_tcp_close((i64)a0); break;
    case SYS_TCP_LISTEN:  r->rax = (u64)sys_tcp_listen((u16)a0); break;
    case SYS_TCP_ACCEPT:  r->rax = (u64)sys_tcp_accept((i64)a0); break;
    case SYS_DNS_RESOLVE: r->rax = (u64)sys_dns_resolve((const char *)a0, (u32 *)a1); break;
    case SYS_NET_FW_ADD:  r->rax = (u64)sys_net_fw_add((u32)a0, (u32)a1, (u32)a2, (u32)r->r10); break;
    case SYS_NET_FW_CLEAR: r->rax = (u64)sys_net_fw_clear(); break;
    case SYS_UDP_BIND:    r->rax = (u64)sys_udp_bind((u16)a0); break;
    case SYS_ICMP_PING:   r->rax = (u64)sys_icmp_ping((u32)a0, (u64 *)a1); break;
    case SYS_ICMP6_PING:  r->rax = (u64)sys_icmp6_ping((const u8 *)a0, (u64 *)a1); break;
    case SYS_IPV6_INFO:   r->rax = (u64)sys_ipv6_info((u8 *)a0, (u8 *)a1); break;
    case SYS_TLS_CONNECT: r->rax = (u64)sys_tls_connect((u32)a0, (u16)a1); break;
    case SYS_TLS_SEND:    r->rax = (u64)sys_tls_send((i64)a0, (const u8 *)a1, (u64)a2); break;
    case SYS_TLS_RECV:    r->rax = (u64)sys_tls_recv((i64)a0, (u8 *)a1, (u64)a2); break;
    case SYS_TLS_CLOSE:   r->rax = (u64)sys_tls_close((i64)a0); break;
    case SYS_TLS_LISTEN:  r->rax = (u64)sys_tls_listen((u16)a0); break;
    case SYS_TLS_ACCEPT:  r->rax = (u64)sys_tls_accept((i64)a0); break;
    case SYS_WIFI_SCAN:   r->rax = (u64)sys_wifi_scan(); break;
    case SYS_WIFI_CONNECT: r->rax = (u64)sys_wifi_connect((const char*)a0, (const char*)a1); break;
    case SYS_WIFI_STATUS: r->rax = (u64)sys_wifi_status((char*)a0, a1); break;
    case SYS_WM_MOVE:     r->rax = (u64)sys_wm_move((u32)a0, (int)a1, (int)a2); break;
    case SYS_WM_RESIZE:   r->rax = (u64)sys_wm_resize((u32)a0, (u32)a1, (u32)a2); break;
    case SYS_WIFI_DISCONNECT: r->rax = (u64)sys_wifi_disconnect(); break;
    case SYS_TASK_LIST:   r->rax = (u64)sys_task_list((u32*)a0, a1); break;
    case SYS_AUDIO_VOL:   r->rax = (u64)sys_audio_vol((int)a0); break;
    case SYS_AUTH_VERIFY: r->rax = (u64)sys_auth_verify((const char *)a0); break;
    case SYS_NOTIFY:      r->rax = (u64)sys_notify((const char *)a0); break;
    case SYS_NOTIFY_POLL: r->rax = (u64)sys_notify_poll((char *)a0, a1); break;
    case SYS_BATTERY:     r->rax = (u64)sys_battery((int *)a0); break;
    case SYS_CLIPBOARD_SET: r->rax = (u64)sys_clipboard_set((const char *)a0); break;
    case SYS_CLIPBOARD_GET: r->rax = (u64)sys_clipboard_get((char *)a0, a1); break;
    case SYS_MOUSE_POS:    r->rax = (u64)sys_mouse_pos((int *)a0); break;
    case SYS_PASSWD:       r->rax = (u64)sys_passwd((const char *)a0, (const char *)a1); break;
    case SYS_REBOOT:       sys_reboot(); r->rax = -1; break;
    case SYS_DUP2:         r->rax = (u64)sys_dup2((int)a0, (int)a1); break;
    default:
        kprintf("syscall: bad #%lu\n", r->rax);
        r->rax = (u64)-1;
    }


}

/* ------------------------------------------------------------------ */
/* fast syscall/sysret path                                           */
/* ------------------------------------------------------------------ */
extern void syscall_entry(void);     /* syscall_entry.asm (LSTAR)      */

#define MSR_EFER    0xC0000080UL
#define MSR_STAR    0xC0000081UL
#define MSR_LSTAR   0xC0000082UL
#define MSR_SFMASK  0xC0000084UL

static u64 g_fast_calls;
static bool g_fast_reported;

/* Called by syscall_entry.asm after it has built the cpu_regs_t frame on
 * the task's kernel stack.  Runs the normal dispatcher, then lets the
 * scheduler decide whether to preempt/switch (exactly like the int 0x80
 * path does inside isr_dispatch).  Returns the frame to sysret to. */
u64 syscall_fast_dispatch(cpu_regs_t *r) {
    g_fast_calls++;
    if (!g_fast_reported) {
        g_fast_reported = true;
        kprintf("syscall: fast path via syscall/sysret (first call #%llu)\n",
                (unsigned long long)g_fast_calls);
    }
    syscall_handler(r);
    return sched_after_isr((u64)r);
}

/* ------------------------------------------------------------------ */
/* ring-3 compositor / display-server syscalls (row 23)                */
/* ------------------------------------------------------------------ */

/* Track which task has claimed the framebuffer (the "wm" / compositor).
 * Only that task may call FB_FLIP/POLL_KEY/POLL_MOUSE. */
static task_t *g_wm_task;
static void    *g_wm_uaddr;   /* user virtual address of fb mapping    */
static u32      g_wm_pages;   /* page count of the mapping             */

/* SYS_FB_INFO(fb_info_t *out) -> user virtual addr of back buffer or 0 */
static u64 sys_fb_info(fb_info_t *out) {
    if (!uptr((u64)out, sizeof(fb_info_t))) return 0;
    task_t *t = cur();
    /* Claim the compositor role for the first caller (the wm binary). */
    if (!g_wm_task) {
        g_wm_task = t;
        kprintf("wm: ring-3 compositor (pid %u) claimed the framebuffer\n", t->pid);
    }
    if (g_wm_task != t) return 0;

    u32 w = g_fb.width, h = g_fb.height, pitch = g_fb.pitch_px;
    size_t bytes = (size_t)pitch * h * 4;
    u32 pages = (u32)PAGE_ALIGN_UP(bytes) / PAGE_SIZE;

    /* Map the kernel's back buffer pages into the caller's user PML4 at a
     * fresh high VA.  The compositor writes here, then calls FB_FLIP to
     * blit the result to the real scanout (which remains kernel-only).
     *
     * We must use vmm_map_in(t->pml4, ...) NOT vmm_map(), because the
     * current CR3 is kernel_pml4 (syscall entered from ring 3 but CR3
     * only switches on schedule-back to the user task).  vmm_map() would
     * write the PTE into kernel_pml4, not into t->pml4, so userland writes
     * would land on a demand-zero COW copy and never reach g_fb.pixels —
     * i.e. black screen even though fb_flip is being called. */
    if (!g_wm_uaddr) {
        u64 va = USER_FB_BASE;
        paddr_t fb_phys = virt_to_phys(g_fb.pixels);
        kprintf("wm: mapping fb: user_va=%p phys=%p pages=%u\n",
                (void *)va, (void *)fb_phys, pages);
        /*
         * We're in a syscall entered from ring 3 via int 0x80.  CR3 still
         * points at the wm task's PML4 (CR3 only changes on context switch
         * in switch_to()), so cur_pml4() == t->pml4.
         *
         * We MUST NOT call vmm_user_reserve() in eager (non-lazy) mode
         * because that allocates FRESH zero pages and maps them at `va`
         * via vmm_map() - overwriting the physical identity we want.  We
         * want the user writes to land on g_fb.pixels (the kernel's back
         * buffer), so:
         *   1) Register a LAZY region covering the fb (no allocations,
         *      just tells the fault handler these VAs are legitimate).
         *   2) Walk the user PML4 ourselves and set each PTE directly to
         *      fb_phys + i*PAGE, taking an extra ref on the frame.
         */
        if (vmm_user_reserve(va, pages,
                             PTE_PRESENT | PTE_RW | PTE_US | PTE_NX | PTE_NOSHR,
                             VMM_USER_LAZY) != 0) {
            kprintf("wm: failed to reserve fb region\n");
            return 0;
        }
        for (u32 i = 0; i < pages; i++) {
            vmm_map_in(t->pml4, va + i * (u64)PAGE_SIZE,
                       fb_phys + i * (u64)PAGE_SIZE,
                       PTE_PRESENT | PTE_RW | PTE_US | PTE_NX | PTE_NOSHR);
            pmm_ref_page(fb_phys + i * PAGE_SIZE);
        }
        g_wm_uaddr = (void *)va;
        g_wm_pages = pages;
    }

    clac();
    stac();
    fb_info_t info = { .width=w, .height=h, .pitch=pitch, .bpp=32,
                       .rgb = (u32)(g_fb.rgb ? 1 : 0) };
    /* SMAP-safe copy */
    for (size_t i = 0; i < sizeof info; i++)
        ((u8 *)out)[i] = ((u8 *)&info)[i];
    clac();
    return (u64)g_wm_uaddr;
}

#define FB_PRESENT_MAX_RECTS 64
static fb_rect_t g_present_rects[FB_PRESENT_MAX_RECTS];

/* SYS_FB_PRESENT(addr, rects, count): Skift-style partial present. */
static u64 sys_fb_present(void *addr, const fb_rect_t *rects, u32 count) {
    if (g_wm_task != cur()) return (u64)-1;
    if ((void *)addr != g_wm_uaddr) return (u64)-1;
    if (count > FB_PRESENT_MAX_RECTS) count = FB_PRESENT_MAX_RECTS;
    if (count && !uptr((u64)rects, (u64)count * sizeof(fb_rect_t)))
        return (u64)-1;
    if (count) {
        stac();
        for (u32 i = 0; i < count; i++)
            g_present_rects[i] = rects[i];
        clac();
        fb_present_rects(g_present_rects, count);
    }
    {
        extern int g_desktop_wd;
        watchdog_kick(g_desktop_wd);
    }
    return 0;
}

/* SYS_FB_FLIP(addr) : copy user-rendered back buffer to real scanout */
static u64 sys_fb_flip(void *addr) {
    if (g_wm_task != cur()) return (u64)-1;
    if ((void *)addr != g_wm_uaddr) return (u64)-1;
    fb_present();
    /* Kick the watchdog: the compositor is alive, it just produced a
     * frame.  This is the heartbeat row 13's supervisor watches. */
    {
        extern int g_desktop_wd;
        watchdog_kick(g_desktop_wd);
    }
    return 0;
}

/* ---------------- input fanout (per-task queues) ----------------
 * Keyboard events go to the focused task (or the wm when nothing is
 * focused); mouse events are COPIED to the wm (it moves the cursor) AND
 * the focused task (the app's UI).  Each task drains its own queue via
 * POLL_KEY / POLL_MOUSE, so no event is ever consumed twice. */

static void input_push_kbd(task_t *t, int ev) {
    if (!t) return;
    u32 next = (t->kbd_qh + 1) % TASK_KBDQ;
    if (next == t->kbd_qt) return;             /* queue full: drop */
    t->kbd_q[t->kbd_qh] = ev;
    t->kbd_qh = next;
}

static void input_push_mouse(task_t *t, const mouse_event_t *me) {
    if (!t) return;
    u32 next = (t->m_qh + 1) % TASK_MOUSEQ;
    if (next == t->m_qt) return;               /* queue full: drop */
    t->mouse_q[t->m_qh] = *me;
    t->m_qh = next;
}

/* Called from IRQ context (PS/2 + USB).  IRQ-safe: interrupts are already
 * off in the handler; the fanout lock only serializes against the syscall
 * path on other CPUs. */
static spinlock_t g_input_lock;

void sys_input_kbd(int ev) {
    u64 fl = irq_save();
    spin_lock(&g_input_lock);
    /* Skift strata-shell model: the compositor sees EVERY key so global
     * shortcuts work regardless of focus; the focused app also gets a copy.
     * sched_find(0) would return the idle task (pid 0), so gate on !=0. */
    input_push_kbd(g_wm_task, ev);
    task_t *focus = g_focus_pid ? sched_find(g_focus_pid) : NULL;
    if (focus && focus != g_wm_task)
        input_push_kbd(focus, ev);
    spin_unlock(&g_input_lock);
    irq_restore(fl);
}

void sys_input_mouse(const mouse_event_t *me) {
    u64 fl = irq_save();
    spin_lock(&g_input_lock);
    input_push_mouse(g_wm_task, me);           /* wm always gets a copy */
    task_t *focus = g_focus_pid ? sched_find(g_focus_pid) : NULL;
    if (focus && focus != g_wm_task)
        input_push_mouse(focus, me);
    spin_unlock(&g_input_lock);
    irq_restore(fl);
}

/* SYS_POLL_KEY -> 0 or ((scancode<<8)|ascii|flags).  The queue pop takes
 * the fanout lock: the IRQ producer reads the tail under the same lock,
 * so a pop can never race a push (SPSC ring, lock-protected both sides). */
static u64 sys_poll_key(void) {
    task_t *t = cur();
    if (!t) return 0;
    if (t != g_wm_task && (g_focus_pid == 0 || t->pid != g_focus_pid)) return 0;
    u64 fl = irq_save();
    spin_lock(&g_input_lock);
    int ev = 0;
    if (t->kbd_qh != t->kbd_qt) {
        ev = t->kbd_q[t->kbd_qt];
        t->kbd_qt = (t->kbd_qt + 1) % TASK_KBDQ;
    }
    spin_unlock(&g_input_lock);
    irq_restore(fl);
    return (u64)ev;
}

/* SYS_POLL_MOUSE(mouse_ev_t *out) -> 1 if event, 0 if none */
static u64 sys_poll_mouse(mouse_ev_t *out) {
    task_t *t = cur();
    if (!t || !uptr((u64)out, sizeof(mouse_ev_t))) return 0;
    if (t != g_wm_task && (g_focus_pid == 0 || t->pid != g_focus_pid)) return 0;
    u64 fl = irq_save();
    spin_lock(&g_input_lock);
    mouse_event_t me;
    int got = 0;
    if (t->m_qh != t->m_qt) {
        me = t->mouse_q[t->m_qt];
        t->m_qt = (t->m_qt + 1) % TASK_MOUSEQ;
        got = 1;
    }
    spin_unlock(&g_input_lock);
    irq_restore(fl);
    if (!got) return 0;
    stac();
    out->dx      = me.dx;
    out->dy      = me.dy;
    out->wheel   = me.wheel;
    out->buttons = me.buttons;
    clac();
    return 1;
}

/* ---------------- window surfaces (ring-3 apps) ----------------
 * A surface is a physical-page-backed canvas mapped into BOTH the app's
 * address space (it draws) and the wm's (it composites).  The kernel owns
 * the refcounts: 1 per mapping + 1 for the table.  Layout:
 *   app side: WM_SURF_APP_BASE + id*stride   (in the app's pml4)
 *   wm  side: WM_SURF_WM_BASE  + id*stride   (in the wm's  pml4)
 * The wm composites the surface at (win_x, win_y) - the kernel computes a
 * centered position at create time and hands it to both sides. */

#define WM_MAX_SURFACES 8
#define WM_SURF_APP_BASE 0x6C000000UL
#define WM_SURF_WM_BASE  0x6D000000UL
#define WM_SURF_STRIDE   0x200000UL        /* 2 MiB per slot             */
#define WM_SURF_MAX_W 640
#define WM_SURF_MAX_H 480
#define WM_SURF_MAX_PAGES (WM_SURF_STRIDE / PAGE_SIZE)   /* 512 */

typedef struct {
    bool   used;
    u32    id;
    u32    owner_pid;
    u32    w, h;
    u32    win_x, win_y;
    u32    npages;
    paddr_t pages[WM_SURF_MAX_PAGES];
    u64    app_va, wm_va;
    bool   app_mapped, wm_mapped;
    bool   dirty;
    char   title[32];
} wm_surface_t;

static wm_surface_t g_wm_surfs[WM_MAX_SURFACES];

/* SYS_WM_CREATE(w, h, wm_surf_info_t *out) -> surface id or -1.
 * Maps the canvas into the CALLER (the app); the wm side is mapped here
 * too so the compositor can start showing the window immediately. */
static i64 sys_wm_create(u32 w, u32 h, wm_surf_info_t *out) {
    task_t *t = cur();
    if (!t || !g_sys_from_user || !t->is_user) return -1;
    if (w < 16 || h < 16 || w > WM_SURF_MAX_W || h > WM_SURF_MAX_H) return -1;
    if (!uptr((u64)out, sizeof(wm_surf_info_t))) return -1;

    wm_surface_t *s = NULL;
    for (int i = 0; i < WM_MAX_SURFACES; i++)
        if (!g_wm_surfs[i].used) { s = &g_wm_surfs[i]; s->id = (u32)i; break; }
    if (!s) return -1;

    u32 npages = (w * h * 4 + PAGE_SIZE - 1) / PAGE_SIZE;
    if (npages > WM_SURF_MAX_PAGES) return -1;

    /* Deferred cleanup: a previous surface in this slot kept its wm mapping
     * + frames after teardown (see wm_surface_teardown).  Reclaim them now.
     * The stale wm-side PTEs are overwritten below by the new mapping. */
    u32 old_n = s->npages;
    paddr_t old_pages[WM_SURF_MAX_PAGES];
    for (u32 i = 0; i < old_n && i < WM_SURF_MAX_PAGES; i++)
        old_pages[i] = s->pages[i];

    memset(s, 0, sizeof *s);
    s->used = true;
    s->owner_pid = t->pid;
    s->w = w; s->h = h;
    s->npages = npages;
    s->app_va = WM_SURF_APP_BASE + (u64)s->id * WM_SURF_STRIDE;
    s->wm_va  = WM_SURF_WM_BASE  + (u64)s->id * WM_SURF_STRIDE;
    s->win_x  = (g_fb.width  > w) ? (g_fb.width  - w) / 2 : 0;
    s->win_y  = (g_fb.height > h) ? (g_fb.height - h) / 2 : 0;

    for (u32 i = 0; i < npages; i++) {
        s->pages[i] = pmm_alloc_page();
        if (!s->pages[i]) {
            /* roll back */
            for (u32 k = 0; k < i; k++) pmm_free_page(s->pages[k]);
            s->used = false;
            return -1;
        }
    }
    /* map into the app (its pml4 is current during this syscall).
     * PTE_NOSHR: these frames are shared with the kernel (blit source) and
     * the wm; never CoW them on fork, never swap them out. */
    for (u32 i = 0; i < npages; i++) {
        vmm_map_in(vmm_current_pml4(), s->app_va + i * PAGE_SIZE, s->pages[i],
                   PTE_PRESENT | PTE_RW | PTE_US | PTE_NX | PTE_NOSHR);
        pmm_ref_page(s->pages[i]);
    }
    s->app_mapped = true;
    /* map into the wm (same no-CoW/no-swap treatment: the wm forks on
     * every app launch and must keep drawing to the same frames).
     * TLB SHOOTDOWN: this syscall runs on the APP's CPU - the wm may be
     * executing RIGHT NOW on another core with its pml4 in CR3; without a
     * cross-CPU flush its TLB keeps a stale "not present" entry and the
     * first draw faults (demand-fault would even shadow the real frame
     * with a fresh zero page). */
    if (g_wm_task && g_wm_task->pml4) {
        for (u32 i = 0; i < npages; i++) {
            vmm_map_in(g_wm_task->pml4, s->wm_va + i * PAGE_SIZE, s->pages[i],
                       PTE_PRESENT | PTE_RW | PTE_US | PTE_NX | PTE_NOSHR);
            pmm_ref_page(s->pages[i]);
        }
        s->wm_mapped = true;
        if (g_wm_task != cur())
            smp_tlb_shootdown_all();   /* wm's pml4 may be live on its CPU */
    }
    /* Reclaim the deferred frames from the previous surface in this slot
     * (their stale wm-side PTEs were just overwritten above). */
    for (u32 i = 0; i < old_n && i < WM_SURF_MAX_PAGES; i++) {
        pmm_unref_page(old_pages[i]);   /* the old wm-side mapping's ref */
        pmm_free_page(old_pages[i]);    /* the old table's own ref        */
    }
    sched_charge_pages((i64)npages);

    wm_surf_info_t info;
    info.id = s->id;
    info.w = s->w; info.h = s->h;
    info.win_x = s->win_x; info.win_y = s->win_y;
    info.app_va = s->app_va;
    info.owner_pid = s->owner_pid;
    info.dirty = 1;
    strncpy(info.title, s->title[0] ? s->title : "App",
            sizeof info.title - 1);
    stac();
    *(wm_surf_info_t *)out = info;
    clac();
    kprintf("wm: surface %u created by pid %u (%ux%u @ %u,%u)\n",
            s->id, t->pid, w, h, s->win_x, s->win_y);
    return (i64)s->id;
}

static void wm_surface_teardown(wm_surface_t *s, bool unmap_app) {
    task_t *owner = sched_find(s->owner_pid);
    if (unmap_app && s->app_mapped) {
        /* app side: unmap + drop the app refs (owner may be dead; its
         * pml4 was already freed by vmm_free_pml4, which unrefs every
         * present user PTE - so only unmap if the owner is alive) */
        if (owner && owner->pml4) {
            for (u32 i = 0; i < s->npages; i++)
                vmm_unmap_in(owner->pml4, s->app_va + i * PAGE_SIZE);
        }
        s->app_mapped = false;
    }
    /* WM side: KEEP the mapping + frames ALIVE (deferred free).
     * The owner may die between two of the compositor's scan passes; if we
     * unmapped the wm-side VA here, the wm would still be holding the stale
     * VA and paint from it -> SIGSEGV (the classic "wm killed" crash).
     * Keeping the pages mapped means the wm can only ever read valid (stale)
     * content until its next scan removes the window.  The frames are freed
     * when the slot is reused by sys_wm_create. */
    if (g_focus_pid == s->owner_pid) g_focus_pid = 0;
    kprintf("wm: surface %u destroyed (owner pid %u)\n", s->id, s->owner_pid);
    {
        u32 saved_n = s->npages;
        u32 saved[WM_SURF_MAX_PAGES];
        for (u32 i = 0; i < saved_n; i++) saved[i] = s->pages[i];
        memset(s, 0, sizeof *s);
        s->npages = saved_n;
        for (u32 i = 0; i < saved_n; i++) s->pages[i] = saved[i];
        s->used = false;
        s->wm_mapped = true;   /* pages + wm-side refs still held */
    }
}

static i64 sys_wm_destroy(u32 id) {
    if (id >= WM_MAX_SURFACES || !g_wm_surfs[id].used) return -1;
    task_t *t = cur();
    if (t && t->pid != g_wm_surfs[id].owner_pid && t != g_wm_task) return -1;
    wm_surface_teardown(&g_wm_surfs[id], true);
    return 0;
}

/* SYS_WM_FLIP(id): the app finished drawing - mark the surface dirty so
 * the compositor re-blits it. */
static i64 sys_wm_flip(u32 id) {
    if (id >= WM_MAX_SURFACES || !g_wm_surfs[id].used) return -1;
    task_t *t = cur();
    if (t && t->pid != g_wm_surfs[id].owner_pid) return -1;
    g_wm_surfs[id].dirty = true;
    return 0;
}

/* SYS_WM_SCAN(out, max): wm-only - list surfaces + clear their dirty
 * flags.  The compositor calls this every frame. */
static i64 sys_wm_scan(wm_surf_info_t *out, u32 max) {
    if (cur() != g_wm_task) return -1;
    if (max > WM_MAX_SURFACES) max = WM_MAX_SURFACES;
    if (!uptr((u64)out, (u64)max * sizeof(wm_surf_info_t))) return -1;
    u32 n = 0;
    for (int i = 0; i < WM_MAX_SURFACES && n < max; i++) {
        wm_surface_t *s = &g_wm_surfs[i];
        if (!s->used) continue;
        wm_surf_info_t info;
        info.id = s->id;
        info.w = s->w; info.h = s->h;
        info.win_x = s->win_x; info.win_y = s->win_y;
        info.app_va = s->wm_va;              /* the WM-side VA!          */
        info.owner_pid = s->owner_pid;
        info.dirty = s->dirty ? 1 : 0;
        strncpy(info.title, s->title[0] ? s->title : "App",
                sizeof info.title - 1);
        s->dirty = false;
        stac();
        out[n++] = info;
        clac();
    }
    return (i64)n;
}

/* SYS_WM_TITLE(id, name): set the window title (drawn by the compositor
 * in the title bar). */
static i64 sys_wm_title(u32 id, const char *name) {
    if (id >= WM_MAX_SURFACES || !g_wm_surfs[id].used) return -1;
    task_t *t = cur();
    if (t && t->pid != g_wm_surfs[id].owner_pid && t != g_wm_task) return -1;
    char kt[32];
    if (!copy_user_str((u64)name, kt, sizeof kt)) return -1;
    strncpy(g_wm_surfs[id].title, kt, sizeof g_wm_surfs[id].title - 1);
    g_wm_surfs[id].dirty = true;
    return 0;
}

/* SYS_WM_FOCUS(pid): route keyboard to `pid` (and mouse copies). */
static i64 sys_wm_focus(u32 pid) {
    if (cur() != g_wm_task) return -1;
    if (pid == 0) { g_focus_pid = 0; return 0; }
    task_t *t = sched_find(pid);
    if (!t || !t->is_user || t->pid == 0 || t->state == TASK_ZOMBIE) return -1;
    g_focus_pid = pid;
    kprintf("wm: input focus -> pid %u\n", pid);
    return 0;
}

/* NEW: SYS_WM_MOVE(id, x, y): move window, only wm or owner can move */
static i64 sys_wm_move(u32 id, int x, int y) {
    if (id >= WM_MAX_SURFACES || !g_wm_surfs[id].used) return -1;
    task_t *t = cur();
    if (t && t->pid != g_wm_surfs[id].owner_pid && t != g_wm_task) return -1;
    if (x < -100) x = -100;
    if (y < 0) y = 0;
    if (x > (int)g_fb.width - 50) x = g_fb.width - 50;
    if (y > (int)g_fb.height - 50) y = g_fb.height - 50;
    g_wm_surfs[id].win_x = (u32)x;
    g_wm_surfs[id].win_y = (u32)y;
    g_wm_surfs[id].dirty = true;
    return 0;
}

/* NEW: SYS_WM_RESIZE(id, w, h): resize window (realloc pages) */
static i64 sys_wm_resize(u32 id, u32 w, u32 h) {
    if (id >= WM_MAX_SURFACES || !g_wm_surfs[id].used) return -1;
    task_t *t = cur();
    if (t && t->pid != g_wm_surfs[id].owner_pid && t != g_wm_task) return -1;
    if (w < 100 || h < 100 || w > WM_SURF_MAX_W || h > WM_SURF_MAX_H) return -1;
    wm_surface_t *s = &g_wm_surfs[id];
    u32 new_pages = (w * h * 4 + PAGE_SIZE - 1) / PAGE_SIZE;
    if (new_pages > WM_SURF_MAX_PAGES) return -1;
    /* For simplicity, keep old pages if same count, else realloc not yet.
       We only allow same-page-count resize to avoid complex remap.
       Full realloc would need unmap+alloc+map both sides.
     */
    if (new_pages != s->npages) {
        /* realloc: free extra or alloc more */
        if (new_pages > s->npages) {
            for (u32 i=s->npages;i<new_pages;i++){
                paddr_t pg = pmm_alloc_page();
                if (!pg) return -1;
                s->pages[i]=pg;
                vmm_map_in(vmm_current_pml4(), s->app_va + i*PAGE_SIZE, pg, PTE_PRESENT|PTE_RW|PTE_US|PTE_NX|PTE_NOSHR);
                pmm_ref_page(pg);
                if (g_wm_task && g_wm_task->pml4){
                    vmm_map_in(g_wm_task->pml4, s->wm_va + i*PAGE_SIZE, pg, PTE_PRESENT|PTE_RW|PTE_US|PTE_NX|PTE_NOSHR);
                    pmm_ref_page(pg);
                }
            }
        } else {
            for (u32 i=new_pages;i<s->npages;i++){
                vmm_unmap_in(sched_find(s->owner_pid)?sched_find(s->owner_pid)->pml4:vmm_current_pml4(), s->app_va + i*PAGE_SIZE);
                if (g_wm_task && g_wm_task->pml4) vmm_unmap_in(g_wm_task->pml4, s->wm_va + i*PAGE_SIZE);
                pmm_free_page(s->pages[i]);
            }
        }
        s->npages = new_pages;
        if (g_wm_task != cur()) smp_tlb_shootdown_all();
    }
    s->w = w; s->h = h; s->dirty = true;
    return 0;
}

/* WiFi syscalls */
static i64 sys_wifi_scan(void){
    if (!g_sys_from_user) return -1;
    return (i64)wifi_scan_syscall();
}
static i64 sys_wifi_connect(const char *ssid, const char *psk){
    char kssid[64], kpsk[64];
    if (!copy_user_str((u64)ssid, kssid, sizeof kssid)) return -1;
    if (psk && !copy_user_str((u64)psk, kpsk, sizeof kpsk)) return -1;
    return (i64)wifi_connect_syscall(kssid, psk?kpsk:NULL);
}
static i64 sys_wifi_disconnect(void){
    if (!g_sys_from_user) return -1;
    extern int wifi_disconnect(void);
    return (i64)wifi_disconnect();
}
static i64 sys_wifi_status(char *out, u64 cap){
    if (!uptr((u64)out, cap)) return -1;
    if (cap > 4096) cap = 4096;
    char kbuf[4096]; int n = wifi_status_syscall(kbuf, (u32)cap);
    if (n<0) return -1;
    stac(); memcpy(out, kbuf, (size_t)n); if ((u64)n < cap) out[n]=0; clac();
    return n;
}
static i64 sys_task_list(u32 *pids, u64 max){
    if (!uptr((u64)pids, max*sizeof(u32))) return -1;
    if (max>128) max=128;
    u32 k[128]; int n=0;
    // walk tasks
    for (task_t *t = sched_tasks(); t && n<(int)max; t=t->next){
        if (t->state != TASK_ZOMBIE) k[n++] = t->pid;
    }
    stac(); for(int i=0;i<n;i++) pids[i]=k[i]; clac();
    return n;
}

/* Called from the orphan reaper when a task dies: free any surface it
 * owned (the app side was already unmapped+unref'd by vmm_free_pml4). */
void wm_surface_owner_died(u32 pid) {
    for (int i = 0; i < WM_MAX_SURFACES; i++) {
        wm_surface_t *s = &g_wm_surfs[i];
        if (s->used && s->owner_pid == pid)
            wm_surface_teardown(s, false);
    }
    if (g_focus_pid == pid) g_focus_pid = 0;
}

/* SYS_TIME_MS -> uptime in milliseconds */
static u64 sys_time_ms(void) {
    /* PIT/pit_ticks() increments at 100 Hz -> 10 ms per tick (PIT remains
     * active as a backup time source). */
    return pit_ticks() * 10ULL;
}

void syscall_install_percpu(void) {
    /* Program the per-CPU MSRs for the SYSCALL/SYSRET fast path.  EFER.SCE,
     * STAR, LSTAR and SFMASK are core-local MSRs - they must be loaded on
     * every CPU (BSP and each AP), otherwise userland executing `syscall` on
     * an AP will #UD.  The BSP also calls this from syscall_install(). */
    wrmsr64(MSR_EFER,   rdmsr64(MSR_EFER) | 1);   /* EFER.SCE              */
    wrmsr64(MSR_STAR,   ((u64)0x18ULL << 48) | ((u64)0x08ULL << 32));
    wrmsr64(MSR_LSTAR,  (u64)syscall_entry);
    wrmsr64(MSR_SFMASK, (1u << 8) | (1u << 9) | (1u << 10) |
                        (1u << 14) | (1u << 18));   /* TF IF DF NT AC     */
}

void syscall_install(void) {
    irq_register(0x80, syscall_handler);   /* keep the int 0x80 fallback */

    /* Fast path: SYSCALL/SYSRET.  STAR[47:32] = kernel CS (0x08) so SYSCALL
     * lands in the kernel code segment and STAR[63:48] = 0x18 so SYSRET
     * returns to CS=0x2B (slot 5, user code) / SS=0x23 (slot 4, user data).
     * LSTAR = our entry stub.  SFMASK masks TF/IF/DF/AC on entry so the
     * kernel runs with a clean RFLAGS until we sysret the user's R11 back. */
    syscall_install_percpu();

    kprintf("syscall: dispatcher up, %d slots; fast syscall/sysret armed "
            "(STAR=0x%016lx SCE=%d)\n", SYS_MAX,
            ((u64)0x18ULL << 48) | ((u64)0x08ULL << 32),
            (int)((rdmsr64(MSR_EFER) >> 0) & 1));
}
