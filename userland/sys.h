/*
 * Yart userland - tiny syscall wrappers + libc.
 * Compiled freestanding.  No glibc, no startup files.
 * v3: WiFi + max filesystem + enhanced WM
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
    SYS_TCP_CONNECT = 54,
    SYS_TCP_SEND    = 55,
    SYS_TCP_RECV    = 56,
    SYS_TCP_CLOSE   = 57,
    SYS_TCP_LISTEN  = 58,
    SYS_TCP_ACCEPT  = 59,
    SYS_DNS_RESOLVE = 60,
    SYS_NET_FW_ADD  = 61,
    SYS_NET_FW_CLEAR = 62,
    SYS_UDP_BIND    = 63,
    SYS_ICMP_PING   = 64,
    SYS_ICMP6_PING  = 65,
    SYS_IPV6_INFO   = 66,
    SYS_TLS_CONNECT = 67,
    SYS_TLS_SEND    = 68,
    SYS_TLS_RECV    = 69,
    SYS_TLS_CLOSE   = 70,
    SYS_TLS_LISTEN  = 71,
    SYS_TLS_ACCEPT  = 72,
    SYS_WIFI_SCAN   = 73,
    SYS_WIFI_CONNECT = 74,
    SYS_WIFI_STATUS = 75,
    SYS_WM_MOVE     = 76,
    SYS_WM_RESIZE   = 77,
    SYS_WIFI_DISCONNECT = 78,
    SYS_TASK_LIST   = 79,
    SYS_FB_PRESENT  = 80,
    SYS_AUDIO_VOL   = 81,
    SYS_AUTH_VERIFY = 82,
    SYS_NOTIFY      = 83,
    SYS_NOTIFY_POLL = 84,
    SYS_BATTERY     = 85,
    SYS_CLIPBOARD_SET = 86,
    SYS_CLIPBOARD_GET = 87,
    SYS_MOUSE_POS    = 88,
    SYS_PASSWD       = 89,
    SYS_REBOOT       = 90,
    SYS_DUP2         = 91,
};

#define O_RDONLY 0x0
#define O_WRONLY 0x1
#define O_RDWR   0x2
#define O_CREAT  0x40
#define O_TRUNC  0x200

static inline long _sc(long n, long a, long b, long c) {
    long r;
    __asm__ volatile ("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory", "rcx", "r11");
    return r;
}
static inline long _sc4(long n, long a, long b, long c, long d) {
    long r; register long r10 __asm__("r10") = d;
    __asm__ volatile ("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10) : "memory", "rcx", "r11");
    return r;
}

static inline ssize_t write(int fd, const void *buf, size_t n) { return _sc(SYS_WRITE, fd, (long)buf, (long)n); }
static inline ssize_t read(int fd, void *buf, size_t n) { return _sc(SYS_READ, fd, (long)buf, (long)n); }
static inline int open(const char *p, int f) { return _sc(SYS_OPEN, (long)p, f, 0); }
static inline int close(int fd) { return _sc(SYS_CLOSE, fd, 0, 0); }
static inline int klog(const char *s) { return _sc(SYS_KLOG, (long)s, 0, 0); }
static inline int unlink(const char *p) { return _sc(SYS_UNLINK, (long)p, 0, 0); }
static inline int mkdir(const char *p) { return (int)_sc(SYS_MKDIR, (long)p, 0, 0); }
static inline int yield(void) { return _sc(SYS_YIELD, 0, 0, 0); }
static inline long getpid(void) { return _sc(SYS_GETPID, 0, 0, 0); }
static inline long fork(void) { return _sc(SYS_FORK, 0, 0, 0); }
static inline long waitpid(long pid, int *status) { return _sc(SYS_WAITPID, pid, (long)status, 0); }
static inline long waitpid_nohang(long pid, int *status) { return _sc4(SYS_WAITPID, pid, (long)status, 0, 1); }
static inline long pipe(int *fds) { return _sc(SYS_PIPE, (long)fds, 0, 0); }
#define PIPE_WOULD_BLOCK (-2)
static inline long doas(const char *password) { return _sc(SYS_DOAS, (long)password, 0, 0); }
static inline long chmod(const char *path, long mode) { return _sc(SYS_CHMOD, (long)path, mode, 0); }
static inline long drop_priv(void) { return _sc(SYS_DROP, 0, 0, 0); }
static inline long kill(long pid) { return _sc(SYS_KILL, pid, 0, 0); }
static inline long getcpu(void) { return _sc(SYS_GETCPU, 0, 0, 0); }
static inline long dmesg(char *buf, long start, long max) { return _sc(SYS_DMESG, (long)buf, start, max); }
#define DMESG_TOTAL 0x7FFFFFFF
static inline long net_info(unsigned int *out) { return _sc(SYS_NET_INFO, (long)out, 0, 0); }
static inline long udp_send(unsigned int ip, unsigned short port, const char *buf, long len) { return _sc4(SYS_UDP_SEND, (long)ip, port, (long)buf, len); }
static inline long udp_recv(char *buf, long cap) { return _sc(SYS_UDP_RECV, (long)buf, cap, 0); }
static inline long tcp_connect(unsigned int ip, unsigned short port) { return _sc(SYS_TCP_CONNECT, (long)ip, port, 0); }
static inline long tcp_send(long c, const char *buf, long len) { return _sc(SYS_TCP_SEND, c, (long)buf, len); }
static inline long tcp_recv(long c, char *buf, long cap) { return _sc(SYS_TCP_RECV, c, (long)buf, cap); }
static inline long tcp_close(long c) { return _sc(SYS_TCP_CLOSE, c, 0, 0); }
static inline long tcp_listen(unsigned short port) { return _sc(SYS_TCP_LISTEN, port, 0, 0); }
static inline long tcp_accept(long l) { return _sc(SYS_TCP_ACCEPT, l, 0, 0); }
static inline long dns_resolve(const char *name, unsigned int *out) { return _sc(SYS_DNS_RESOLVE, (long)name, (long)out, 0); }
static inline long fw_add(long proto, unsigned int dip, unsigned short dport, long drop) { return _sc4(SYS_NET_FW_ADD, proto, (long)dip, dport, drop); }
static inline long fw_clear(void) { return _sc(SYS_NET_FW_CLEAR, 0, 0, 0); }
static inline long udp_bind(unsigned short port) { return _sc(SYS_UDP_BIND, port, 0, 0); }
static inline long icmp_ping(unsigned int ip, unsigned long *rtt) { return _sc(SYS_ICMP_PING, (long)ip, (long)rtt, 0); }
static inline long icmp6_ping(const unsigned char *addr, unsigned long *rtt) { return _sc(SYS_ICMP6_PING, (long)addr, (long)rtt, 0); }
static inline long ipv6_info(unsigned char *addr, unsigned char *router) { return _sc(SYS_IPV6_INFO, (long)addr, (long)router, 0); }
static inline long tls_connect(unsigned int ip, unsigned short port) { return _sc(SYS_TLS_CONNECT, (long)ip, port, 0); }
static inline long tls_send(long h, const char *buf, long len) { return _sc(SYS_TLS_SEND, h, (long)buf, len); }
static inline long tls_recv(long h, char *buf, long cap) { return _sc(SYS_TLS_RECV, h, (long)buf, cap); }
static inline long tls_close(long h) { return _sc(SYS_TLS_CLOSE, h, 0, 0); }
static inline long tls_listen(unsigned short port) { return _sc(SYS_TLS_LISTEN, port, 0, 0); }
static inline long tls_accept(long l) { return _sc(SYS_TLS_ACCEPT, l, 0, 0); }
static inline long wifi_scan(void) { return _sc(SYS_WIFI_SCAN, 0,0,0); }
static inline long wifi_connect(const char *ssid, const char *psk) { return _sc(SYS_WIFI_CONNECT, (long)ssid, (long)psk, 0); }
static inline long wifi_disconnect(void) { return _sc(SYS_WIFI_DISCONNECT,0,0,0); }
static inline long wifi_status(char *out, long cap) { return _sc(SYS_WIFI_STATUS, (long)out, cap, 0); }
static inline long wm_move(unsigned id, int x, int y) { return _sc(SYS_WM_MOVE, id, x, y); }
static inline long wm_resize(unsigned id, unsigned w, unsigned h) { return _sc(SYS_WM_RESIZE, id, w, h); }
static inline long task_list(unsigned *pids, long max) { return _sc(SYS_TASK_LIST, (long)pids, max, 0); }
static inline long audio_vol(long v) { return _sc(SYS_AUDIO_VOL, v, 0, 0); }      /* v>=0 set & return old; v<0 get */
static inline long auth_verify(const char *pw) { return _sc(SYS_AUTH_VERIFY, (long)pw, 0, 0); }
static inline long notify(const char *m) { return _sc(SYS_NOTIFY, (long)m, 0, 0); }
static inline long notify_poll(char *buf, long cap) { return _sc(SYS_NOTIFY_POLL, (long)buf, cap, 0); }
static inline long battery(int *out) { return _sc(SYS_BATTERY, (long)out, 0, 0); }
static inline long clipboard_set(const char *text) { return _sc(SYS_CLIPBOARD_SET, (long)text, 0, 0); }
static inline long clipboard_get(char *out, long cap) { return _sc(SYS_CLIPBOARD_GET, (long)out, cap, 0); }
static inline long mouse_pos(int *out) { return _sc(SYS_MOUSE_POS, (long)out, 0, 0); }
static inline long passwd(const char *oldpw, const char *newpw) { return _sc(SYS_PASSWD, (long)oldpw, (long)newpw, 0); }
static inline long reboot(void) { return _sc(SYS_REBOOT, 0, 0, 0); }
static inline long dup2(long oldfd, long newfd) { return _sc(SYS_DUP2, oldfd, newfd, 0); }
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
static inline int strncmp(const char *a, const char *b, size_t n) { while (n && *a && *a == *b) { a++; b++; n--; } return n ? (int)(unsigned char)*a - (int)(unsigned char)*b : 0; }
static inline int strcmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (int)(unsigned char)*a - (int)(unsigned char)*b; }
static inline char *strncpy(char *d, const char *s, size_t n) { size_t i=0; while (s[i] && i<n){ d[i]=s[i]; i++; } while(i<n) d[i++]=0; return d; }
static inline int puts(const char *s) { klog(s); return 0; }
static inline void *memcpy(void *dst, const void *src, size_t n) { unsigned char *d=dst; const unsigned char *s=src; for (size_t i=0;i<n;i++) d[i]=s[i]; return dst; }
static inline void *memset(void *dst, int c, size_t n) { unsigned char *d=dst; for (size_t i=0;i<n;i++) d[i]=(unsigned char)c; return dst; }

