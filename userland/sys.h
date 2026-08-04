/*
 * Yart userland - tiny syscall wrappers + libc.
 * Compiled freestanding.  No glibc, no startup files.
 */
#ifndef YART_USER_SYS_H
#define YART_USER_SYS_H

typedef unsigned long  size_t;
typedef long           ssize_t;
typedef unsigned char  uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int   uint32_t;
typedef unsigned long  uint64_t;
typedef long           int64_t;

/* short aliases used by userland code */
typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef unsigned long  u64;
typedef signed char    i8;
typedef signed short   i16;
typedef signed int     i32;
typedef signed long    i64;
typedef unsigned char  bool;
#define true 1
#define false 0
#define NULL ((void *)0)

enum {
    SYS_EXIT     = 0,
    SYS_WRITE    = 1,
    SYS_READ     = 2,
    SYS_OPEN     = 3,
    SYS_CLOSE    = 4,
    SYS_LSEEK    = 5,
    SYS_GETDENTS = 6,
    SYS_MKDIR    = 7,
    SYS_UNLINK   = 8,
    SYS_STAT     = 9,
    SYS_GETCWD   = 10,
    SYS_CHDIR    = 11,
    SYS_GETPID   = 12,
    SYS_TIME     = 13,
    SYS_YIELD    = 14,
    SYS_TRUNCATE = 15,
    SYS_KLOG     = 16,
    SYS_FORK     = 17,
    SYS_WAITPID  = 18,
    SYS_DOAS     = 19,
    SYS_CHMOD    = 20,
    SYS_DROP     = 21,
    SYS_KILL     = 22,
    SYS_MMAP     = 23,
    SYS_MUNMAP   = 24,
    SYS_SETUID   = 25,
    SYS_RENAME   = 26,
    SYS_BRK      = 27,
    SYS_SIGACTION = 28,
    SYS_RAISE    = 29,
    SYS_FSYNC    = 30,
    SYS_SETGID   = 31,
    SYS_UMASK    = 32,
    SYS_ACL      = 33,
    SYS_GETCPU   = 34,
    SYS_DMESG    = 35,
    SYS_NET_INFO = 36,
    SYS_UDP_SEND = 37,
    SYS_UDP_RECV = 38,
    SYS_FB_INFO  = 39,
    SYS_FB_FLIP  = 40,
    SYS_POLL_KEY = 41,
    SYS_POLL_MOUSE = 42,
    SYS_TIME_MS  = 43,
    SYS_SLEEP    = 44,
    SYS_EXEC     = 45,
    SYS_WM_CREATE = 46,
    SYS_WM_FLIP   = 47,
    SYS_WM_SCAN   = 48,
    SYS_WM_FOCUS  = 49,
    SYS_WM_DESTROY = 50,
    SYS_SIGRETURN  = 51,
    SYS_WM_TITLE   = 52,
    SYS_PIPE       = 53,
};

#define O_RDONLY 0x0
#define O_WRONLY 0x1
#define O_RDWR   0x2
#define O_CREAT  0x40
#define O_TRUNC  0x200

/* Fast path: the `syscall` instruction (EFER.SCE + STAR/LSTAR/SFMASK,
 * GDT slot-5 user code 0x2B).  The kernel keeps int 0x80 as a fallback,
 * but userland uses the native instruction - this is the path the kernel's
 * syscall_entry.asm was built for.  SysV-ish register ABI: rdi/rsi/rdx/r10
 * carry args, rax the number, result back in rax; rcx/r11 are clobbered
 * by the hardware. */
static inline long _sc(long n, long a, long b, long c) {
    long r;
    __asm__ volatile (
        "syscall"
        : "=a"(r)
        : "a"(n), "D"(a), "S"(b), "d"(c)
        : "memory", "rcx", "r11"
    );
    return r;
}
static inline long _sc4(long n, long a, long b, long c, long d) {
    long r;
    register long r10 __asm__("r10") = d;
    __asm__ volatile (
        "syscall"
        : "=a"(r)
        : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10)
        : "memory", "rcx", "r11"
    );
    return r;
}

static inline ssize_t write(int fd, const void *buf, size_t n) {
    return _sc(SYS_WRITE, fd, (long)buf, (long)n);
}
static inline ssize_t read(int fd, void *buf, size_t n) {
    return _sc(SYS_READ, fd, (long)buf, (long)n);
}
static inline int open(const char *p, int f) { return _sc(SYS_OPEN, (long)p, f, 0); }
static inline int close(int fd) { return _sc(SYS_CLOSE, fd, 0, 0); }
static inline int klog(const char *s) { return _sc(SYS_KLOG, (long)s, 0, 0); }
static inline int unlink(const char *p) { return _sc(SYS_UNLINK, (long)p, 0, 0); }
static inline int yield(void) { return _sc(SYS_YIELD, 0, 0, 0); }
static inline long getpid(void) { return _sc(SYS_GETPID, 0, 0, 0); }
static inline long fork(void) { return _sc(SYS_FORK, 0, 0, 0); }
static inline long waitpid(long pid, int *status) { return _sc(SYS_WAITPID, pid, (long)status, 0); }
/* waitpid with WNOHANG: returns 0 immediately if the child is alive */
static inline long waitpid_nohang(long pid, int *status) {
    return _sc4(SYS_WAITPID, pid, (long)status, 0, 1);
}
/* pipe(fds): fds[0]=read end, fds[1]=write end.  0 = ok, -1 = fail. */
static inline long pipe(int *fds) { return _sc(SYS_PIPE, (long)fds, 0, 0); }
/* read/write on a pipe return -2 when it would block (buffer empty/full);
 * userland should sleep briefly and retry. */
