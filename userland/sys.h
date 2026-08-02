/*
 * Yart userland - tiny syscall wrappers + libc.
 * Compiled freestanding.  No glibc, no startup files.
 */
#ifndef YART_USER_SYS_H
#define YART_USER_SYS_H

typedef unsigned long  size_t;
typedef long           ssize_t;
typedef unsigned char  uint8_t;
typedef unsigned int   uint32_t;
typedef unsigned long  uint64_t;
typedef long           int64_t;

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
};

#define O_RDONLY 0x0
#define O_WRONLY 0x1
#define O_RDWR   0x2
#define O_CREAT  0x40
#define O_TRUNC  0x200

static inline long _sc(long n, long a, long b, long c) {
    long r;
    /* Fast syscall/sysret path (kernel sets EFER.SCE + STAR + LSTAR).
     * ABI: number in rax, args in rdi/rsi/rdx; result in rax; rcx/r11
     * are clobbered by the CPU (return RIP / saved RFLAGS) so the compiler
     * must treat them as dead.  `int $0x80` remains as the kernel fallback
     * but userland takes the fast path. */
    __asm__ volatile (
        "syscall"
        : "=a"(r)
        : "a"(n), "D"(a), "S"(b), "d"(c)
        : "memory", "rcx", "r11"
    );
    return r;
}
/* 4-arg variant: the 4th argument goes in r10 (Linux/SysV convention), which
 * the fast-path entry preserves (it stashes the user RSP in a per-CPU slot,
 * not a register). */
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
static inline long doas(const char *password) { return _sc(SYS_DOAS, (long)password, 0, 0); }
static inline long chmod(const char *path, long mode) { return _sc(SYS_CHMOD, (long)path, mode, 0); }
static inline long drop_priv(void) { return _sc(SYS_DROP, 0, 0, 0); }
static inline long kill(long pid) { return _sc(SYS_KILL, pid, 0, 0); }
static inline long getcpu(void) { return _sc(SYS_GETCPU, 0, 0, 0); }
/* read the kernel audit log: dmesg(buf, start_line, max_lines) -> lines copied */
static inline long dmesg(char *buf, long start, long max) { return _sc(SYS_DMESG, (long)buf, start, max); }
/* dmesg(NULL, 0x7FFFFFFF, 0) returns the total number of log lines */
#define DMESG_TOTAL 0x7FFFFFFF
/* networking */
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
static inline int puts(const char *s) {
    write(1, s, strlen(s));
    write(1, "\n", 1);
    return 0;
}

#endif
