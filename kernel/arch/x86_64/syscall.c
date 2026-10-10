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
#define DOAS_LOCKOUT_TICKS MS_TO_TICKS(1000)  /* 1 s per failed attempt window */
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
static int alloc_fd_from(int min_fd) {
    task_t *t = cur(); if (!t) return -1;
    if (min_fd < 0) min_fd = 0;
    for (int i = min_fd; i < MAX_FD; i++) if (!t->fds[i].in_use) return i;
    return -1;
}
static int alloc_fd(void) {
    return alloc_fd_from(3);
}

/* ---------- pty and random helpers ---------- */
#define PTY_MAX 16
#define PTY_BUF_SZ 4096

typedef struct {
    bool in_use;
    bool locked;
    u32  id;
    u32  master_pid;
    u32  fg_pgrp;
    u8   in_buf[PTY_BUF_SZ];
    u32  in_head, in_tail, in_count;
    u8   out_buf[PTY_BUF_SZ];
    u32  out_head, out_tail, out_count;
    u32  c_iflag, c_oflag, c_cflag, c_lflag;
    u8   c_cc[32];
    u16  ws_row, ws_col, ws_xpixel, ws_ypixel;
} pty_state_t;

static pty_state_t g_ptys[PTY_MAX];
static spinlock_t  g_pty_lock;

static u64 linux_rand_u64(void) {
    static u64 s;
    if (!s) {
        u64 t; __asm__ volatile("rdtsc" : "=A"(t));
        s = t ^ ((u64)pit_ticks() << 32) ^ 0x9E3779B97F4A7C15ULL;
        if (!s) s = 1;
    }
    u64 x = s;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    return s = x;
}

static int pty_alloc(void) {
    u64 fl = irq_save(); spin_lock(&g_pty_lock);
    int slot = -1;
    for (int i = 0; i < PTY_MAX; i++) {
        if (!g_ptys[i].in_use) {
            slot = i;
            memset(&g_ptys[i], 0, sizeof(pty_state_t));
            g_ptys[i].in_use = true;
            g_ptys[i].locked = false;
            g_ptys[i].id = (u32)i;
            g_ptys[i].master_pid = cur() ? cur()->pid : 0;
            g_ptys[i].fg_pgrp = cur() ? (cur()->pgid ? cur()->pgid : cur()->pid) : 0;
            g_ptys[i].c_iflag = 0x0500;
            g_ptys[i].c_oflag = 0x0005;
            g_ptys[i].c_cflag = 0x00bf;
            g_ptys[i].c_lflag = 0x8a3b;
            g_ptys[i].c_cc[0] = 3;   /* VINTR = Ctrl+C */
            g_ptys[i].c_cc[1] = 28;  /* VQUIT */
            g_ptys[i].c_cc[2] = 127; /* VERASE */
            g_ptys[i].c_cc[3] = 21;  /* VKILL */
            g_ptys[i].c_cc[4] = 4;   /* VEOF = Ctrl+D */
            g_ptys[i].c_cc[5] = 0;   /* VTIME */
            g_ptys[i].c_cc[6] = 1;   /* VMIN */
            g_ptys[i].c_cc[10] = 26; /* VSUSP = Ctrl+Z */
            g_ptys[i].ws_row = 25;
            g_ptys[i].ws_col = 80;
            g_ptys[i].ws_xpixel = 640;
            g_ptys[i].ws_ypixel = 400;
            break;
        }
    }
    spin_unlock(&g_pty_lock); irq_restore(fl);
    return slot;
}

static int pty_slave_open(const char *name) {
    int id = 0;
    while (*name >= '0' && *name <= '9') { id = id * 10 + (*name - '0'); name++; }
    if (id < 0 || id >= PTY_MAX) return -1;
    u64 fl = irq_save(); spin_lock(&g_pty_lock);
    int ret = (g_ptys[id].in_use && !g_ptys[id].locked) ? id : -1;
    spin_unlock(&g_pty_lock); irq_restore(fl);
    return ret;
}

static i64 pty_write_master(int id, const char *buf, u64 len) {
    if (id < 0 || id >= PTY_MAX || !g_ptys[id].in_use) return -1;
    pty_state_t *p = &g_ptys[id];
    u64 written = 0;
    u64 fl = irq_save(); spin_lock(&g_pty_lock);
    for (u64 i = 0; i < len; i++) {
        u8 ch = (u8)buf[i];
        if (p->in_count < PTY_BUF_SZ) {
            p->in_buf[p->in_head] = ch;
            p->in_head = (p->in_head + 1) % PTY_BUF_SZ;
            p->in_count++;
            written++;
            if ((p->c_lflag & 0x0008 /* ECHO */) && p->out_count < PTY_BUF_SZ) {
                p->out_buf[p->out_head] = ch;
                p->out_head = (p->out_head + 1) % PTY_BUF_SZ;
                p->out_count++;
            }
        } else break;
    }
    spin_unlock(&g_pty_lock); irq_restore(fl);
    return (i64)written;
}

static i64 pty_read_master(int id, char *buf, u64 len) {
    if (id < 0 || id >= PTY_MAX || !g_ptys[id].in_use) return -1;
    pty_state_t *p = &g_ptys[id];
    u64 got = 0;
    u64 fl = irq_save(); spin_lock(&g_pty_lock);
    while (got < len && p->out_count > 0) {
        buf[got++] = (char)p->out_buf[p->out_tail];
        p->out_tail = (p->out_tail + 1) % PTY_BUF_SZ;
        p->out_count--;
    }
    spin_unlock(&g_pty_lock); irq_restore(fl);
    return (i64)got;
}

static i64 pty_write_slave(int id, const char *buf, u64 len) {
    if (id < 0 || id >= PTY_MAX || !g_ptys[id].in_use) return -1;
    pty_state_t *p = &g_ptys[id];
    u64 written = 0;
    u64 fl = irq_save(); spin_lock(&g_pty_lock);
    for (u64 i = 0; i < len; i++) {
        if (p->out_count < PTY_BUF_SZ) {
            p->out_buf[p->out_head] = (u8)buf[i];
            p->out_head = (p->out_head + 1) % PTY_BUF_SZ;
            p->out_count++;
            written++;
        } else break;
    }
    spin_unlock(&g_pty_lock); irq_restore(fl);
    return (i64)written;
}