#define PIPE_WOULD_BLOCK (-2)
static inline long doas(const char *password) { return _sc(SYS_DOAS, (long)password, 0, 0); }
static inline long chmod(const char *path, long mode) { return _sc(SYS_CHMOD, (long)path, mode, 0); }
static inline long drop_priv(void) { return _sc(SYS_DROP, 0, 0, 0); }
static inline long kill(long pid) { return _sc(SYS_KILL, pid, 0, 0); }
static inline long getcpu(void) { return _sc(SYS_GETCPU, 0, 0, 0); }
static inline long dmesg(char *buf, long start, long max) { return _sc(SYS_DMESG, (long)buf, start, max); }
#define DMESG_TOTAL 0x7FFFFFFF
static inline long net_info(unsigned int *out) { return _sc(SYS_NET_INFO, (long)out, 0, 0); }
static inline long udp_send(unsigned int ip, unsigned short port, const char *buf, long len)
    { return _sc4(SYS_UDP_SEND, (long)ip, port, (long)buf, len); }
static inline long udp_recv(char *buf, long cap) { return _sc(SYS_UDP_RECV, (long)buf, cap, 0); }
static inline long mmap(long len) { return _sc(SYS_MMAP, len, 0, 0); }
static inline long munmap(long addr) { return _sc(SYS_MUNMAP, addr, 0, 0); }
static inline long setuid(long uid) { return _sc(SYS_SETUID, uid, 0, 0); }
static inline long rename(const char *o, const char *n) { return _sc(SYS_RENAME, (long)o, (long)n, 0); }
static inline long brk(long addr) { return _sc(SYS_BRK, addr, 0, 0); }
static inline long sigaction(long sig, long handler) { return _sc(SYS_SIGACTION, sig, handler, 0); }
static inline long raise(long pid, long sig) { return _sc(SYS_RAISE, pid, sig, 0); }
static inline long fsync(long fd) { return _sc(SYS_FSYNC, fd, 0, 0); }
static inline long setgid(long gid) { return _sc(SYS_SETGID, gid, 0, 0); }
static inline long umask(long m) { return _sc(SYS_UMASK, m, 0, 0); }
static inline long acl(const char *p, long uid, long mask) { return _sc(SYS_ACL, (long)p, uid, mask); }
static inline void exit(int n) { _sc(SYS_EXIT, n, 0, 0); for(;;); }

static inline size_t strlen(const char *s) { size_t n=0; while(s[n]) n++; return n; }
static inline int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
static inline char *strncpy(char *d, const char *s, size_t n) {
    size_t i = 0;
    while (s[i] && i < n) { d[i] = s[i]; i++; }
    while (i < n) d[i++] = 0;
    return d;
}
static inline int puts(const char *s) { klog(s); return 0; }
static inline void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d=dst; const unsigned char *s=src;
    for (size_t i=0;i<n;i++) { d[i]=s[i]; } return dst;
}
static inline void *memset(void *dst, int c, size_t n) {
    unsigned char *d=dst;
    for (size_t i=0;i<n;i++) { d[i]=(unsigned char)c; } return dst;
}

/* --- compositor / wm syscalls --- */
typedef struct { unsigned w, h, pitch, bpp, rgb; } fb_info_t;
typedef struct { int dx, dy, wheel; unsigned char buttons; } mouse_ev_t;
static inline void *fb_info(fb_info_t *i) { return (void *)(u64)_sc(SYS_FB_INFO, (long)i, 0, 0); }
static inline long fb_flip(void *p)           { return _sc(SYS_FB_FLIP, (long)p, 0, 0); }
static inline int  poll_key(void)            { return (int)_sc(SYS_POLL_KEY, 0, 0, 0); }
static inline int  poll_mouse(mouse_ev_t *m) { return (int)_sc(SYS_POLL_MOUSE, (long)m, 0, 0); }
static inline long time_ms(void)    { return _sc(SYS_TIME_MS, 0, 0, 0); }
/* SYS_TIME returns packed YYYYMMDDhhmmss as one i64 (RTC) */
static inline long wall_time(void)  { return _sc(SYS_TIME, 0, 0, 0); }
/* SYS_SLEEP: block the calling task (no busy loop) for ms milliseconds */
static inline long sleep(long ms)   { return _sc(SYS_SLEEP, ms, 0, 0); }
/* SYS_EXEC: replace this process with `path`; argv/envp are NULL-terminated
 * arrays of strings.  Returns 0 inside the NEW program on success. */
static inline long exec(const char *path, char *const argv[], char *const envp[]) {
    return _sc(SYS_EXEC, (long)path, (long)argv, (long)envp);
}

/* --- window surfaces (real ring-3 apps) --- */
typedef struct {
    unsigned id, w, h;
    unsigned win_x, win_y;
    unsigned long long app_va;   /* mapped canvas (side depends on call) */
    unsigned owner_pid;
    unsigned dirty;
} wm_surf_info_t;
static inline long wm_create(unsigned w, unsigned h, wm_surf_info_t *o) {
    return _sc(SYS_WM_CREATE, w, h, (long)o);
}
static inline long wm_flip(unsigned id) { return _sc(SYS_WM_FLIP, id, 0, 0); }
static inline long wm_scan(wm_surf_info_t *o, unsigned max) {
    return _sc(SYS_WM_SCAN, (long)o, max, 0);
}
static inline long wm_focus(unsigned pid) { return _sc(SYS_WM_FOCUS, pid, 0, 0); }
static inline long wm_destroy(unsigned id) { return _sc(SYS_WM_DESTROY, id, 0, 0); }
static inline long wm_title(unsigned id, const char *t) {
    return _sc(SYS_WM_TITLE, id, (long)t, 0);
}

#endif