/* compositor */
typedef struct { unsigned w, h, pitch, bpp, rgb; } fb_info_t;
typedef struct { int dx, dy, wheel; unsigned char buttons; } mouse_ev_t;
static inline void *fb_info(fb_info_t *i) { return (void *)(u64)_sc(SYS_FB_INFO, (long)i, 0, 0); }
static inline long fb_flip(void *p) { return _sc(SYS_FB_FLIP, (long)p, 0, 0); }
typedef struct { int x, y, w, h; } fb_rect_t;
static inline long fb_present(void *p, const fb_rect_t *rects, long count) { return _sc(SYS_FB_PRESENT, (long)p, (long)rects, count); }
static inline int  poll_key(void) { return (int)_sc(SYS_POLL_KEY, 0, 0, 0); }
static inline int  poll_mouse(mouse_ev_t *m) { return (int)_sc(SYS_POLL_MOUSE, (long)m, 0, 0); }
static inline long time_ms(void) { return _sc(SYS_TIME_MS, 0, 0, 0); }
static inline long wall_time(void) { return _sc(SYS_TIME, 0, 0, 0); }
static inline long sleep(long ms) { return _sc(SYS_SLEEP, ms, 0, 0); }
static inline long exec(const char *path, char *const argv[], char *const envp[]) { return _sc(SYS_EXEC, (long)path, (long)argv, (long)envp); }

/* window surfaces */
typedef struct {
    unsigned id, w, h;
    unsigned win_x, win_y;
    unsigned long long app_va;
    unsigned owner_pid;
    unsigned dirty;
    char title[32];
} wm_surf_info_t;
static inline long wm_create(unsigned w, unsigned h, wm_surf_info_t *o) { return _sc(SYS_WM_CREATE, w, h, (long)o); }
static inline long wm_flip(unsigned id) { return _sc(SYS_WM_FLIP, id, 0, 0); }
static inline long wm_scan(wm_surf_info_t *o, unsigned max) { return _sc(SYS_WM_SCAN, (long)o, max, 0); }
static inline long wm_focus(unsigned pid) { return _sc(SYS_WM_FOCUS, pid, 0, 0); }
static inline long wm_destroy(unsigned id) { return _sc(SYS_WM_DESTROY, id, 0, 0); }
static inline long wm_title(unsigned id, const char *t) { return _sc(SYS_WM_TITLE, id, (long)t, 0); }

#endif
