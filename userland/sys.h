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
};

#define O_RDONLY 0x0
#define O_WRONLY 0x1
#define O_RDWR   0x2
#define O_CREAT  0x40
#define O_TRUNC  0x200

static inline long _sc(long n, long a, long b, long c) {
    long r;
    register long r10 __asm__("r10") = 0;
    __asm__ volatile (
        "int $0x80"
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
static inline int yield(void) { return _sc(SYS_YIELD, 0, 0, 0); }
static inline void exit(int n) { _sc(SYS_EXIT, n, 0, 0); for(;;); }

static inline size_t strlen(const char *s) { size_t n=0; while(s[n]) n++; return n; }
static inline int puts(const char *s) {
    write(1, s, strlen(s));
    write(1, "\n", 1);
    return 0;
}

#endif