static i64 pty_read_slave(int id, char *buf, u64 len) {
    if (id < 0 || id >= PTY_MAX || !g_ptys[id].in_use) return -1;
    pty_state_t *p = &g_ptys[id];
    u64 got = 0;
    u64 fl = irq_save(); spin_lock(&g_pty_lock);
    while (got < len && p->in_count > 0) {
        u8 ch = p->in_buf[p->in_tail];
        p->in_tail = (p->in_tail + 1) % PTY_BUF_SZ;
        p->in_count--;
        if (ch == p->c_cc[0] /* VINTR */) {
            if (p->fg_pgrp) sched_signal(p->fg_pgrp, 2 /* SIGINT */);
            continue;
        }
        if (ch == p->c_cc[10] /* VSUSP */) {
            if (p->fg_pgrp) sched_signal(p->fg_pgrp, 20 /* SIGTSTP */);
            continue;
        }
        if (ch == p->c_cc[4] /* VEOF */) {
            break;
        }
        buf[got++] = (char)ch;
        if ((p->c_lflag & 0x0002 /* ICANON */) && ch == '\n') break;
    }
    spin_unlock(&g_pty_lock); irq_restore(fl);
    return (i64)got;
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
    fd_entry_t *f = fd_get(fd);
    if (!f) {
        if (fd == 1 || fd == 2) {
            size_t k = n < sizeof g_klog_line - 1 ? n : sizeof g_klog_line - 1;
            stac(); memcpy(g_klog_line, buf, k); clac();
            g_klog_line[k] = 0;
            kputs(g_klog_line);
            return (i64)n;
        }
        return -1;
    }
    if (f->type == FD_TYPE_DEV_NULL || f->type == FD_TYPE_DEV_ZERO || f->type == FD_TYPE_DEV_RANDOM) {
        return (i64)n;
    }
    if (f->type == FD_TYPE_DEV_TTY) {
        size_t k = n < sizeof g_klog_line - 1 ? n : sizeof g_klog_line - 1;
        stac(); memcpy(g_klog_line, buf, k); clac();
        g_klog_line[k] = 0;
        kputs(g_klog_line);
        return (i64)n;
    }
    if (f->type == FD_TYPE_PTY_MASTER) {
        return pty_write_master((int)f->pty_id, buf, n);
    }
    if (f->type == FD_TYPE_PTY_SLAVE) {
        return pty_write_slave((int)f->pty_id, buf, n);
    }
    if (f->type == FD_TYPE_EVENTFD) {
        if (n < 8) return -1;
        u64 val; stac(); val = *(const u64 *)(const void *)buf; clac();
        f->eventfd_val += val;
        return 8;
    }
    if (f->is_pipe || f->type == FD_TYPE_PIPE) {
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
    if (!f->vn) {
        if (fd == 1 || fd == 2) {
            size_t k = n < sizeof g_klog_line - 1 ? n : sizeof g_klog_line - 1;
            stac(); memcpy(g_klog_line, buf, k); clac();
            g_klog_line[k] = 0;
            kputs(g_klog_line);
            return (i64)n;
        }
        return -1;
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
    if (!f) {
        if (fd <= 2) return 0;
        return -1;
    }
    if (f->type == FD_TYPE_DEV_NULL) return 0;
    if (f->type == FD_TYPE_DEV_ZERO) {
        stac(); memset(buf, 0, (size_t)n); clac();
        return (i64)n;
    }
    if (f->type == FD_TYPE_DEV_RANDOM) {
        u8 tmp[64];
        for (u64 off = 0; off < n; off += sizeof tmp) {
            u64 take = n - off; if (take > sizeof tmp) take = sizeof tmp;
            for (u64 i = 0; i < take; i += 8) {
                u64 v = linux_rand_u64();
                u64 k = take - i; if (k > 8) k = 8;
                memcpy(tmp + i, &v, (size_t)k);
            }
            stac(); memcpy(buf + off, tmp, (size_t)take); clac();
        }
        return (i64)n;
    }
    if (f->type == FD_TYPE_PTY_MASTER) {
        return pty_read_master((int)f->pty_id, buf, n);
    }
    if (f->type == FD_TYPE_PTY_SLAVE) {
        return pty_read_slave((int)f->pty_id, buf, n);
    }
    if (f->type == FD_TYPE_DEV_TTY) {
        task_t *t = cur();
        if (t && t->kbd_qh != t->kbd_qt) {
            u32 got = 0;
            while (got < n && t->kbd_qh != t->kbd_qt) {
                int ev = t->kbd_q[t->kbd_qt];
                t->kbd_qt = (t->kbd_qt + 1) % TASK_KBDQ;
                if (ev > 0 && ev < 256) {
                    stac(); buf[got++] = (char)ev; clac();
                }
            }
            if (got > 0) return got;
        }
        return 0;
    }
    if (f->type == FD_TYPE_EVENTFD) {
        if (n < 8) return -1;
        if (f->eventfd_val == 0) return -2;
        u64 val = f->eventfd_val;
        f->eventfd_val = 0;
        stac(); *(u64 *)(void *)buf = val; clac();
        return 8;
    }
    if (f->is_pipe || f->type == FD_TYPE_PIPE) {
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
    if (!f->vn) return 0;
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

    int fd = alloc_fd_from(0);
    if (fd < 0) return -1;
    fd_entry_t *f = &cur()->fds[fd];
    memset(f, 0, sizeof *f);

    if (strcmp(path, "/dev/null") == 0) {
        f->in_use = true; f->type = FD_TYPE_DEV_NULL; f->flags = (u32)flags; return fd;
    }
    if (strcmp(path, "/dev/zero") == 0) {
        f->in_use = true; f->type = FD_TYPE_DEV_ZERO; f->flags = (u32)flags; return fd;
    }
    if (strcmp(path, "/dev/urandom") == 0 || strcmp(path, "/dev/random") == 0) {
        f->in_use = true; f->type = FD_TYPE_DEV_RANDOM; f->flags = (u32)flags; return fd;
    }
    if (strcmp(path, "/dev/tty") == 0 || strcmp(path, "/dev/console") == 0) {
        f->in_use = true; f->type = FD_TYPE_DEV_TTY; f->flags = (u32)flags; return fd;
    }
    if (strcmp(path, "/dev/ptmx") == 0) {
        int p = pty_alloc();
        if (p < 0) return -1;
        f->in_use = true; f->type = FD_TYPE_PTY_MASTER; f->pty_id = (u32)p; f->flags = (u32)flags; return fd;
    }
    if (strncmp(path, "/dev/pts/", 9) == 0) {
        int p = pty_slave_open(path + 9);
        if (p < 0) return -1;
        f->in_use = true; f->type = FD_TYPE_PTY_SLAVE; f->pty_id = (u32)p; f->flags = (u32)flags; return fd;
    }

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

    vnode_ref(v);                        /* fd holds its own reference      */
    f->vn     = v;
    f->pos    = (flags & O_APPEND) ? v->size : 0;
    f->flags  = (u32)flags;
    f->type   = FD_TYPE_FILE;
    f->in_use = true;
    return fd;
}

static i64 sys_close(int fd) {
    fd_entry_t *f = fd_get(fd);
    if (!f || fd < 0 || fd >= MAX_FD) return -1;
    if (f->is_pipe && f->pipe) {
        yart_pipe_t *p = f->pipe;
        if (f->pipe_is_read_end) {
            if (p->read_ends > 0) p->read_ends--;
        } else {
            if (p->write_ends > 0) p->write_ends--;
        }
        pipe_put(p);
        f->pipe = NULL;
    } else if (f->type == FD_TYPE_PTY_MASTER) {
        if (f->pty_id < PTY_MAX) g_ptys[f->pty_id].in_use = false;
    } else if (f->vn) {
        vnode_unref(f->vn);              /* release the fd's reference      */
    }
    f->in_use = false;
    f->vn = NULL;
    f->is_pipe = false;
    f->type = 0;
    return 0;
}

/* dup2(oldfd, newfd): POSIX fd duplication.  Closes newfd if open, then
 * copies the oldfd entry onto it, taking a fresh reference on the shared
 * resource (vnode or pipe) so close() of either end is balanced. */
/* symlink(target, linkpath): create a symlink.  Returns 0 or -1. */
static i64 sys_symlink(const char *target, const char *linkpath) {
    char kt[VFS_MAX_PATH], kl[VFS_MAX_PATH];
    if (!copy_user_str((u64)target, kt, sizeof kt)) return -1;
    if (!copy_user_str((u64)linkpath, kl, sizeof kl)) return -1;
    task_t *t = cur();
    /* split linkpath into dir + base */
    char dir[VFS_MAX_PATH] = "/", base[VFS_MAX_NAME];
    const char *slash = NULL;
    for (const char *p = kl; *p; p++) if (*p == '/') slash = p;
    if (slash && slash != kl) {
        size_t l = slash - kl; if (l >= sizeof dir) return -1;
        memcpy(dir, kl, l); dir[l] = 0;
        strncpy(base, slash + 1, sizeof base - 1);
    } else if (slash) {
        strncpy(base, kl + 1, sizeof base - 1);
    } else {
        strncpy(base, kl, sizeof base - 1);
    }
    vnode_t *d = vfs_lookup_at(t->cwd, dir);
    if (!d || d->type != VN_DIR) return -1;
    if (!perm_ok(d, PERM_W)) return -1;
    return vfs_symlink(d, base, kt) ? 0 : -1;
}

/* readlink(path, buf, size): copy the symlink target into buf (NUL-terminated,
 * truncated to size-1).  Returns the target length or -1. */
static i64 sys_readlink(const char *path, char *buf, u64 size) {
    char kp[VFS_MAX_PATH];
    if (!copy_user_str((u64)path, kp, sizeof kp)) return -1;
    if (!uptr((u64)buf, size)) return -1;
    task_t *t = cur();
    vnode_t *v = vfs_lookup_at_nofollow(t->cwd, kp);
    if (!v || v->type != VN_SYMLINK) return -1;
    u64 n = v->size;
    if (n >= size) n = size ? size - 1 : 0;
    stac();
    memcpy(buf, v->data, n);
    if (size) buf[n] = 0;
    clac();
    return (i64)v->size;
}

static i64 sys_dup2(int oldfd, int newfd) {
    if (newfd < 0 || newfd >= MAX_FD || oldfd < 0 || oldfd >= MAX_FD) return -1;
    task_t *t = cur(); if (!t || !g_sys_from_user) return -1;
    fd_entry_t *of = fd_get(oldfd);
    if (!of) return -1;
    if (oldfd == newfd) return newfd;
    if (t->fds[newfd].in_use) {
        i64 rc = sys_close(newfd);          /* drop the target's refs */
        if (rc != 0) return -1;
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
    nf->in_use = true;
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
    vnode_t *v = vfs_lookup_at_nofollow(t->cwd, path);  /* act on the link */
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
    vnode_t *v = vfs_lookup_at_nofollow(t->cwd, kold);  /* rename the link */
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
static i64 sys_mmap_prot(u64 len, bool exec);   /* defined below */
static i64 sys_mmap(u64 len) {
    return sys_mmap_prot(len, false);
}

/* mmap with a protection hint: exec=true maps the pages executable (needed
 * by the dynamic linker to load shared libraries' code).  exec=false is the
 * classic anonymous data mapping. */
static i64 sys_mmap_prot(u64 len, bool exec) {
    if (!g_sys_from_user) return -1;
    task_t *t = cur();
    if (!t) return -1;
    if (len == 0 || len > USER_MMAP_END - USER_MMAP_BASE) return -1;
    len = PAGE_ALIGN_UP(len);
    u64 pages = len / PAGE_SIZE;
    u64 flags = PTE_RW | PTE_US | (exec ? 0 : PTE_NX);

    for (int wrap = 0; wrap < 2; wrap++) {
        u64 cand = PAGE_ALIGN_UP(t->mmap_next);
        if (cand < USER_MMAP_BASE) cand = USER_MMAP_BASE;
        /* find the first free spot quietly (O(regions)), then reserve it */
        u64 found = vmm_user_find_free(cand, pages, USER_MMAP_END);
        if (found && vmm_user_reserve(found, pages, flags,
                                      VMM_USER_LAZY) == 0) {
            /* GUARD PAGE: advance past one extra unmapped page so the
             * next region starts a page later - a 1-page overflow past
             * this region faults instead of silently touching the next
             * allocation (or unmapped memory) */
            t->mmap_next = found + len + PAGE_SIZE;
            return (i64)found;
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
static spinlock_t g_tcp_sys_lock;
static u8 g_k_tcp_tx[1400];
static u8 g_k_tcp_rx[4096];

static i64 sys_tcp_send(i64 conn, const u8 *buf, u64 len) {
    if (!g_sys_from_user) return -1;
    if (len > 1400) return -1;
    if (!uptr((u64)buf, len)) return -1;
    u64 fl = irq_save();
    spin_lock(&g_tcp_sys_lock);
    stac(); memcpy(g_k_tcp_tx, buf, len); clac();
    int r = net_tcp_send((int)conn, g_k_tcp_tx, (int)len);
    spin_unlock(&g_tcp_sys_lock);
    irq_restore(fl);
    net_service();
    return (i64)r;
}
static i64 sys_tcp_recv(i64 conn, u8 *buf, u64 cap) {
    if (!g_sys_from_user) return -1;
    if (cap > 4096) cap = 4096;
    if (!uptr((u64)buf, cap)) return -1;
    net_service();
    u64 fl = irq_save();
    spin_lock(&g_tcp_sys_lock);
    int n = net_tcp_recv((int)conn, g_k_tcp_rx, (int)cap);
    if (n > 0) { stac(); memcpy(buf, g_k_tcp_rx, (size_t)n); clac(); }
    spin_unlock(&g_tcp_sys_lock);
    irq_restore(fl);
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

/* ---------------------------------------------------------------------------
 * Linux x86_64 ABI layer.
 *
 * Full Linux x86_64 ABI emulation for running unmodified Linux ELF binaries
 * (static musl/glibc binaries, dynamic executables via musl ld.so, pthreads,
 * terminal utilities, epoll/eventfd, etc.).
 * ------------------------------------------------------------------------- */

#define LINUX_AT_FDCWD      (-100)
#define LINUX_ENOENT        2
#define LINUX_ESRCH         3
#define LINUX_EINTR         4
#define LINUX_EIO           5
#define LINUX_EBADF         9
#define LINUX_EAGAIN        11
#define LINUX_ENOMEM        12
#define LINUX_EACCES        13
#define LINUX_EFAULT        14
#define LINUX_EEXIST        17
#define LINUX_ENODEV        19
#define LINUX_ENOTDIR       20
#define LINUX_EISDIR        21
#define LINUX_EINVAL        22
#define LINUX_EMFILE        24
#define LINUX_ENOTTY        25
#define LINUX_ENOSPC        28
#define LINUX_ENOSYS        38
#define LINUX_ENOTSOCK      88
#define LINUX_ECONNREFUSED  111

/* Linux x86_64 struct stat (glibc/musl layout, 144 bytes). */
typedef struct PACKED {
    u64 st_dev, st_ino, st_nlink;
    u32 st_mode, st_uid, st_gid, __pad0;
    u64 st_rdev, st_size, st_blksize, st_blocks;
    i64 st_atim_sec,  st_atim_nsec;
    i64 st_mtim_sec,  st_mtim_nsec;
    i64 st_ctim_sec,  st_ctim_nsec;
    i64 __unused[3];
} linux_stat_t;

#define S_IFMT   0xF000
#define S_IFREG  0x8000
#define S_IFDIR  0x4000
#define S_IFLNK  0xA000
#define S_IFCHR  0x2000
#define DT_UNKNOWN 0
#define DT_DIR   4
#define DT_REG   8
#define DT_LNK   10
#define DT_CHR   2

/* Linux struct utsname (6 x 65 bytes = 390 bytes). */
typedef struct { char s[390]; } linux_utsname_t;

struct linux_termios {
    u32 c_iflag;
    u32 c_oflag;
    u32 c_cflag;
    u32 c_lflag;
    u8  c_line;
    u8  c_cc[32];
    u32 c_ispeed;
    u32 c_ospeed;
};

struct linux_winsize {
    u16 ws_row;
    u16 ws_col;
    u16 ws_xpixel;
    u16 ws_ypixel;
};

struct linux_pollfd {
    int   fd;
    short events;
    short revents;
};

struct PACKED linux_epoll_event {
    u32 events;
    u64 data;
};

struct linux_iovec {
    u64 iov_base;
    u64 iov_len;
};

struct linux_msghdr {
    u64 msg_name;
    u32 msg_namelen;
    u32 __pad1;
    u64 msg_iov;
    u64 msg_iovlen;
    u64 msg_control;
    u64 msg_controllen;
    u32 msg_flags;
    u32 __pad2;
};

struct linux_sigaction {
    u64 sa_handler;
    u64 sa_flags;
    u64 sa_restorer;
    u64 sa_mask;
};

struct linux_statfs {
    i64 f_type;
    i64 f_bsize;
    i64 f_blocks;
    i64 f_bfree;
    i64 f_bavail;
    i64 f_files;
    i64 f_ffree;
    i64 f_fsid[2];
    i64 f_namelen;
    i64 f_frsize;
    i64 f_flags;
    i64 f_spare[4];
};

struct linux_statx {
    u32 stx_mask;
    u32 stx_blksize;
    u64 stx_attributes;
    u32 stx_nlink;
    u32 stx_uid;
    u32 stx_gid;
    u16 stx_mode;
    u16 __spare0[1];
    u64 stx_ino;
    u64 stx_size;
    u64 stx_blocks;
    u64 stx_attributes_mask;
    struct { i64 tv_sec; u32 tv_nsec; i32 __pad; } stx_atime, stx_btime, stx_ctime, stx_mtime;
    u32 stx_rdev_major;
    u32 stx_rdev_minor;
    u32 stx_dev_major;
    u32 stx_dev_minor;
    u64 __spare2[14];
};

struct linux_tms {
    long tms_utime;
    long tms_stime;
    long tms_cutime;
    long tms_cstime;
};

struct linux_timespec { long tv_sec; long tv_nsec; };

struct linux_sysinfo {
    long uptime, loads[3], totalram, freeram, sharedram, bufferram,
         totalswap, freeswap;
    unsigned short procs, pad;
    long totalhigh, freehigh;
    unsigned mem_unit;
};

#define POLLIN     0x0001
#define POLLPRI    0x0002
#define POLLOUT    0x0004
#define POLLERR    0x0008
#define POLLHUP    0x0010
#define POLLNVAL   0x0020

#define LINUX_TCGETS     0x5401
#define LINUX_TCSETS     0x5402
#define LINUX_TCSETSW    0x5403
#define LINUX_TCSETSF    0x5404
#define LINUX_TIOCGWINSZ 0x5413
#define LINUX_TIOCSWINSZ 0x5414
#define LINUX_TIOCGPGRP  0x540F
#define LINUX_TIOCSPGRP  0x5410
#define LINUX_FIONREAD   0x541B
#define LINUX_FIONBIO    0x5421
#define LINUX_TIOCGPTN   0x80045430
#define LINUX_TIOCSPTLCK 0x40045431

#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3
#define EPOLL_MAX_ITEMS 64
#define EPOLL_INST_MAX  16

typedef struct {
    int  fd;
    u32  events;
    u64  data;
} epoll_item_t;

typedef struct {
    bool in_use;
    int  count;
    epoll_item_t items[EPOLL_MAX_ITEMS];
} epoll_inst_t;

static epoll_inst_t g_epolls[EPOLL_INST_MAX];
static spinlock_t   g_epoll_lock;

#define LINUX_SOCK_MAX    64
#define LINUX_SOCK_FD    200
#define LINUX_SOCK_STREAM 1
#define LINUX_SOCK_DGRAM  2
#define LINUX_AF_INET     2

typedef struct {
    int  in_use;
    int  type;          /* STREAM(conn) / LISTENER / DGRAM */
    int  id;            /* conn id (STREAM) or listener id */
    u32  peer_ip;       /* DGRAM default peer */
    u16  peer_port;
    u16  bound_port;
} linux_sock_t;
static linux_sock_t g_lsock[LINUX_SOCK_MAX];
static spinlock_t    g_lsock_lock;

static u16 ntohs16(u16 v) { return (u16)((v >> 8) | (v << 8)); }

static int linux_sock_slot(int fd) {
    int slot = fd - LINUX_SOCK_FD;
    if (slot < 0 || slot >= LINUX_SOCK_MAX) return -1;
    if (!g_lsock[slot].in_use) return -1;
    return slot;
}

static u64 linux_epoch_seconds(void) {
    rtc_time_t t; rtc_read(&t);
    static const int mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    long days = (long)(t.year - 1970) * 365;
    days += (t.year - 1969) / 4 - (t.year - 1901) / 100 + (t.year - 1601) / 400;
    for (int m = 1; m < (int)t.month; m++) days += mdays[m - 1];
    int leap = ((t.year % 4 == 0 && t.year % 100 != 0) || t.year % 400 == 0);
    if (t.month > 2 && leap) days += 1;
    days += t.day - 1;
    return (u64)days * 86400ULL + t.hour * 3600ULL + t.minute * 60ULL + t.second;
}

/* fill a Linux struct stat from a vnode (writes into validated user memory) */
static void linux_fill_stat(vnode_t *v, void *ubuf) {
    linux_stat_t st;
    memset(&st, 0, sizeof st);
    st.st_ino   = v->ino ? v->ino : (u64)(uintptr_t)v;
    st.st_nlink = 1;
    u32 type = (v->type == VN_DIR) ? S_IFDIR : (v->type == VN_SYMLINK ? S_IFLNK : S_IFREG);
    st.st_mode  = type | (v->mode & 07777u);
    st.st_uid   = v->uid;
    st.st_gid   = v->gid;
    st.st_size  = v->size;
    st.st_blksize = 4096;
    st.st_blocks  = (v->size + 511) / 512;
    u64 mt = v->mtime;
    st.st_atim_sec = st.st_mtim_sec = st.st_ctim_sec = (i64)mt;
    stac(); memcpy(ubuf, &st, sizeof st); clac();
}

/* Linux getdents64: fill dirent64 records from a dir fd's children */
static i64 linux_getdents64(int fd, char *ubuf, u64 count) {
    if (!uptr((u64)ubuf, count)) return -LINUX_EINVAL;
    fd_entry_t *f = fd_get(fd);
    if (!f || !f->vn || f->vn->type != VN_DIR) return -LINUX_EBADF;
    u64 off = 0;
    for (vnode_t *c = f->vn->child; c; c = c->sibling) {
        u64 namelen = strlen(c->name);
        u64 reclen = (19 + namelen + 1 + 7) & ~7ULL;   /* align8 */
        if (off + reclen > count) break;
        u8 d_type = (c->type == VN_DIR) ? DT_DIR
                  : (c->type == VN_SYMLINK) ? DT_LNK : DT_REG;
        stac();
        u64 *ino = (u64 *)(ubuf + off);       *ino = c->ino ? c->ino : (u64)(uintptr_t)c;
        i64 *doff = (i64 *)(ubuf + off + 8);  *doff = (i64)(off + reclen);
        u16 *rl = (u16 *)(ubuf + off + 16);   *rl = (u16)reclen;
        ubuf[off + 18] = d_type;
        memcpy(ubuf + off + 19, c->name, namelen + 1);
        clac();
        off += reclen;
    }
    return (i64)off;
}

/* Linux writev: gather-write a vector of iovec {base, len} pairs */
static i64 linux_writev(int fd, const void *iov_ubuf, u64 iovcnt) {
    if (iovcnt > 64 || !uptr((u64)iov_ubuf, iovcnt * 16)) return -LINUX_EINVAL;
    i64 total = 0;
    for (u64 i = 0; i < iovcnt; i++) {
        u64 base, len;
        stac();
        base = *(u64 *)((u8 *)iov_ubuf + i * 16);
        len  = *(u64 *)((u8 *)iov_ubuf + i * 16 + 8);
        clac();
        i64 w = sys_write(fd, (const char *)base, len);
        if (w < 0) return w;
        total += w;
        if ((u64)w < len) break;
    }
    return total;
}

static i64 linux_readv(int fd, const void *iov_ubuf, u64 iovcnt) {
    if (iovcnt > 64 || !uptr((u64)iov_ubuf, iovcnt * 16)) return -LINUX_EINVAL;
    i64 total = 0;
    for (u64 i = 0; i < iovcnt; i++) {
        u64 base, len;
        stac();
        base = *(u64 *)((u8 *)iov_ubuf + i * 16);
        len  = *(u64 *)((u8 *)iov_ubuf + i * 16 + 8);
        clac();
        i64 r = sys_read(fd, (char *)base, len);
        if (r < 0) return r;
        total += r;
        if ((u64)r < len) break;
    }
    return total;
}

/* Linux mmap with MAP_FIXED and file-backed mapping support */
static i64 linux_mmap(u64 addr, u64 len, u64 prot, u64 flags, u64 fd, u64 off) {
    if (len == 0) return -LINUX_EINVAL;
    task_t *t = cur();
    if (!t) return -LINUX_EINVAL;
    len = PAGE_ALIGN_UP(len);
    u64 pages = len / PAGE_SIZE;
    u64 pte_flags = PTE_US;
    if (prot & 2 /* PROT_WRITE */) pte_flags |= PTE_RW;
    if (!(prot & 4 /* PROT_EXEC */)) pte_flags |= PTE_NX;

    u64 va = 0;
    if (flags & 0x10 /* MAP_FIXED */) {
        if (!addr || (addr & (PAGE_SIZE - 1)) != 0) return -LINUX_EINVAL;
        va = addr;
        if (vmm_user_reserve(va, pages, pte_flags, VMM_USER_LAZY) != 0) {
            for (u64 i = 0; i < pages; i++) {
                paddr_t p = pmm_alloc_page();
                if (p) vmm_map_in(t->pml4, va + i * PAGE_SIZE, p, pte_flags | PTE_RW);
            }
        }
    } else {
        va = (u64)sys_mmap_prot(len, (prot & 4) != 0);
        if ((i64)va < 0) return -LINUX_ENOMEM;
    }

    /* File-backed mapping */
    if (!(flags & 0x20 /* MAP_ANONYMOUS */) && (i64)fd >= 0) {
        fd_entry_t *f = fd_get((int)fd);
        if (f && f->vn) {
            stac();
            vfs_read(f->vn, (void *)va, (size_t)off, (size_t)len);
            clac();
        }
    }

    return (i64)va;
}

static i64 linux_mremap(u64 old_addr, u64 old_size, u64 new_size, u64 flags, u64 new_addr) {
    (void)flags; (void)new_addr;
    if (new_size <= old_size) return (i64)old_addr;
    return linux_mmap(0, new_size, 3 /* RW */, 0x22 /* MAP_PRIVATE|MAP_ANONYMOUS */, (u64)-1, 0);
}

static i64 linux_pread64(int fd, u64 buf_ptr, u64 count, i64 offset) {
    if (!uptr(buf_ptr, count)) return -LINUX_EINVAL;
    fd_entry_t *f = fd_get(fd);
    if (!f || !f->vn) return -LINUX_EBADF;
    stac(); int r = vfs_read(f->vn, (void *)buf_ptr, (size_t)offset, (size_t)count); clac();
    return r >= 0 ? r : -LINUX_EIO;
}

static i64 linux_pwrite64(int fd, u64 buf_ptr, u64 count, i64 offset) {
    if (!uptr(buf_ptr, count)) return -LINUX_EINVAL;
    fd_entry_t *f = fd_get(fd);
    if (!f || !f->vn) return -LINUX_EBADF;
    stac(); int w = vfs_write(f->vn, (const void *)buf_ptr, (size_t)offset, (size_t)count); clac();
    return w >= 0 ? w : -LINUX_EIO;
}

static i64 linux_access(const char *path, u64 mode) {
    char kpath[VFS_MAX_PATH];
    if (!copy_user_str((u64)path, kpath, sizeof kpath)) return -LINUX_EINVAL;
    vnode_t *v = vfs_lookup_at(cur() ? cur()->cwd : NULL, kpath);
    if (!v) return -LINUX_ENOENT;
    if (mode & 2 && !perm_ok(v, PERM_W)) return -LINUX_EACCES;
    if (mode & 4 && !perm_ok(v, PERM_R)) return -LINUX_EACCES;
    return 0;
}

static short linux_check_fd_events(int fd, short events) {
    short revents = 0;
    if (fd < 0 || fd >= MAX_FD) return POLLNVAL;
    fd_entry_t *f = fd_get(fd);
    if (!f) {
        if (fd <= 2) {
            if (events & POLLIN)  revents |= POLLIN;
            if (events & POLLOUT) revents |= POLLOUT;
            return revents;
        }
        return POLLNVAL;
    }
    if (f->type == FD_TYPE_DEV_NULL || f->type == FD_TYPE_DEV_ZERO || f->type == FD_TYPE_DEV_RANDOM || f->type == FD_TYPE_DEV_TTY) {
        if (events & POLLIN)  revents |= POLLIN;
        if (events & POLLOUT) revents |= POLLOUT;
        return revents;
    }
    if (f->type == FD_TYPE_PTY_MASTER) {
        pty_state_t *p = &g_ptys[f->pty_id];
        if ((events & POLLIN) && p->out_count > 0) revents |= POLLIN;
        if ((events & POLLOUT) && p->in_count < PTY_BUF_SZ) revents |= POLLOUT;
        return revents;
    }
    if (f->type == FD_TYPE_PTY_SLAVE) {
        pty_state_t *p = &g_ptys[f->pty_id];
        if ((events & POLLIN) && p->in_count > 0) revents |= POLLIN;
        if ((events & POLLOUT) && p->out_count < PTY_BUF_SZ) revents |= POLLOUT;
        return revents;
    }
    if (f->is_pipe || f->type == FD_TYPE_PIPE) {
        yart_pipe_t *p = f->pipe;
        if (p) {
            if (f->pipe_is_read_end) {
                if ((events & POLLIN) && p->count > 0) revents |= POLLIN;
                if (p->write_ends == 0) revents |= (POLLHUP | POLLIN);
            } else {
                if ((events & POLLOUT) && (PIPE_BUF_SIZE - p->count > 0)) revents |= POLLOUT;
                if (p->read_ends == 0) revents |= POLLERR;
            }
        }
        return revents;
    }
    if (f->type == FD_TYPE_EVENTFD) {
        if ((events & POLLIN) && f->eventfd_val > 0) revents |= POLLIN;
        if (events & POLLOUT) revents |= POLLOUT;
        return revents;
    }
    if (f->vn) {
        if (events & POLLIN)  revents |= POLLIN;
        if (events & POLLOUT) revents |= POLLOUT;
        return revents;
    }
    return revents;
}

static i64 linux_poll(u64 fds_ptr, u64 nfds, i64 timeout_ms) {
    if (nfds > 256 || (nfds > 0 && !uptr(fds_ptr, nfds * sizeof(struct linux_pollfd))))
        return -LINUX_EINVAL;
    struct linux_pollfd fds[32];
    u64 count = nfds > 32 ? 32 : nfds;
    stac(); memcpy(fds, (void *)fds_ptr, count * sizeof(struct linux_pollfd)); clac();

    u64 t0 = pit_ticks();
    u64 limit = timeout_ms > 0 ? (t0 + (u64)((timeout_ms * TICK_HZ) / 1000)) : 0;

    for (;;) {
        int ready = 0;
        for (u64 i = 0; i < count; i++) {
            fds[i].revents = linux_check_fd_events(fds[i].fd, fds[i].events);
            if (fds[i].revents) ready++;
        }
        if (ready > 0 || timeout_ms == 0) {
            stac(); memcpy((void *)fds_ptr, fds, count * sizeof(struct linux_pollfd)); clac();
            return ready;
        }
        if (timeout_ms > 0 && pit_ticks() >= limit) {
            stac(); memcpy((void *)fds_ptr, fds, count * sizeof(struct linux_pollfd)); clac();
            return 0;
        }
        sched_sleep_ms(5);
    }
}

static i64 linux_ppoll(u64 fds_ptr, u64 nfds, u64 tsp_ptr, u64 sigmask_ptr) {
    (void)sigmask_ptr;
    i64 timeout_ms = -1;
    if (tsp_ptr && uptr(tsp_ptr, sizeof(struct linux_timespec))) {
        struct linux_timespec ts;
        stac(); memcpy(&ts, (void *)tsp_ptr, sizeof ts); clac();
        timeout_ms = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    }
    return linux_poll(fds_ptr, nfds, timeout_ms);
}

static i64 linux_select(u64 nfds, u64 rfds, u64 wfds, u64 efds, u64 tv_ptr) {
    if (nfds > 64) nfds = 64;
    u64 rbits = 0, wbits = 0, ebits = 0;
    stac();
    if (rfds && uptr(rfds, 8)) rbits = *(u64 *)(void *)rfds;
    if (wfds && uptr(wfds, 8)) wbits = *(u64 *)(void *)wfds;
    if (efds && uptr(efds, 8)) ebits = *(u64 *)(void *)efds;
    clac();
    (void)ebits;

    i64 timeout_ms = -1;
    if (tv_ptr && uptr(tv_ptr, 16)) {
        i64 sec, usec;
        stac(); sec = *(i64 *)(void *)tv_ptr; usec = *(i64 *)(void *)(tv_ptr + 8); clac();
        timeout_ms = sec * 1000 + usec / 1000;
    }

    u64 t0 = pit_ticks();
    u64 limit = timeout_ms > 0 ? (t0 + (u64)((timeout_ms * TICK_HZ) / 1000)) : 0;

    for (;;) {
        u64 rout = 0, wout = 0, eout = 0;
        int ready = 0;
        for (u64 i = 0; i < nfds; i++) {
            if (rbits & (1ULL << i)) {
                short rev = linux_check_fd_events((int)i, POLLIN);
                if (rev & (POLLIN | POLLHUP | POLLERR)) { rout |= (1ULL << i); ready++; }
            }
            if (wbits & (1ULL << i)) {
                short rev = linux_check_fd_events((int)i, POLLOUT);
                if (rev & POLLOUT) { wout |= (1ULL << i); ready++; }
            }
        }
        if (ready > 0 || timeout_ms == 0) {
            stac();
            if (rfds && uptr(rfds, 8)) *(u64 *)(void *)rfds = rout;
            if (wfds && uptr(wfds, 8)) *(u64 *)(void *)wfds = wout;
            if (efds && uptr(efds, 8)) *(u64 *)(void *)efds = eout;
            clac();
            return ready;
        }
        if (timeout_ms > 0 && pit_ticks() >= limit) {
            stac();
            if (rfds && uptr(rfds, 8)) *(u64 *)(void *)rfds = 0;
            if (wfds && uptr(wfds, 8)) *(u64 *)(void *)wfds = 0;
            if (efds && uptr(efds, 8)) *(u64 *)(void *)efds = 0;
            clac();
            return 0;
        }
        sched_sleep_ms(5);
    }
}

static i64 linux_pselect6(u64 nfds, u64 rfds, u64 wfds, u64 efds, u64 tsp_ptr, u64 sigmask_ptr) {
    (void)sigmask_ptr;
    return linux_select(nfds, rfds, wfds, efds, tsp_ptr);
}

static i64 linux_epoll_create1(u64 flags) {
    (void)flags;
    int fd = alloc_fd_from(3);
    if (fd < 0) return -LINUX_EMFILE;
    u64 fl = irq_save(); spin_lock(&g_epoll_lock);
    int inst = -1;
    for (int i = 0; i < EPOLL_INST_MAX; i++) {
        if (!g_epolls[i].in_use) {
            inst = i;
            memset(&g_epolls[i], 0, sizeof(epoll_inst_t));
            g_epolls[i].in_use = true;
            break;
        }
    }
    spin_unlock(&g_epoll_lock); irq_restore(fl);
    if (inst < 0) return -LINUX_ENOMEM;
    fd_entry_t *f = &cur()->fds[fd];
    memset(f, 0, sizeof *f);
    f->in_use = true; f->type = FD_TYPE_EPOLL; f->epoll_data = &g_epolls[inst];
    return fd;
}

static i64 linux_epoll_ctl(int epfd, int op, int fd, u64 event_ptr) {
    fd_entry_t *ef = fd_get(epfd);
    if (!ef || ef->type != FD_TYPE_EPOLL || !ef->epoll_data) return -LINUX_EBADF;
    epoll_inst_t *inst = (epoll_inst_t *)ef->epoll_data;
    struct linux_epoll_event ev; memset(&ev, 0, sizeof ev);
    if (op != EPOLL_CTL_DEL) {
        if (!uptr(event_ptr, sizeof(struct linux_epoll_event))) return -LINUX_EINVAL;
        stac(); memcpy(&ev, (void *)event_ptr, sizeof ev); clac();
    }
    u64 fl = irq_save(); spin_lock(&g_epoll_lock);
    if (op == EPOLL_CTL_ADD) {
        if (inst->count >= EPOLL_MAX_ITEMS) { spin_unlock(&g_epoll_lock); irq_restore(fl); return -LINUX_ENOSPC; }
        inst->items[inst->count].fd = fd;
        inst->items[inst->count].events = ev.events;
        inst->items[inst->count].data = ev.data;
        inst->count++;
    } else if (op == EPOLL_CTL_MOD) {
        for (int i = 0; i < inst->count; i++) {
            if (inst->items[i].fd == fd) {
                inst->items[i].events = ev.events;
                inst->items[i].data = ev.data;
                break;
            }
        }
    } else if (op == EPOLL_CTL_DEL) {
        for (int i = 0; i < inst->count; i++) {
            if (inst->items[i].fd == fd) {
                inst->items[i] = inst->items[inst->count - 1];
                inst->count--;
                break;
            }
        }
    }
    spin_unlock(&g_epoll_lock); irq_restore(fl);
    return 0;
}

static i64 linux_epoll_wait(int epfd, u64 events_ptr, int maxevents, int timeout_ms) {
    if (maxevents <= 0 || maxevents > 64 || !uptr(events_ptr, (u64)maxevents * sizeof(struct linux_epoll_event)))
        return -LINUX_EINVAL;
    fd_entry_t *ef = fd_get(epfd);
    if (!ef || ef->type != FD_TYPE_EPOLL || !ef->epoll_data) return -LINUX_EBADF;
    epoll_inst_t *inst = (epoll_inst_t *)ef->epoll_data;

    struct linux_epoll_event out_evs[32];
    u64 t0 = pit_ticks();
    u64 limit = timeout_ms > 0 ? (t0 + (u64)((timeout_ms * TICK_HZ) / 1000)) : 0;

    for (;;) {
        int ready = 0;
        u64 fl = irq_save(); spin_lock(&g_epoll_lock);
        for (int i = 0; i < inst->count && ready < maxevents && ready < 32; i++) {
            short rev = linux_check_fd_events(inst->items[i].fd, (short)inst->items[i].events);
            if (rev) {
                out_evs[ready].events = (u32)rev;
                out_evs[ready].data = inst->items[i].data;
                ready++;
            }
        }
        spin_unlock(&g_epoll_lock); irq_restore(fl);
        if (ready > 0 || timeout_ms == 0) {
            stac(); memcpy((void *)events_ptr, out_evs, (size_t)ready * sizeof(struct linux_epoll_event)); clac();
            return ready;
        }
        if (timeout_ms > 0 && pit_ticks() >= limit) return 0;
        sched_sleep_ms(5);
    }
}

static i64 linux_eventfd(u64 initval, u64 flags) {
    int fd = alloc_fd_from(3);
    if (fd < 0) return -LINUX_EMFILE;
    fd_entry_t *f = &cur()->fds[fd];
    memset(f, 0, sizeof *f);
    f->in_use = true; f->type = FD_TYPE_EVENTFD; f->eventfd_val = initval; f->flags = (u32)flags;
    return fd;
}

static i64 linux_ioctl(int fd, u64 cmd, u64 arg) {
    fd_entry_t *f = fd_get(fd);
    task_t *t = cur();
    if (!t) return -LINUX_EBADF;

    if (f && (f->type == FD_TYPE_PTY_MASTER || f->type == FD_TYPE_PTY_SLAVE)) {
        pty_state_t *p = &g_ptys[f->pty_id];
        if (cmd == LINUX_TIOCGPTN) {
            if (!uptr(arg, 4)) return -LINUX_EINVAL;
            stac(); *(u32 *)(void *)arg = f->pty_id; clac();
            return 0;
        }
        if (cmd == LINUX_TIOCSPTLCK) {
            if (!uptr(arg, 4)) return -LINUX_EINVAL;
            u32 lk; stac(); lk = *(u32 *)(void *)arg; clac();
            p->locked = (lk != 0);
            return 0;
        }
        if (cmd == LINUX_TCGETS) {
            if (!uptr(arg, sizeof(struct linux_termios))) return -LINUX_EINVAL;
            struct linux_termios lt; memset(&lt, 0, sizeof lt);
            lt.c_iflag = p->c_iflag; lt.c_oflag = p->c_oflag;
            lt.c_cflag = p->c_cflag; lt.c_lflag = p->c_lflag;
            memcpy(lt.c_cc, p->c_cc, sizeof p->c_cc);
            stac(); memcpy((void *)arg, &lt, sizeof lt); clac();
            return 0;
        }
        if (cmd == LINUX_TCSETS || cmd == LINUX_TCSETSW || cmd == LINUX_TCSETSF) {
            if (!uptr(arg, sizeof(struct linux_termios))) return -LINUX_EINVAL;
            struct linux_termios lt; stac(); memcpy(&lt, (void *)arg, sizeof lt); clac();
            p->c_iflag = lt.c_iflag; p->c_oflag = lt.c_oflag;
            p->c_cflag = lt.c_cflag; p->c_lflag = lt.c_lflag;
            memcpy(p->c_cc, lt.c_cc, sizeof p->c_cc);
            return 0;
        }
        if (cmd == LINUX_TIOCGWINSZ) {
            if (!uptr(arg, sizeof(struct linux_winsize))) return -LINUX_EINVAL;
            struct linux_winsize ws; memset(&ws, 0, sizeof ws);
            ws.ws_row = p->ws_row ? p->ws_row : 25;
            ws.ws_col = p->ws_col ? p->ws_col : 80;
            ws.ws_xpixel = p->ws_xpixel ? p->ws_xpixel : 640;
            ws.ws_ypixel = p->ws_ypixel ? p->ws_ypixel : 400;
            stac(); memcpy((void *)arg, &ws, sizeof ws); clac();
            return 0;
        }
        if (cmd == LINUX_TIOCSWINSZ) {
            if (!uptr(arg, sizeof(struct linux_winsize))) return -LINUX_EINVAL;
            struct linux_winsize ws; stac(); memcpy(&ws, (void *)arg, sizeof ws); clac();
            p->ws_row = ws.ws_row; p->ws_col = ws.ws_col;
            p->ws_xpixel = ws.ws_xpixel; p->ws_ypixel = ws.ws_ypixel;
            if (p->fg_pgrp) sched_signal(p->fg_pgrp, 28 /* SIGWINCH */);
            return 0;
        }
        if (cmd == LINUX_TIOCGPGRP) {
            if (!uptr(arg, 4)) return -LINUX_EINVAL;
            stac(); *(u32 *)(void *)arg = p->fg_pgrp ? p->fg_pgrp : t->pid; clac();
            return 0;
        }
        if (cmd == LINUX_TIOCSPGRP) {
            if (!uptr(arg, 4)) return -LINUX_EINVAL;
            stac(); p->fg_pgrp = *(u32 *)(void *)arg; clac();
            return 0;
        }
    }

    if (cmd == LINUX_TCGETS) {
        if (!uptr(arg, sizeof(struct linux_termios))) return -LINUX_EINVAL;
        struct linux_termios lt; memset(&lt, 0, sizeof lt);
        lt.c_iflag = 0x0500; lt.c_oflag = 0x0005; lt.c_cflag = 0x00bf; lt.c_lflag = 0x8a3b;
        lt.c_cc[0] = 3; lt.c_cc[1] = 28; lt.c_cc[2] = 127; lt.c_cc[3] = 21; lt.c_cc[4] = 4;
        lt.c_cc[10] = 26;
        stac(); memcpy((void *)arg, &lt, sizeof lt); clac();
        return 0;
    }
    if (cmd == LINUX_TCSETS || cmd == LINUX_TCSETSW || cmd == LINUX_TCSETSF) {
        return 0;
    }
    if (cmd == LINUX_TIOCGWINSZ) {
        if (!uptr(arg, sizeof(struct linux_winsize))) return -LINUX_EINVAL;
        struct linux_winsize ws; memset(&ws, 0, sizeof ws);
        ws.ws_row = 25; ws.ws_col = 80; ws.ws_xpixel = 640; ws.ws_ypixel = 400;
        stac(); memcpy((void *)arg, &ws, sizeof ws); clac();
        return 0;
    }
    if (cmd == LINUX_TIOCSWINSZ) return 0;
    if (cmd == LINUX_TIOCGPGRP) {
        if (!uptr(arg, 4)) return -LINUX_EINVAL;
        stac(); *(u32 *)(void *)arg = t->pgid ? t->pgid : t->pid; clac();
        return 0;
    }
    if (cmd == LINUX_TIOCSPGRP) {
        if (!uptr(arg, 4)) return -LINUX_EINVAL;
        stac(); t->pgid = *(u32 *)(void *)arg; clac();
        return 0;
    }
    if (cmd == LINUX_FIONREAD) {
        if (!uptr(arg, 4)) return -LINUX_EINVAL;
        u32 cnt = 0;
        if (f && f->is_pipe && f->pipe) cnt = f->pipe->count;
        stac(); *(u32 *)(void *)arg = cnt; clac();
        return 0;
    }
    if (cmd == LINUX_FIONBIO) {
        if (!uptr(arg, 4)) return -LINUX_EINVAL;
        u32 nb; stac(); nb = *(u32 *)(void *)arg; clac();
        if (f) {
            if (nb) f->flags |= 0x800 /* O_NONBLOCK */;
            else    f->flags &= ~0x800;
        }
        return 0;
    }

    return 0;
}

static vnode_t *linux_resolve_dirfd(i64 dirfd) {
    if (dirfd == LINUX_AT_FDCWD) return cur() ? cur()->cwd : vfs_root();
    fd_entry_t *f = fd_get((int)dirfd);
    if (!f || !f->vn || f->vn->type != VN_DIR) return NULL;
    return f->vn;
}

static i64 linux_openat(i64 dirfd, u64 path_ptr, u64 flags, u64 mode) {
    char kpath[VFS_MAX_PATH];
    if (!copy_user_str(path_ptr, kpath, sizeof kpath)) return -LINUX_EINVAL;
    vnode_t *base = (kpath[0] == '/') ? vfs_root() : linux_resolve_dirfd(dirfd);
    if (!base) return -LINUX_EBADF;

    int fd = alloc_fd_from(0);
    if (fd < 0) return -LINUX_EMFILE;
    fd_entry_t *f = &cur()->fds[fd];
    memset(f, 0, sizeof *f);

    if (strcmp(kpath, "/dev/null") == 0) { f->in_use = true; f->type = FD_TYPE_DEV_NULL; f->flags = (u32)flags; return fd; }
    if (strcmp(kpath, "/dev/zero") == 0) { f->in_use = true; f->type = FD_TYPE_DEV_ZERO; f->flags = (u32)flags; return fd; }
    if (strcmp(kpath, "/dev/urandom") == 0 || strcmp(kpath, "/dev/random") == 0) { f->in_use = true; f->type = FD_TYPE_DEV_RANDOM; f->flags = (u32)flags; return fd; }
    if (strcmp(kpath, "/dev/tty") == 0 || strcmp(kpath, "/dev/console") == 0) { f->in_use = true; f->type = FD_TYPE_DEV_TTY; f->flags = (u32)flags; return fd; }
    if (strcmp(kpath, "/dev/ptmx") == 0) {
        int p = pty_alloc();
        if (p < 0) return -LINUX_ENOMEM;
        f->in_use = true; f->type = FD_TYPE_PTY_MASTER; f->pty_id = (u32)p; f->flags = (u32)flags; return fd;
    }
    if (strncmp(kpath, "/dev/pts/", 9) == 0) {
        int p = pty_slave_open(kpath + 9);
        if (p < 0) return -LINUX_ENOENT;
        f->in_use = true; f->type = FD_TYPE_PTY_SLAVE; f->pty_id = (u32)p; f->flags = (u32)flags; return fd;
    }

    vnode_t *v = vfs_lookup_at(base, kpath);
    if (!v && (flags & 0x40 /* O_CREAT */)) {
        char dirbuf[VFS_MAX_PATH] = {0}, basebuf[VFS_MAX_NAME] = {0};
        const char *slash = NULL;
        for (const char *p = kpath; *p; p++) if (*p == '/') slash = p;
        if (slash) {
            size_t l = slash - kpath;
            if (l == 0) l = 1;
            if (l >= sizeof dirbuf) return -LINUX_EINVAL;
            memcpy(dirbuf, kpath, l);
            dirbuf[l] = 0;
            strncpy(basebuf, slash + 1, sizeof basebuf - 1);
        } else {
            strncpy(dirbuf, ".", sizeof dirbuf - 1);
            strncpy(basebuf, kpath, sizeof basebuf - 1);
        }
        vnode_t *d = vfs_lookup_at(base, dirbuf);
        if (!d) return -LINUX_ENOENT;
        if (!perm_ok(d, PERM_W)) return -LINUX_EACCES;
        v = vfs_create(d, basebuf, VN_FILE);
        if (v && mode) v->mode = (u16)(mode & 0777);
    }
    if (!v) return -LINUX_ENOENT;
    if (v->type == VN_FILE && (flags & 0x200 /* O_TRUNC */)) vfs_truncate(v, 0);

    vnode_ref(v);
    f->vn     = v;
    f->pos    = (flags & 0x400 /* O_APPEND */) ? v->size : 0;
    f->flags  = (u32)flags;
    f->type   = FD_TYPE_FILE;
    f->in_use = true;
    return fd;
}

static i64 linux_newfstatat(i64 dirfd, u64 path_ptr, u64 stat_ptr, u64 flags) {
    if (!uptr(stat_ptr, sizeof(linux_stat_t))) return -LINUX_EINVAL;
    char kpath[VFS_MAX_PATH];
    if (path_ptr) {
        if (!copy_user_str(path_ptr, kpath, sizeof kpath)) return -LINUX_EINVAL;
    } else {
        kpath[0] = 0;
    }
    vnode_t *v = NULL;
    if (kpath[0] == 0 || (flags & 0x1000 /* AT_EMPTY_PATH */)) {
        if (dirfd == LINUX_AT_FDCWD) v = cur() ? cur()->cwd : vfs_root();
        else {
            fd_entry_t *f = fd_get((int)dirfd);
            if (!f || !f->vn) return -LINUX_EBADF;
            v = f->vn;
        }
    } else {
        vnode_t *base = (kpath[0] == '/') ? vfs_root() : linux_resolve_dirfd(dirfd);
        if (!base) return -LINUX_EBADF;
        v = (flags & 0x100 /* AT_SYMLINK_NOFOLLOW */)
            ? vfs_lookup_at_nofollow(base, kpath)
            : vfs_lookup_at(base, kpath);
    }
    if (!v) return -LINUX_ENOENT;
    linux_fill_stat(v, (void *)stat_ptr);
    return 0;
}

static i64 linux_unlinkat(i64 dirfd, u64 path_ptr, u64 flags) {
    char kpath[VFS_MAX_PATH];
    if (!copy_user_str(path_ptr, kpath, sizeof kpath)) return -LINUX_EINVAL;
    vnode_t *base = (kpath[0] == '/') ? vfs_root() : linux_resolve_dirfd(dirfd);
    if (!base) return -LINUX_EBADF;
    vnode_t *v = vfs_lookup_at_nofollow(base, kpath);
    if (!v) return -LINUX_ENOENT;
    if ((flags & 0x200 /* AT_REMOVEDIR */) && v->type != VN_DIR) return -LINUX_ENOTDIR;
    return vfs_unlink(v) == 0 ? 0 : -LINUX_EACCES;
}

static i64 linux_renameat2(i64 olddirfd, u64 oldpath_ptr, i64 newdirfd, u64 newpath_ptr, u64 flags) {
    (void)flags;
    char oldp[VFS_MAX_PATH], newp[VFS_MAX_PATH];
    if (!copy_user_str(oldpath_ptr, oldp, sizeof oldp) || !copy_user_str(newpath_ptr, newp, sizeof newp))
        return -LINUX_EINVAL;
    vnode_t *oldbase = (oldp[0] == '/') ? vfs_root() : linux_resolve_dirfd(olddirfd);
    vnode_t *newbase = (newp[0] == '/') ? vfs_root() : linux_resolve_dirfd(newdirfd);
    if (!oldbase || !newbase) return -LINUX_EBADF;
    vnode_t *v = vfs_lookup_at_nofollow(oldbase, oldp);
    if (!v) return -LINUX_ENOENT;
    return sys_rename(oldp, newp);
}
static i64 linux_renameat(i64 olddirfd, u64 oldpath_ptr, i64 newdirfd, u64 newpath_ptr) {
    return linux_renameat2(olddirfd, oldpath_ptr, newdirfd, newpath_ptr, 0);
}

static i64 linux_mkdirat(i64 dirfd, u64 path_ptr, u64 mode) {
    (void)mode;
    char kpath[VFS_MAX_PATH];
    if (!copy_user_str(path_ptr, kpath, sizeof kpath)) return -LINUX_EINVAL;
    return sys_mkdir(kpath);
}

static i64 linux_mknodat(i64 dirfd, u64 path_ptr, u64 mode, u64 dev) {
    (void)dev;
    return linux_openat(dirfd, path_ptr, 0x40 /* O_CREAT */ | 0x2 /* O_RDWR */, mode);
}

static i64 linux_mknod(u64 path_ptr, u64 mode, u64 dev) {
    return linux_mknodat(LINUX_AT_FDCWD, path_ptr, mode, dev);
}

static i64 linux_faccessat(i64 dirfd, u64 path_ptr, u64 mode, u64 flags) {
    char kpath[VFS_MAX_PATH];
    if (!copy_user_str(path_ptr, kpath, sizeof kpath)) return -LINUX_EINVAL;
    vnode_t *base = (kpath[0] == '/') ? vfs_root() : linux_resolve_dirfd(dirfd);
    if (!base) return -LINUX_EBADF;
    vnode_t *v = (flags & 0x100 /* AT_SYMLINK_NOFOLLOW */)
        ? vfs_lookup_at_nofollow(base, kpath)
        : vfs_lookup_at(base, kpath);
    if (!v) return -LINUX_ENOENT;
    if (mode & 2 && !perm_ok(v, PERM_W)) return -LINUX_EACCES;
    if (mode & 4 && !perm_ok(v, PERM_R)) return -LINUX_EACCES;
    return 0;
}

static i64 linux_symlinkat(u64 target_ptr, i64 newdirfd, u64 linkpath_ptr) {
    char target[VFS_MAX_PATH], linkpath[VFS_MAX_PATH];
    if (!copy_user_str(target_ptr, target, sizeof target) || !copy_user_str(linkpath_ptr, linkpath, sizeof linkpath))
        return -LINUX_EINVAL;
    return sys_symlink(target, linkpath);
}

static i64 linux_readlinkat(i64 dirfd, u64 path_ptr, u64 buf_ptr, u64 bufsiz) {
    char kpath[VFS_MAX_PATH];
    if (!copy_user_str(path_ptr, kpath, sizeof kpath)) return -LINUX_EINVAL;
    return sys_readlink(kpath, (char *)buf_ptr, bufsiz);
}

static i64 linux_fchmodat(i64 dirfd, u64 path_ptr, u64 mode, u64 flags) {
    (void)flags;
    char kpath[VFS_MAX_PATH];
    if (!copy_user_str(path_ptr, kpath, sizeof kpath)) return -LINUX_EINVAL;
    return sys_chmod(kpath, (u16)mode);
}

static i64 linux_fchmod(int fd, u64 mode) {
    fd_entry_t *f = fd_get(fd);
    if (!f || !f->vn) return -LINUX_EBADF;
    f->vn->mode = (u16)(mode & 0777);
    return 0;
}

static i64 linux_utimensat(i64 dirfd, u64 path_ptr, u64 tsp_ptr, u64 flags) {
    (void)dirfd; (void)flags;
    if (!path_ptr) return 0;
    char kpath[VFS_MAX_PATH];
    if (!copy_user_str(path_ptr, kpath, sizeof kpath)) return -LINUX_EINVAL;
    vnode_t *v = vfs_lookup_at(cur() ? cur()->cwd : NULL, kpath);
    if (!v) return -LINUX_ENOENT;
    if (tsp_ptr && uptr(tsp_ptr, sizeof(struct linux_timespec) * 2)) {
        struct linux_timespec ts[2];
        stac(); memcpy(ts, (void *)tsp_ptr, sizeof ts); clac();
        v->mtime = (u64)ts[1].tv_sec;
    } else {
        v->mtime = linux_epoch_seconds();
    }
    return 0;
}

static i64 linux_futimesat(i64 dirfd, u64 path_ptr, u64 tvp_ptr) {
    return linux_utimensat(dirfd, path_ptr, tvp_ptr, 0);
}

static i64 linux_utimes(u64 path_ptr, u64 tvp_ptr) {
    return linux_utimensat(LINUX_AT_FDCWD, path_ptr, tvp_ptr, 0);
}

static i64 linux_utime(u64 path_ptr, u64 utimbuf_ptr) {
    return linux_utimensat(LINUX_AT_FDCWD, path_ptr, utimbuf_ptr, 0);
}

static i64 linux_statfs(u64 path_ptr, u64 buf_ptr) {
    if (!uptr(buf_ptr, sizeof(struct linux_statfs))) return -LINUX_EINVAL;
    struct linux_statfs s; memset(&s, 0, sizeof s);
    s.f_type = 0x01021994; /* TMPFS_MAGIC */
    s.f_bsize = 4096;
    s.f_blocks = 1048576;
    s.f_bfree = 800000;
    s.f_bavail = 800000;
    s.f_files = 65536;
    s.f_ffree = 60000;
    s.f_namelen = 255;
    s.f_frsize = 4096;
    stac(); memcpy((void *)buf_ptr, &s, sizeof s); clac();
    return 0;
}

static i64 linux_fstatfs(int fd, u64 buf_ptr) {
    (void)fd;
    return linux_statfs(0, buf_ptr);
}

static i64 linux_statx(i64 dirfd, u64 path_ptr, u64 flags, u64 mask, u64 buf_ptr) {
    (void)mask;
    if (!uptr(buf_ptr, sizeof(struct linux_statx))) return -LINUX_EINVAL;
    linux_stat_t st;
    i64 ret = linux_newfstatat(dirfd, path_ptr, (u64)(uintptr_t)&st, flags);
    if (ret != 0) return ret;
    struct linux_statx sx; memset(&sx, 0, sizeof sx);
    sx.stx_mask = 0x7ff;
    sx.stx_blksize = (u32)st.st_blksize;
    sx.stx_nlink = (u32)st.st_nlink;
    sx.stx_uid = st.st_uid;
    sx.stx_gid = st.st_gid;
    sx.stx_mode = (u16)st.st_mode;
    sx.stx_ino = st.st_ino;
    sx.stx_size = st.st_size;
    sx.stx_blocks = st.st_blocks;
    sx.stx_mtime.tv_sec = st.st_mtim_sec;
    sx.stx_atime.tv_sec = st.st_atim_sec;
    sx.stx_ctime.tv_sec = st.st_ctim_sec;
    stac(); memcpy((void *)buf_ptr, &sx, sizeof sx); clac();
    return 0;
}

static i64 linux_rt_sigaction(u64 sig, u64 act_ptr, u64 oact_ptr, u64 sigsetsize) {
    (void)sigsetsize;
    if (sig >= 32) return -LINUX_EINVAL;
    task_t *t = cur();
    if (!t) return -LINUX_EINVAL;
    if (oact_ptr) {
        if (!uptr(oact_ptr, sizeof(struct linux_sigaction))) return -LINUX_EINVAL;
        struct linux_sigaction osa; memset(&osa, 0, sizeof osa);
        osa.sa_handler = t->sig_handlers[sig];
        osa.sa_restorer = t->sig_restorer[sig];
        osa.sa_flags = t->sig_flags[sig];
        stac(); memcpy((void *)oact_ptr, &osa, sizeof osa); clac();
    }
    if (act_ptr) {
        if (!uptr(act_ptr, sizeof(struct linux_sigaction))) return -LINUX_EINVAL;
        struct linux_sigaction sa; stac(); memcpy(&sa, (void *)act_ptr, sizeof sa); clac();
        t->sig_handlers[sig] = sa.sa_handler;
        t->sig_restorer[sig] = sa.sa_restorer;
        t->sig_flags[sig] = sa.sa_flags;
    }
    return 0;
}

static i64 linux_rt_sigprocmask(u64 how, u64 set_ptr, u64 oset_ptr, u64 sigsetsize) {
    (void)sigsetsize;
    task_t *t = cur();
    if (!t) return -LINUX_EINVAL;
    if (oset_ptr) {
        if (!uptr(oset_ptr, 8)) return -LINUX_EINVAL;
        stac(); *(u64 *)(void *)oset_ptr = t->sig_blocked; clac();
    }
    if (set_ptr) {
        if (!uptr(set_ptr, 8)) return -LINUX_EINVAL;
        u64 set; stac(); set = *(u64 *)(void *)set_ptr; clac();
        set &= ~((1ULL << 9) | (1ULL << 19)); /* Cannot block SIGKILL or SIGSTOP */
        if (how == 0 /* SIG_BLOCK */)   t->sig_blocked |= set;
        else if (how == 1 /* SIG_UNBLOCK */) t->sig_blocked &= ~set;
        else if (how == 2 /* SIG_SETMASK */) t->sig_blocked = set;
    }
    return 0;
}

static i64 linux_sigaltstack(u64 ss_ptr, u64 oss_ptr) {
    task_t *t = cur();
    if (!t) return -LINUX_EINVAL;
    if (oss_ptr) {
        if (!uptr(oss_ptr, 24)) return -LINUX_EINVAL;
        stac();
        *(u64 *)(void *)oss_ptr = t->sigaltstack_sp;
        *(u32 *)(void *)(oss_ptr + 8) = t->sigaltstack_flags;
        *(u64 *)(void *)(oss_ptr + 16) = t->sigaltstack_size;
        clac();
    }
    if (ss_ptr) {
        if (!uptr(ss_ptr, 24)) return -LINUX_EINVAL;
        stac();
        t->sigaltstack_sp = *(u64 *)(void *)ss_ptr;
        t->sigaltstack_flags = *(u32 *)(void *)(ss_ptr + 8);
        t->sigaltstack_size = *(u64 *)(void *)(ss_ptr + 16);
        clac();
    }
    return 0;
}

#define FUTEX_WAIT            0
#define FUTEX_WAKE            1
#define FUTEX_REQUEUE         3
#define FUTEX_CMP_REQUEUE     4
#define FUTEX_WAKE_OP         5
#define FUTEX_LOCK_PI         6
#define FUTEX_UNLOCK_PI       7
#define FUTEX_WAIT_BITSET     9
#define FUTEX_WAKE_BITSET    10
#define FUTEX_PRIVATE_FLAG 128

static i64 linux_futex(u64 uaddr, u64 op, u64 val, u64 val2, u64 uaddr2, u64 val3) {
    if (!uptr(uaddr, 4)) return -LINUX_EINVAL;
    int cmd = (int)(op & 0x7F);
    if (cmd == FUTEX_WAIT || cmd == FUTEX_WAIT_BITSET) {
        u32 curval; stac(); curval = *(u32 *)(void *)uaddr; clac();
        if (curval != (u32)val) return -LINUX_EAGAIN;
        sched_futex_wait(uaddr);
        return 0;
    }
    if (cmd == FUTEX_WAKE || cmd == FUTEX_WAKE_BITSET) {
        return sched_futex_wake(uaddr, (int)val);
    }
    if (cmd == FUTEX_REQUEUE || cmd == FUTEX_CMP_REQUEUE) {
        if (cmd == FUTEX_CMP_REQUEUE) {
            u32 curval; stac(); curval = *(u32 *)(void *)uaddr; clac();
            if (curval != (u32)val3) return -LINUX_EAGAIN;
        }
        return sched_futex_requeue(uaddr, uaddr2, (int)val, (int)val2);
    }
    return 0;
}

static i64 linux_wait4(i64 pid, u64 status_ptr, i64 options, u64 rusage_ptr) {
    (void)rusage_ptr;
    for (;;) {
        int status = 0;
        i64 r = (i64)sched_waitpid((u32)pid, &status, (int)options);
        if (r != 0 || (options & 1 /* WNOHANG */)) {
            if (r > 0 && status_ptr && uptr(status_ptr, sizeof(int))) {
                stac();
                *(int *)(void *)status_ptr = status;
                clac();
            }
            return r;
        }
        sched_yield();
    }
}

static i64 linux_clone(cpu_regs_t *r, u64 flags, u64 child_stack, u64 ptid, u64 ctid, u64 tls) {
    task_t *parent = cur();
    if (!parent) return -LINUX_EINVAL;
    task_t *child = sched_clone_thread(parent, r, child_stack, tls, ptid, ctid, flags);
    return child ? (i64)child->pid : -LINUX_ENOMEM;
}

static i64 linux_socket(u64 domain, u64 type, u64 protocol) {
    (void)protocol;
    if (domain != LINUX_AF_INET) return -LINUX_EINVAL;
    int st = ((type & 0xF) == LINUX_SOCK_STREAM) ? 1
           : ((type & 0xF) == LINUX_SOCK_DGRAM)  ? 3 : 0;
    if (!st) return -LINUX_EINVAL;
    u64 fl = irq_save(); spin_lock(&g_lsock_lock);
    int slot = -1;
    for (int i = 0; i < LINUX_SOCK_MAX; i++) if (!g_lsock[i].in_use) { slot = i; break; }
    if (slot >= 0) { g_lsock[slot].in_use = 1; g_lsock[slot].type = st; g_lsock[slot].id = -1; }
    spin_unlock(&g_lsock_lock); irq_restore(fl);
    return slot >= 0 ? (i64)(LINUX_SOCK_FD + slot) : -1;
}

static int linux_parse_sockaddr(u64 addr, u64 len, u32 *ip, u16 *port) {
    if (len < 8 || !uptr(addr, 8)) return -1;
    u8 b[8];
    stac(); memcpy(b, (void *)addr, 8); clac();
    if ((b[0] | (b[1] << 8)) != LINUX_AF_INET) return -1;
    *ip   = ((u32)b[4] << 24) | ((u32)b[5] << 16) | ((u32)b[6] << 8) | b[7];
    *port = ntohs16((u16)(b[2] | (b[3] << 8)));
    return 0;
}

static i64 linux_sock_close(int fd) {
    int slot = linux_sock_slot(fd);
    if (slot < 0) return -LINUX_ENOTSOCK;
    u64 fl = irq_save(); spin_lock(&g_lsock_lock);
    if (g_lsock[slot].type == 1 && g_lsock[slot].id >= 0)
        net_tcp_close(g_lsock[slot].id);
    g_lsock[slot].in_use = 0;
    spin_unlock(&g_lsock_lock); irq_restore(fl);
    return 0;
}

static i64 linux_sock_connect(int fd, u64 addr, u64 len) {
    int slot = linux_sock_slot(fd);
    if (slot < 0) return -LINUX_ENOTSOCK;
    u32 ip; u16 port;
    if (linux_parse_sockaddr(addr, len, &ip, &port) != 0) return -LINUX_EINVAL;
    if (g_lsock[slot].type == 3) {
        g_lsock[slot].peer_ip = ip;
        g_lsock[slot].peer_port = port;
        return 0;
    }
    int cid = net_tcp_connect(ip, port);
    if (cid < 0) return -LINUX_ECONNREFUSED;
    g_lsock[slot].id = cid;
    g_lsock[slot].peer_ip = ip;
    g_lsock[slot].peer_port = port;
    return 0;
}

static i64 linux_sock_bind(int fd, u64 addr, u64 len) {
    int slot = linux_sock_slot(fd);
    if (slot < 0) return -LINUX_ENOTSOCK;
    u32 ip; u16 port;
    if (linux_parse_sockaddr(addr, len, &ip, &port) != 0) return -LINUX_EINVAL;
    if (g_lsock[slot].type == 3) {
        int res = net_udp_bind(port);
        if (res < 0) return -LINUX_EACCES;
    }
    g_lsock[slot].bound_port = port;
    return 0;
}

static i64 linux_sock_listen(int fd, u64 backlog) {
    (void)backlog;
    int slot = linux_sock_slot(fd);
    if (slot < 0) return -LINUX_ENOTSOCK;
    if (g_lsock[slot].type != 1) return -LINUX_EINVAL;
    int lid = net_tcp_listen(g_lsock[slot].bound_port);
    if (lid < 0) return -LINUX_EACCES;
    g_lsock[slot].id = lid;
    return 0;
}

static i64 linux_sock_accept(int fd, u64 addr, u64 addrlen) {
    int slot = linux_sock_slot(fd);
    if (slot < 0) return -LINUX_ENOTSOCK;
    if (g_lsock[slot].type != 1 || g_lsock[slot].id < 0) return -LINUX_EINVAL;
    int cid = net_tcp_accept(g_lsock[slot].id);
    if (cid < 0) return -LINUX_EAGAIN;
    u64 fl = irq_save(); spin_lock(&g_lsock_lock);
    int nslot = -1;
    for (int i = 0; i < LINUX_SOCK_MAX; i++) if (!g_lsock[i].in_use) { nslot = i; break; }
    if (nslot >= 0) {
        g_lsock[nslot].in_use = 1; g_lsock[nslot].type = 1; g_lsock[nslot].id = cid;
    }
    spin_unlock(&g_lsock_lock); irq_restore(fl);
    if (nslot < 0) { net_tcp_close(cid); return -LINUX_EMFILE; }
    if (addr && addrlen && uptr(addr, 16) && uptr(addrlen, 4)) {
        u8 b[16]; memset(b, 0, sizeof b);
        b[0] = LINUX_AF_INET;
        stac(); memcpy((void *)addr, b, 16); *(u32 *)(void *)addrlen = 16; clac();
    }
    return (i64)(LINUX_SOCK_FD + nslot);
}

static i64 linux_sock_send(int fd, u64 buf, u64 len, u64 flags, u64 addr, u64 addrlen) {
    (void)flags;
    int slot = linux_sock_slot(fd);
    if (slot < 0) return -LINUX_ENOTSOCK;
    if (len > 4096) len = 4096;
    if (!uptr(buf, len)) return -LINUX_EINVAL;
    u8 k[4096]; stac(); memcpy(k, (void *)buf, len); clac();
    if (g_lsock[slot].type == 3) {
        u32 ip = g_lsock[slot].peer_ip; u16 port = g_lsock[slot].peer_port;
        if (addr && addrlen) linux_parse_sockaddr(addr, addrlen, &ip, &port);
        if (!ip || !port) return -LINUX_EINVAL;
        net_udp_send(ip, port, k, (u16)len);
        return (i64)len;
    }
    int sent = net_tcp_send(g_lsock[slot].id, k, (int)len);
    return sent >= 0 ? (i64)sent : -LINUX_EIO;
}

static i64 linux_sock_recv(int fd, u64 buf, u64 len, u64 flags, u64 addr, u64 addrlen) {
    (void)flags; (void)addr; (void)addrlen;
    int slot = linux_sock_slot(fd);
    if (slot < 0) return -LINUX_ENOTSOCK;
    if (len > 4096) len = 4096;
    if (!uptr(buf, len)) return -LINUX_EINVAL;
    if (g_lsock[slot].type == 3) {
        u8 k[4096]; int n = net_udp_recv(k, (u16)len);
        if (n <= 0) return 0;
        stac(); memcpy((void *)buf, k, (size_t)n); clac();
        return n;
    }
    u8 k[4096]; int n = net_tcp_recv(g_lsock[slot].id, k, (int)len);
    if (n <= 0) return 0;
    stac(); memcpy((void *)buf, k, (size_t)n); clac();
    return n;
}

static i64 linux_sock_shutdown(int fd, u64 how) {
    (void)how;
    int slot = linux_sock_slot(fd);
    if (slot < 0) return -LINUX_ENOTSOCK;
    if (g_lsock[slot].type == 1 && g_lsock[slot].id >= 0)
        net_tcp_close(g_lsock[slot].id);
    g_lsock[slot].id = -1;
    return 0;
}

static i64 linux_sock_getsockname(int fd, u64 addr, u64 addrlen) {
    int slot = linux_sock_slot(fd);
    if (slot < 0) return -LINUX_ENOTSOCK;
    if (!uptr(addr, 16) || !uptr(addrlen, 4)) return -LINUX_EINVAL;
    u8 b[16]; memset(b, 0, sizeof b);
    b[0] = LINUX_AF_INET;
    u16 port = g_lsock[slot].bound_port;
    b[2] = (u8)(port >> 8); b[3] = (u8)(port & 0xFF);
    stac(); memcpy((void *)addr, b, 16); *(u32 *)(void *)addrlen = 16; clac();
    return 0;
}

static i64 linux_sock_getpeername(int fd, u64 addr, u64 addrlen) {
    int slot = linux_sock_slot(fd);
    if (slot < 0) return -LINUX_ENOTSOCK;
    if (!uptr(addr, 16) || !uptr(addrlen, 4)) return -LINUX_EINVAL;
    u8 b[16]; memset(b, 0, sizeof b);
    b[0] = LINUX_AF_INET;
    u16 port = g_lsock[slot].peer_port;
    b[2] = (u8)(port >> 8); b[3] = (u8)(port & 0xFF);
    u32 ip = g_lsock[slot].peer_ip;
    b[4] = (u8)(ip >> 24); b[5] = (u8)(ip >> 16); b[6] = (u8)(ip >> 8); b[7] = (u8)(ip & 0xFF);
    stac(); memcpy((void *)addr, b, 16); *(u32 *)(void *)addrlen = 16; clac();
    return 0;
}

static i64 linux_sock_setsockopt(int fd, u64 level, u64 optname, u64 optval, u64 optlen) {
    (void)level; (void)optname; (void)optval; (void)optlen;
    int slot = linux_sock_slot(fd);
    if (slot < 0) return -LINUX_ENOTSOCK;
    return 0;
}

static i64 linux_sock_getsockopt(int fd, u64 level, u64 optname, u64 optval, u64 optlen) {
    (void)level; (void)optname;
    int slot = linux_sock_slot(fd);
    if (slot < 0) return -LINUX_ENOTSOCK;
    if (optval && optlen && uptr(optval, 4) && uptr(optlen, 4)) {
        stac(); *(u32 *)(void *)optval = 0; *(u32 *)(void *)optlen = 4; clac();
    }
    return 0;
}

static i64 linux_socketpair(u64 domain, u64 type, u64 protocol, u64 sv_ptr) {
    (void)domain; (void)type; (void)protocol;
    if (!uptr(sv_ptr, 8)) return -LINUX_EINVAL;
    int fds[2];
    if (sys_pipe(fds) != 0) return -LINUX_EMFILE;
    stac(); ((int *)(void *)sv_ptr)[0] = fds[0]; ((int *)(void *)sv_ptr)[1] = fds[1]; clac();
    return 0;
}

static i64 linux_sock_sendmsg(int fd, u64 msg_ptr, u64 flags) {
    (void)flags;
    if (!uptr(msg_ptr, sizeof(struct linux_msghdr))) return -LINUX_EINVAL;
    struct linux_msghdr m; stac(); memcpy(&m, (void *)msg_ptr, sizeof m); clac();
    if (m.msg_iov && m.msg_iovlen > 0) {
        return linux_writev(fd, (const void *)m.msg_iov, m.msg_iovlen);
    }
    return 0;
}

static i64 linux_sock_recvmsg(int fd, u64 msg_ptr, u64 flags) {
    (void)flags;
    if (!uptr(msg_ptr, sizeof(struct linux_msghdr))) return -LINUX_EINVAL;
    struct linux_msghdr m; stac(); memcpy(&m, (void *)msg_ptr, sizeof m); clac();
    if (m.msg_iov && m.msg_iovlen > 0) {
        return linux_readv(fd, (const void *)m.msg_iov, m.msg_iovlen);
    }
    return 0;
}

static i64 linux_sendfile(int out_fd, int in_fd, u64 offset_ptr, u64 count) {
    u8 tmp[4096];
    u64 copied = 0;
    i64 off = 0;
    if (offset_ptr) {
        if (!uptr(offset_ptr, 8)) return -LINUX_EINVAL;
        stac(); off = *(i64 *)(void *)offset_ptr; clac();
    }
    while (copied < count) {
        u64 want = count - copied;
        if (want > sizeof tmp) want = sizeof tmp;
        i64 r;
        if (offset_ptr) {
            r = linux_pread64(in_fd, (u64)(uintptr_t)tmp, want, off + (i64)copied);
        } else {
            r = sys_read(in_fd, (char *)tmp, want);
        }
        if (r <= 0) break;
        i64 w = sys_write(out_fd, (const char *)tmp, (u64)r);
        if (w <= 0) break;
        copied += (u64)w;
    }
    if (offset_ptr) {
        stac(); *(i64 *)(void *)offset_ptr = off + (i64)copied; clac();
    }
    return (i64)copied;
}

static i64 linux_nanosleep(u64 ts_ptr) {
    if (!uptr(ts_ptr, sizeof(struct linux_timespec))) return -LINUX_EINVAL;
    struct linux_timespec ts;
    stac(); memcpy(&ts, (void *)ts_ptr, sizeof ts); clac();
    if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000L)
        return -LINUX_EINVAL;
    u64 ms = (u64)ts.tv_sec * 1000ULL + (u64)(ts.tv_nsec / 1000000L);
    if (ts.tv_nsec % 1000000L) ms++;
    if (ms > 0xFFFFFFFFULL) ms = 0xFFFFFFFFULL;
    if (ms) sched_sleep_ms((u32)ms);
    return 0;
}

static i64 linux_getrandom(u64 buf, u64 len, u64 flags) {
    (void)flags;
    if (len > USER_BUF_MAX || !uptr(buf, len)) return -LINUX_EINVAL;
    u8 tmp[64];
    for (u64 off = 0; off < len; off += sizeof tmp) {
        u64 n = len - off; if (n > sizeof tmp) n = sizeof tmp;
        for (u64 i = 0; i < n; i += 8) {
            u64 v = linux_rand_u64();
            u64 take = n - i; if (take > 8) take = 8;
            memcpy(tmp + i, &v, (size_t)take);
        }
        stac(); memcpy((void *)(buf + off), tmp, (size_t)n); clac();
    }
    return (i64)len;
}

static i64 linux_ftruncate(int fd, u64 len) {
    fd_entry_t *f = fd_get(fd);
    if (!f || !f->vn || f->is_pipe) return -LINUX_EBADF;
    return vfs_truncate(f->vn, (size_t)len) == 0 ? 0 : -1;
}

static i64 linux_dup(int oldfd) {
    fd_entry_t *f = fd_get(oldfd);
    if (!f) return -LINUX_EBADF;
    task_t *t = cur();
    for (int i = 0; i < MAX_FD; i++) {
        if (t->fds[i].in_use) continue;
        fd_entry_t *nf = &t->fds[i];
        *nf = *f;
        if (nf->is_pipe && nf->pipe) {
            nf->pipe->refs++;
            if (nf->pipe_is_read_end) nf->pipe->read_ends++;
            else                      nf->pipe->write_ends++;
        } else if (nf->vn) {
            vnode_ref(nf->vn);
        }
        nf->in_use = true;
        return i;
    }
    return -LINUX_EMFILE;
}

#define LINUX_F_DUPFD     0
#define LINUX_F_GETFD     1
#define LINUX_F_SETFD     2
#define LINUX_F_GETFL     3
#define LINUX_F_SETFL     4
#define LINUX_F_DUPFD_CLOEXEC 1030
#define LINUX_O_APPEND    0x400
#define LINUX_O_NONBLOCK  0x800

static i64 linux_fcntl(int fd, u64 cmd, u64 arg) {
    fd_entry_t *f = fd_get(fd);
    if (!f) return -LINUX_EBADF;
    switch (cmd) {
    case LINUX_F_DUPFD:
    case LINUX_F_DUPFD_CLOEXEC: {
        task_t *t = cur();
        int start = (int)arg;
        if (start < 0) start = 0;
        if (start >= MAX_FD) return -LINUX_EINVAL;
        for (int i = start; i < MAX_FD; i++) {
            if (t->fds[i].in_use) continue;
            fd_entry_t *nf = &t->fds[i];
            *nf = *f;
            if (nf->is_pipe && nf->pipe) {
                nf->pipe->refs++;
                if (nf->pipe_is_read_end) nf->pipe->read_ends++;
                else                      nf->pipe->write_ends++;
            } else if (nf->vn) vnode_ref(nf->vn);
            nf->in_use = true;
            return i;
        }
        return -LINUX_EMFILE;
    }
    case LINUX_F_GETFD: return 0;
    case LINUX_F_SETFD: return 0;
    case LINUX_F_GETFL: return (i64)f->flags;
    case LINUX_F_SETFL:
        f->flags = (f->flags & ~(LINUX_O_APPEND | LINUX_O_NONBLOCK)) |
                   ((u32)arg & (LINUX_O_APPEND | LINUX_O_NONBLOCK));
        return 0;
    default:
        return 0;
    }
}

static i64 linux_getppid(void) { task_t *t = cur(); return t ? (i64)t->ppid : 0; }

static i64 linux_getaffinity(u64 mask_ptr, u64 len) {
    if (!mask_ptr || !uptr(mask_ptr, len)) return -LINUX_EINVAL;
    u8 m[8]; memset(m, 0, sizeof m);
    for (int i = 0; i < 8; i++) m[i / 8] |= (u8)(1u << (i % 8));
    u64 n = len < sizeof m ? len : sizeof m;
    stac(); memcpy((void *)mask_ptr, m, (size_t)n); clac();
    return (i64)n;
}

#define LINUX_RLIMIT_STACK  3
#define LINUX_RLIMIT_NOFILE 7
static i64 linux_getrlimit(u64 res, u64 rlim_ptr) {
    if (!uptr(rlim_ptr, 16)) return -LINUX_EINVAL;
    u64 cur = (u64)-1, max = (u64)-1;
    if (res == LINUX_RLIMIT_STACK)  cur = max = 8 * 1024 * 1024;
    if (res == LINUX_RLIMIT_NOFILE) cur = max = 1024;
    stac();
    ((u64 *)(void *)rlim_ptr)[0] = cur;
    ((u64 *)(void *)rlim_ptr)[1] = max;
    clac();
    return 0;
}

static i64 linux_getrusage(u64 rusage_ptr) {
    if (!uptr(rusage_ptr, 144)) return -LINUX_EINVAL;
    stac(); memset((void *)rusage_ptr, 0, 144); clac();
    return 0;
}

static i64 linux_sysinfo(u64 info_ptr) {
    if (!uptr(info_ptr, sizeof(struct linux_sysinfo))) return -LINUX_EINVAL;
    struct linux_sysinfo si;
    memset(&si, 0, sizeof si);
    si.uptime = (long)(pit_ticks() / TICK_HZ);
    si.totalram = (long)(pmm_total_pages() * PAGE_SIZE);
    si.freeram  = (long)((pmm_total_pages() - pmm_used_pages()) * PAGE_SIZE);
    si.mem_unit = 1;
    stac(); memcpy((void *)info_ptr, &si, sizeof si); clac();
    return 0;
}

static i64 linux_sync(void) {
    extern int  blkfs_sync(void);
    extern bool blkfs_active(void);
    if (blkfs_active()) blkfs_sync();
    return 0;
}

static i64 linux_clock_getres(u64 res_ptr) {
    if (!uptr(res_ptr, 16)) return -LINUX_EINVAL;
    stac();
    ((long *)(void *)res_ptr)[0] = 0;
    ((long *)(void *)res_ptr)[1] = 1000000000L / TICK_HZ;
    clac();
    return 0;
}

static i64 linux_setpgid(u32 pid, u32 pgid) {
    task_t *t = pid ? sched_find(pid) : cur();
    if (!t) return -LINUX_ESRCH;
    t->pgid = pgid ? pgid : t->pid;
    return 0;
}
static i64 linux_getpgrp(void) { task_t *t = cur(); return t ? (i64)t->pgid : 0; }
static i64 linux_getpgid(u32 pid) { task_t *t = pid ? sched_find(pid) : cur(); return t ? (i64)t->pgid : -LINUX_ESRCH; }
static i64 linux_setsid(void) {
    task_t *t = cur();
    if (!t) return -LINUX_EINVAL;
    t->sid = t->pid; t->pgid = t->pid;
    return (i64)t->sid;
}
static i64 linux_getsid(u32 pid) { task_t *t = pid ? sched_find(pid) : cur(); return t ? (i64)t->sid : -LINUX_ESRCH; }
static i64 linux_setreuid(u32 ruid, u32 euid) {
    task_t *t = cur(); if (!t) return -LINUX_EINVAL;
    if (ruid != (u32)-1) t->uid = ruid;
    if (euid != (u32)-1) t->euid = euid;
    return 0;
}
static i64 linux_setregid(u32 rgid, u32 egid) {
    (void)egid;
    task_t *t = cur(); if (!t) return -LINUX_EINVAL;
    if (rgid != (u32)-1) t->gid = rgid;
    return 0;
}
static i64 linux_setresuid(u32 ruid, u32 euid, u32 suid) { (void)suid; return linux_setreuid(ruid, euid); }
static i64 linux_getresuid(u64 ruid_p, u64 euid_p, u64 suid_p) {
    task_t *t = cur(); if (!t) return -LINUX_EINVAL;
    stac();
    if (ruid_p && uptr(ruid_p, 4)) *(u32 *)(void *)ruid_p = t->uid;
    if (euid_p && uptr(euid_p, 4)) *(u32 *)(void *)euid_p = t->euid;
    if (suid_p && uptr(suid_p, 4)) *(u32 *)(void *)suid_p = t->euid;
    clac();
    return 0;
}
static i64 linux_setresgid(u32 rgid, u32 egid, u32 sgid) { (void)sgid; return linux_setregid(rgid, egid); }
static i64 linux_getresgid(u64 rgid_p, u64 egid_p, u64 sgid_p) {
    task_t *t = cur(); if (!t) return -LINUX_EINVAL;
    stac();
    if (rgid_p && uptr(rgid_p, 4)) *(u32 *)(void *)rgid_p = t->gid;
    if (egid_p && uptr(egid_p, 4)) *(u32 *)(void *)egid_p = t->gid;
    if (sgid_p && uptr(sgid_p, 4)) *(u32 *)(void *)sgid_p = t->gid;
    clac();
    return 0;
}
static i64 linux_getgroups(u64 size, u64 list_ptr) {
    task_t *t = cur(); if (!t) return -LINUX_EINVAL;
    if (size == 0) return t->supp_gid_count ? t->supp_gid_count : 1;
    if (!uptr(list_ptr, size * 4)) return -LINUX_EINVAL;
    stac();
    if (t->supp_gid_count > 0) memcpy((void *)list_ptr, t->supp_gids, t->supp_gid_count * 4);
    else *(u32 *)(void *)list_ptr = t->gid;
    clac();
    return t->supp_gid_count ? t->supp_gid_count : 1;
}
static i64 linux_setgroups(u64 size, u64 list_ptr) {
    task_t *t = cur(); if (!t) return -LINUX_EINVAL;
    if (size > 8) return -LINUX_EINVAL;
    if (size > 0 && uptr(list_ptr, size * 4)) {
        stac(); memcpy(t->supp_gids, (void *)list_ptr, size * 4); clac();
        t->supp_gid_count = (u8)size;
    }
    return 0;
}
static i64 linux_prctl(u64 option, u64 a1, u64 a2, u64 a3, u64 a4) {
    (void)a2; (void)a3; (void)a4;
    task_t *t = cur(); if (!t) return -LINUX_EINVAL;
    if (option == 15 /* PR_SET_NAME */) {
        if (!copy_user_str(a1, t->name, TASK_NAME_LEN)) return -LINUX_EINVAL;
        return 0;
    }
    if (option == 16 /* PR_GET_NAME */) {
        if (!uptr(a1, TASK_NAME_LEN)) return -LINUX_EINVAL;
        stac(); memcpy((void *)a1, t->name, TASK_NAME_LEN); clac();
        return 0;
    }
    return 0;
}

/* Dispatch a Linux syscall. Returns 1 = handled (rax set), 0 = translated. */
static int linux_dispatch(cpu_regs_t *r) {
    u64 nr = r->rax;
    u64 a0 = r->rdi, a1 = r->rsi, a2 = r->rdx, a3 = r->r10, a4 = r->r8, a5 = r->r9;
    switch (nr) {
    case 0:   r->rax = (u64)sys_read((int)a0, (char *)a1, a2); return 1;
    case 1:   r->rax = (u64)sys_write((int)a0, (const char *)a1, a2); return 1;
    case 2:   r->rax = (u64)sys_open((const char *)a0, (int)a1); return 1;
    case 3:   if (a0 >= (u64)LINUX_SOCK_FD) { r->rax = (u64)linux_sock_close((int)a0); return 1; }
              r->rax = (u64)sys_close((int)a0); return 1;
    case 4:   { char kp[VFS_MAX_PATH];
                if (!copy_user_str(a0, kp, sizeof kp) || !uptr(a1, sizeof(linux_stat_t))) { r->rax = (u64)-LINUX_EINVAL; return 1; }
                vnode_t *v = vfs_lookup_at(cur() ? cur()->cwd : NULL, kp);
                if (!v) { r->rax = (u64)-LINUX_ENOENT; return 1; }
                linux_fill_stat(v, (void *)a1); r->rax = 0; return 1; }
    case 5:   { fd_entry_t *f = fd_get((int)a0);
                if (!f || !uptr(a1, sizeof(linux_stat_t))) { r->rax = (u64)-LINUX_EBADF; return 1; }
                if (f->vn) linux_fill_stat(f->vn, (void *)a1);
                else { linux_stat_t st; memset(&st, 0, sizeof st); st.st_mode = 020666; stac(); memcpy((void *)a1, &st, sizeof st); clac(); }
                r->rax = 0; return 1; }
    case 6:   { char kp[VFS_MAX_PATH];
                if (!copy_user_str(a0, kp, sizeof kp) || !uptr(a1, sizeof(linux_stat_t))) { r->rax = (u64)-LINUX_EINVAL; return 1; }
                vnode_t *v = vfs_lookup_at_nofollow(cur() ? cur()->cwd : NULL, kp);
                if (!v) { r->rax = (u64)-LINUX_ENOENT; return 1; }
                linux_fill_stat(v, (void *)a1); r->rax = 0; return 1; }
    case 7:   r->rax = (u64)linux_poll(a0, a1, (i64)a2); return 1;
    case 8:   r->rax = (u64)sys_lseek((int)a0, (i64)a1, (int)a2); return 1;
    case 9:   r->rax = (u64)linux_mmap(a0, a1, a2, a3, a4, a5); return 1;
    case 10:  if (!uptr(a0, a1)) { r->rax = (u64)-LINUX_EINVAL; return 1; }
              r->rax = 0; return 1;
    case 11:  r->rax = (u64)sys_munmap(a0); return 1;
    case 12:  r->rax = (u64)sys_brk(a0); return 1;
    case 13:  r->rax = (u64)linux_rt_sigaction(a0, a1, a2, a3); return 1;
    case 14:  r->rax = (u64)linux_rt_sigprocmask(a0, a1, a2, a3); return 1;
    case 15:  sys_sigreturn(r); return 1;
    case 16:  r->rax = (u64)linux_ioctl((int)a0, a1, a2); return 1;
    case 17:  r->rax = (u64)linux_pread64((int)a0, a1, a2, (i64)a3); return 1;
    case 18:  r->rax = (u64)linux_pwrite64((int)a0, a1, a2, (i64)a3); return 1;
    case 19:  r->rax = (u64)linux_readv((int)a0, (const void *)a1, a2); return 1;
    case 20:  r->rax = (u64)linux_writev((int)a0, (const void *)a1, a2); return 1;
    case 21:  r->rax = (u64)linux_access((const char *)a0, a1); return 1;
    case 22:  r->rax = (u64)sys_pipe((int *)a0); return 1;
    case 23:  r->rax = (u64)linux_select(a0, a1, a2, a3, a4); return 1;
    case 24:  sched_yield(); r->rax = 0; return 1;
    case 25:  r->rax = (u64)linux_mremap(a0, a1, a2, a3, a4); return 1;
    case 26:  r->rax = 0; return 1; /* msync */
    case 27:  r->rax = 0; return 1; /* mincore */
    case 28:  r->rax = 0; return 1; /* madvise */
    case 32:  r->rax = (u64)linux_dup((int)a0); return 1;
    case 33:  r->rax = (u64)sys_dup2((int)a0, (int)a1); return 1;
    case 34:  sched_yield(); r->rax = (u64)-LINUX_EINTR; return 1;
    case 35:  r->rax = (u64)linux_nanosleep(a0); return 1;
    case 36:  r->rax = 0; return 1; /* getitimer */
    case 37:  r->rax = 0; return 1; /* alarm */
    case 38:  r->rax = 0; return 1; /* setitimer */
    case 39:  r->rax = (u64)task_getpid(); return 1;
    case 40:  r->rax = (u64)linux_sendfile((int)a0, (int)a1, a2, a3); return 1;
    case 41:  r->rax = (u64)linux_socket(a0, a1, a2); return 1;
    case 42:  r->rax = (u64)linux_sock_connect((int)a0, a1, a2); return 1;
    case 43:  r->rax = (u64)linux_sock_accept((int)a0, a1, a2); return 1;
    case 44:  r->rax = (u64)linux_sock_send((int)a0, a1, a2, a3, a4, a5); return 1;
    case 45:  r->rax = (u64)linux_sock_recv((int)a0, a1, a2, a3, a4, a5); return 1;
    case 46:  r->rax = (u64)linux_sock_send((int)a0, a1, a2, a3, 0, 0); return 1;
    case 47:  r->rax = (u64)linux_sock_recv((int)a0, a1, a2, a3, 0, 0); return 1;
    case 48:  r->rax = (u64)linux_sock_shutdown((int)a0, a1); return 1;
    case 49:  r->rax = (u64)linux_sock_bind((int)a0, a1, a2); return 1;
    case 50:  r->rax = (u64)linux_sock_listen((int)a0, a1); return 1;
    case 51:  r->rax = (u64)linux_sock_getsockname((int)a0, a1, a2); return 1;
    case 52:  r->rax = (u64)linux_sock_getpeername((int)a0, a1, a2); return 1;
    case 53:  r->rax = (u64)linux_socketpair(a0, a1, a2, a3); return 1;
    case 54:  r->rax = (u64)linux_sock_setsockopt((int)a0, a1, a2, a3, a4); return 1;
    case 55:  r->rax = (u64)linux_sock_getsockopt((int)a0, a1, a2, a3, a4); return 1;
    case 56:  r->rax = (u64)linux_clone(r, a0, a1, a2, a3, a4); return 1;
    case 57:  { task_t *c = sched_fork(cur(), r); r->rax = c ? (u64)c->pid : (u64)-1; return 1; }
    case 58:  { task_t *c = sched_fork(cur(), r); r->rax = c ? (u64)c->pid : (u64)-1; return 1; }
    case 59:  r->rax = SYS_EXEC; return 0; /* execve */
    case 60:  sched_exit((int)a0); r->rax = 0; return 1;
    case 61:  r->rax = (u64)linux_wait4((i64)a0, a1, (i64)a2, a3); return 1;
    case 62:  r->rax = (u64)sys_kill((u32)a0); return 1;
    case 63:  { if (!uptr(a0, sizeof(linux_utsname_t))) { r->rax = (u64)-LINUX_EINVAL; return 1; }
                linux_utsname_t u; memset(&u, 0, sizeof u);
                const char *sys = "Linux", *node = "yart", *rel = "6.6.0-yart",
                           *ver = "#1 SMP PREEMPT Fri Aug 28 2026", *mach = "x86_64";
                stac();
                memcpy(u.s, sys, strlen(sys)+1); memcpy(u.s + 65, node, strlen(node)+1);
                memcpy(u.s + 130, rel, strlen(rel)+1); memcpy(u.s + 195, ver, strlen(ver)+1);
                memcpy(u.s + 260, mach, strlen(mach)+1);
                memcpy((void *)a0, &u, sizeof u); clac();
                r->rax = 0; return 1; }
    case 72:  r->rax = (u64)linux_fcntl((int)a0, a1, a2); return 1;
    case 73:  r->rax = 0; return 1; /* flock */
    case 74:  r->rax = (u64)sys_fsync((int)a0); return 1;
    case 75:  r->rax = (u64)sys_fsync((int)a0); return 1;
    case 76:  r->rax = (u64)sys_truncate((const char *)a0, a1); return 1;
    case 77:  r->rax = (u64)linux_ftruncate((int)a0, a1); return 1;
    case 78:  r->rax = (u64)linux_getdents64((int)a0, (char *)a1, a2); return 1;
    case 79:  r->rax = (u64)sys_getcwd((char *)a0, a1); return 1;
    case 80:  r->rax = (u64)sys_chdir((const char *)a0); return 1;
    case 81:  { fd_entry_t *f = fd_get((int)a0);
                if (!f || !f->vn || f->vn->type != VN_DIR) { r->rax = (u64)-LINUX_EBADF; return 1; }
                cur()->cwd = f->vn; r->rax = 0; return 1; }
    case 82:  r->rax = (u64)sys_rename((const char *)a0, (const char *)a1); return 1;
    case 83:  r->rax = (u64)sys_mkdir((const char *)a0); return 1;
    case 84:  r->rax = (u64)sys_unlink((const char *)a0); return 1;
    case 85:  r->rax = (u64)sys_open((const char *)a0, O_CREAT | O_WRONLY | O_TRUNC); return 1;
    case 86:  r->rax = (u64)sys_symlink((const char *)a0, (const char *)a1); return 1;
    case 87:  r->rax = (u64)sys_unlink((const char *)a0); return 1;
    case 88:  r->rax = (u64)sys_symlink((const char *)a0, (const char *)a1); return 1;
    case 89:  r->rax = (u64)sys_readlink((const char *)a0, (char *)a1, a2); return 1;
    case 90:  r->rax = (u64)sys_chmod((const char *)a0, (u16)a1); return 1;
    case 91:  r->rax = (u64)linux_fchmod((int)a0, a1); return 1;
    case 92:  r->rax = 0; return 1; /* chown */
    case 93:  r->rax = 0; return 1; /* fchown */
    case 94:  r->rax = 0; return 1; /* lchown */
    case 95:  r->rax = (u64)sys_umask((u16)a0); return 1;
    case 96:  { if (!uptr(a0, 16)) { r->rax = (u64)-LINUX_EINVAL; return 1; }
                u64 sec = linux_epoch_seconds(), usec = 0;
                stac(); *(u64 *)(void *)a0 = sec; *(u64 *)(void *)(a0 + 8) = usec; clac();
                r->rax = 0; return 1; }
    case 97:  r->rax = (u64)linux_getrlimit(a0, a1); return 1;
    case 98:  r->rax = (u64)linux_getrusage(a1); return 1;
    case 99:  r->rax = (u64)linux_sysinfo(a0); return 1;
    case 100: { if (!uptr(a0, sizeof(struct linux_tms))) { r->rax = (u64)-LINUX_EINVAL; return 1; }
                struct linux_tms tms; memset(&tms, 0, sizeof tms);
                tms.tms_utime = (long)(pit_ticks() / 2);
                tms.tms_stime = (long)(pit_ticks() / 2);
                stac(); memcpy((void *)a0, &tms, sizeof tms); clac();
                r->rax = (u64)pit_ticks(); return 1; }
    case 102: r->rax = (u64)cur()->uid;  return 1;
    case 104: r->rax = (u64)cur()->gid;  return 1;
    case 105: r->rax = (u64)sys_setuid((u32)a0); return 1;
    case 106: r->rax = (u64)sys_setgid((u32)a0); return 1;
    case 107: r->rax = (u64)cur()->euid; return 1;
    case 108: r->rax = (u64)cur()->gid;  return 1;
    case 109: r->rax = (u64)linux_setpgid((u32)a0, (u32)a1); return 1;
    case 110: r->rax = (u64)linux_getppid(); return 1;
    case 111: r->rax = (u64)linux_getpgrp(); return 1;
    case 112: r->rax = (u64)linux_setsid(); return 1;
    case 113: r->rax = (u64)linux_setreuid((u32)a0, (u32)a1); return 1;
    case 114: r->rax = (u64)linux_setregid((u32)a0, (u32)a1); return 1;
    case 115: r->rax = (u64)linux_getgroups(a0, a1); return 1;
    case 116: r->rax = (u64)linux_setgroups(a0, a1); return 1;
    case 117: r->rax = (u64)linux_setresuid((u32)a0, (u32)a1, (u32)a2); return 1;
    case 118: r->rax = (u64)linux_getresuid(a0, a1, a2); return 1;
    case 119: r->rax = (u64)linux_setresgid((u32)a0, (u32)a1, (u32)a2); return 1;
    case 120: r->rax = (u64)linux_getresgid(a0, a1, a2); return 1;
    case 121: r->rax = (u64)linux_getpgid((u32)a0); return 1;
    case 122: r->rax = 0; return 1; /* setfsuid */
    case 123: r->rax = 0; return 1; /* setfsgid */
    case 124: r->rax = (u64)linux_getsid((u32)a0); return 1;
    case 125: r->rax = 0; return 1; /* capget */
    case 126: r->rax = 0; return 1; /* capset */
    case 127: { if (!uptr(a0, 8)) { r->rax = (u64)-LINUX_EINVAL; return 1; }
                stac(); *(u64 *)(void *)a0 = cur()->sig_pending; clac();
                r->rax = 0; return 1; }
    case 130: r->rax = 0; return 1; /* rt_sigsuspend */
    case 131: r->rax = (u64)linux_sigaltstack(a0, a1); return 1;
    case 132: r->rax = (u64)linux_utime(a0, a1); return 1;
    case 133: r->rax = (u64)linux_mknod(a0, a1, a2); return 1;
    case 135: r->rax = 0; return 1; /* personality */
    case 137: r->rax = (u64)linux_statfs(a0, a1); return 1;
    case 138: r->rax = (u64)linux_fstatfs((int)a0, a1); return 1;
    case 142: r->rax = 0; return 1; /* sched_setparam */
    case 143: r->rax = 0; return 1; /* sched_getparam */
    case 144: r->rax = 0; return 1; /* sched_setscheduler */
    case 145: r->rax = 0; return 1; /* sched_getscheduler */
    case 146: r->rax = 99; return 1; /* sched_get_priority_max */
    case 147: r->rax = 1; return 1; /* sched_get_priority_min */
    case 149: r->rax = 0; return 1; /* mlock */
    case 150: r->rax = 0; return 1; /* munlock */
    case 151: r->rax = 0; return 1; /* mlockall */
    case 152: r->rax = 0; return 1; /* munlockall */
    case 157: r->rax = (u64)linux_prctl(a0, a1, a2, a3, a4); return 1;
    case 158: { task_t *t = cur();
                if (a0 == 0x1002) {
                    t->linux_fs_base = a1;
                    wrmsr64(MSR_FS_BASE, a1);
                    r->rax = 0; return 1;
                }
                if (a0 == 0x1003) { r->rax = (u64)t->linux_fs_base; return 1; }
                r->rax = (u64)-LINUX_EINVAL; return 1; }
    case 160: r->rax = 0; return 1; /* setrlimit */
    case 162: r->rax = (u64)linux_sync(); return 1;
    case 186: r->rax = (u64)task_getpid(); return 1;
    case 187: r->rax = 0; return 1; /* readahead */
    case 200: case 234: r->rax = (u64)sys_kill((u32)a0); return 1;
    case 201: r->rax = (u64)linux_epoch_seconds(); return 1;
    case 202: r->rax = (u64)linux_futex(a0, a1, a2, a3, a4, a5); return 1;
    case 203: r->rax = 0; return 1; /* sched_setaffinity */
    case 204: r->rax = (u64)linux_getaffinity(a1, a2); return 1;
    case 213: r->rax = (u64)linux_epoll_create1(0); return 1;
    case 217: r->rax = (u64)linux_getdents64((int)a0, (char *)a1, a2); return 1;
    case 218: { task_t *t = cur(); t->clear_child_tid = a0; r->rax = (u64)t->pid; return 1; }
    case 221: r->rax = 0; return 1; /* fadvise64 */
    case 228: { if (!uptr(a1, 16)) { r->rax = (u64)-LINUX_EINVAL; return 1; }
                u64 sec = linux_epoch_seconds(), nsec = 0;
                stac(); *(u64 *)(void *)a1 = sec; *(u64 *)(void *)(a1 + 8) = nsec; clac();
                r->rax = 0; return 1; }
    case 229: r->rax = (u64)linux_clock_getres(a0); return 1;
    case 230: r->rax = (u64)linux_nanosleep(a0); return 1;
    case 231: { task_t *t = cur(); sched_exit_group(t->pml4, (int)a0); sched_exit((int)a0); r->rax = 0; return 1; }
    case 232: r->rax = (u64)linux_epoll_wait((int)a0, a1, (int)a2, (int)a3); return 1;
    case 233: r->rax = (u64)linux_epoll_ctl((int)a0, (int)a1, (int)a2, a3); return 1;
    case 235: r->rax = (u64)linux_utimes(a0, a1); return 1;
    case 247: r->rax = (u64)linux_wait4((i64)a1, a2, (i64)a3, 0); return 1; /* waitid */
    case 257: r->rax = (u64)linux_openat((i64)a0, a1, a2, a3); return 1;
    case 258: r->rax = (u64)linux_mkdirat((i64)a0, a1, a2); return 1;
    case 259: r->rax = (u64)linux_mknodat((i64)a0, a1, a2, a3); return 1;
    case 260: r->rax = 0; return 1; /* fchownat */
    case 261: r->rax = (u64)linux_futimesat((i64)a0, a1, a2); return 1;
    case 262: r->rax = (u64)linux_newfstatat((i64)a0, a1, a2, a3); return 1;
    case 263: r->rax = (u64)linux_unlinkat((i64)a0, a1, a2); return 1;
    case 264: r->rax = (u64)linux_renameat((i64)a0, a1, (i64)a2, a3); return 1;
    case 265: r->rax = (u64)linux_symlinkat(a0, (i64)a1, a2); return 1; /* linkat */
    case 266: r->rax = (u64)linux_symlinkat(a0, (i64)a1, a2); return 1; /* symlinkat */
    case 267: r->rax = (u64)linux_readlinkat((i64)a0, a1, a2, a3); return 1;
    case 268: r->rax = (u64)linux_fchmodat((i64)a0, a1, a2, a3); return 1;
    case 269: r->rax = (u64)linux_faccessat((i64)a0, a1, a2, a3); return 1;
    case 270: r->rax = (u64)linux_pselect6(a0, a1, a2, a3, a4, a5); return 1;
    case 271: r->rax = (u64)linux_ppoll(a0, a1, a2, a3); return 1;
    case 273: { task_t *t = cur(); t->robust_list_head = a0; r->rax = 0; return 1; }
    case 274: { task_t *t = a0 ? sched_find((u32)a0) : cur();
                if (!t || !uptr(a1, 8) || !uptr(a2, 8)) { r->rax = (u64)-LINUX_EINVAL; return 1; }
                stac(); *(u64 *)(void *)a1 = t->robust_list_head; *(u64 *)(void *)a2 = 24; clac();
                r->rax = 0; return 1; }
    case 280: r->rax = (u64)linux_utimensat((i64)a0, a1, a2, a3); return 1;
    case 281: r->rax = (u64)linux_epoll_wait((int)a0, a1, (int)a2, (int)a3); return 1;
    case 284: r->rax = (u64)linux_eventfd(a0, a1); return 1;
    case 285: r->rax = 0; return 1; /* fallocate */
    case 288: r->rax = (u64)linux_sock_accept((int)a0, a1, a2); return 1; /* accept4 */
    case 290: r->rax = (u64)linux_eventfd(a0, a1); return 1;
    case 291: r->rax = (u64)linux_epoll_create1(a0); return 1;
    case 292: r->rax = (u64)sys_dup2((int)a0, (int)a1); return 1;
    case 293: r->rax = (u64)sys_pipe((int *)a0); return 1;
    case 297: r->rax = 0; return 1; /* rt_tgsigqueueinfo */
    case 299: r->rax = 0; return 1; /* recvmmsg */
    case 302: r->rax = (u64)linux_getrlimit(a0, a1); return 1;
    case 306: r->rax = (u64)linux_sync(); return 1;
    case 307: r->rax = 0; return 1; /* sendmmsg */
    case 309: r->rax = (u64)sys_getcpu(); return 1;
    case 316: r->rax = (u64)linux_renameat2((i64)a0, a1, (i64)a2, a3, a4); return 1;
    case 318: r->rax = (u64)linux_getrandom(a0, a1, a2); return 1;
    case 324: r->rax = 0; return 1; /* membarrier */
    case 325: r->rax = 0; return 1; /* mlock2 */
    case 326: r->rax = (u64)linux_sendfile((int)a2, (int)a0, 0, a4); return 1; /* copy_file_range */
    case 332: r->rax = (u64)linux_statx((i64)a0, a1, a2, a3, a4); return 1;

    default:
        r->rax = (u64)-LINUX_ENOSYS;
        return 1;
    }
    return 0;   /* fall through to the YartOS switch */
}

static void syscall_handler(cpu_regs_t *r) {
    check_user_segments(r);
    g_sys_from_user = ((r->cs & 3) == 3);
    if (cur() == g_wm_task) {
        extern int g_desktop_wd;
        watchdog_kick(g_desktop_wd);
    }
    if (g_sys_from_user && cur()->linux_abi) {
        if (linux_dispatch(r) == 1) return;   /* handled (or -ENOSYS) */
        /* else: translated -> fall through to the YartOS switch */
    }
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
    case SYS_SYMLINK:      r->rax = (u64)sys_symlink((const char *)a0, (const char *)a1); break;
    case SYS_READLINK:     r->rax = (u64)sys_readlink((const char *)a0, (char *)a1, a2); break;
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
#define WM_SURF_APP_BASE 0x60000000UL
#define WM_SURF_WM_BASE  0x64000000UL
#define WM_SURF_STRIDE   0x400000UL        /* 4 MiB per slot             */
#define WM_SURF_MAX_W 1280
#define WM_SURF_MAX_H 800
#define WM_SURF_MAX_PAGES (WM_SURF_STRIDE / PAGE_SIZE)   /* 1024 */

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
    if (t && t->name[0]) {
        if (strcmp(t->name, "calc") == 0) strncpy(s->title, "Calculator", sizeof s->title - 1);
        else if (strcmp(t->name, "files") == 0) strncpy(s->title, "Files", sizeof s->title - 1);
        else if (strcmp(t->name, "editor") == 0) strncpy(s->title, "Text", sizeof s->title - 1);
        else if (strcmp(t->name, "viewer") == 0) strncpy(s->title, "Viewer", sizeof s->title - 1);
        else if (strcmp(t->name, "browser") == 0) strncpy(s->title, "Browser", sizeof s->title - 1);
        else if (strcmp(t->name, "settings") == 0) strncpy(s->title, "Settings", sizeof s->title - 1);
        else if (strcmp(t->name, "media") == 0) strncpy(s->title, "Media", sizeof s->title - 1);
        else if (strcmp(t->name, "camera") == 0) strncpy(s->title, "Camera", sizeof s->title - 1);
        else if (strcmp(t->name, "nyra") == 0) strncpy(s->title, "Console", sizeof s->title - 1);
        else strncpy(s->title, t->name, sizeof s->title - 1);
    }

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

/* SYS_WM_MOVE(id, x, y): move window, only wm or owner can move */
static i64 sys_wm_move(u32 id, int x, int y) {
    if (id >= WM_MAX_SURFACES || !g_wm_surfs[id].used) return -1;
    task_t *t = cur();
    if (t && t->pid != g_wm_surfs[id].owner_pid && (!g_wm_task || t->pid != g_wm_task->pid)) return -1;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > (int)g_fb.width - 20) x = (int)g_fb.width - 20;
    if (y > (int)g_fb.height - 20) y = (int)g_fb.height - 20;
    g_wm_surfs[id].win_x = (u32)x;
    g_wm_surfs[id].win_y = (u32)y;
    g_wm_surfs[id].dirty = true;
    return 0;
}

/* SYS_WM_RESIZE(id, w, h): resize window (realloc pages) */
static i64 sys_wm_resize(u32 id, u32 w, u32 h) {
    if (id >= WM_MAX_SURFACES || !g_wm_surfs[id].used) return -1;
    task_t *t = cur();
    if (t && t->pid != g_wm_surfs[id].owner_pid && (!g_wm_task || t->pid != g_wm_task->pid)) return -1;
    if (w < 100 || h < 100 || w > WM_SURF_MAX_W || h > WM_SURF_MAX_H) return -1;
    wm_surface_t *s = &g_wm_surfs[id];
    u32 new_pages = (w * h * 4 + PAGE_SIZE - 1) / PAGE_SIZE;
    if (new_pages > WM_SURF_MAX_PAGES) return -1;

    /* GROW: allocate the extra pages and map them ONLY on the WM side (the
     * compositor must be able to read the enlarged canvas).  The app side is
     * deliberately left alone: the app keeps drawing at its ORIGINAL size, so
     * extending its mapping is unnecessary. */
    if (new_pages > s->npages) {
        for (u32 i = s->npages; i < new_pages; i++) {
            paddr_t pg = pmm_alloc_page();
            if (!pg) return -1;
            s->pages[i] = pg;
            if (g_wm_task && g_wm_task->pml4) {
                vmm_map_in(g_wm_task->pml4, s->wm_va + i * PAGE_SIZE, pg,
                           PTE_PRESENT | PTE_RW | PTE_US | PTE_NX | PTE_NOSHR);
                pmm_ref_page(pg);
            }
        }
        if (g_wm_task != cur()) smp_tlb_shootdown_all();
    }
    /* SHRINK: do NOT unmap or free pages.  The app keeps drawing at its
     * original size, and a shrink here used to vmm_unmap_in() the tail pages
     * out of the app's address space - the app then faulted on the very next
     * draw ("SIGSEGV on window resize") and was killed.  The WM just
     * composites a smaller rect; the pages are reclaimed on surface destroy. */

    s->npages = new_pages > s->npages ? new_pages : s->npages;
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

    /* If the COMPOSITOR itself died, reclaim the framebuffer so a new
     * compositor (a restarted graphical session) can claim it.  Without this,
     * g_wm_task points at a dead task forever and no process can ever take
     * over the screen - the WM session model is impossible.  The wm task's
     * pml4 (and its fb PTEs) is freed by the reaper, which unrefs the fb
     * pages we ref'd in sys_fb_info, so this just clears the claim. */
    if (g_wm_task && g_wm_task->pid == pid) {
        kprintf("wm: compositor (pid %u) died - reclaiming framebuffer\n", pid);
        g_wm_task = NULL;
        g_wm_uaddr = NULL;
        g_wm_pages = 0;
        /* drop to the text fallback screen (no wm to composite for us) */
        fb_fallback_screen();
    }
}

/* SYS_TIME_MS -> uptime in milliseconds (TSC-backed monotonic clock). */
static u64 sys_time_ms(void) {
    return time_ms();
}

void syscall_install_percpu(void) {
    /* Program the per-CPU MSRs for the SYSCALL/SYSRET fast path.  EFER.SCE,
     * EFER.NXE, STAR, LSTAR and SFMASK are core-local MSRs - they must be
     * loaded on every CPU (BSP and each AP), otherwise userland executing
     * `syscall` on an AP will #UD, or touching NX pages will #PF(RSVD).
     * The BSP also calls this from syscall_install(). */
    wrmsr64(MSR_EFER,   rdmsr64(MSR_EFER) | (1ULL << 0) | (1ULL << 11)); /* EFER.SCE + EFER.NXE */
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
