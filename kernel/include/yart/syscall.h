#pragma once
#include <yart/types.h>
#include <yart/drivers.h>    /* mouse_event_t */

/* Yart syscall numbers - kept small + linux-ish.  Used by both the kernel
 * dispatcher (kernel/arch/x86_64/syscall.c) and the userland libc
 * (initrd_root/usr/include/yart/sys.h). */
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
    SYS_KLOG     = 16,    /* write to kernel serial */
    SYS_FORK     = 17,    /* clone the calling process (CoW) */
    SYS_WAITPID  = 18,    /* reap a child (0 = still running, -1 = none) */
    SYS_DOAS     = 19,    /* elevate euid to 0 after password auth (admin) */
    SYS_CHMOD    = 20,    /* change a file's permission bits (owner/root)  */
    SYS_DROP     = 21,    /* drop privileges: euid = uid                   */
    SYS_KILL     = 22,    /* SIGKILL another process (minimal signal)      */
    SYS_MMAP     = 23,    /* reserve dynamic user memory (demand-paged)    */
    SYS_MUNMAP   = 24,    /* release it back                              */
    SYS_SETUID   = 25,    /* change uid/gid (root only)                      */
    SYS_RENAME   = 26,    /* rename/move a file or directory                  */
    SYS_BRK      = 27,    /* grow/shrink the program break (sbrk)                 */
    SYS_SIGACTION = 28,  /* install a signal handler                        */
    SYS_RAISE    = 29,    /* send a signal to another process                */
    SYS_FSYNC    = 30,    /* force dirty files to disk now (fsync)               */
    SYS_SETGID   = 31,    /* change gid (root only)                             */
    SYS_UMASK    = 32,    /* set file-creation mask, returns old                 */
    SYS_ACL      = 33,    /* set/clear an ACL entry on a file (owner/root)       */
    SYS_GETCPU   = 34,    /* which CPU am I running on (SMP)                     */
    SYS_DMESG    = 35,    /* read the kernel audit/dmesg log (ring 3)            */
    SYS_NET_INFO = 36,    /* read the assigned IP/gw/dns/mask (ring 3)            */
    SYS_UDP_SEND = 37,    /* send a UDP datagram                                   */
    SYS_UDP_RECV = 38,    /* poll a received UDP datagram                          */
    /* --- ring-3 compositor / display server syscalls (row 23) --- */
    SYS_FB_INFO  = 39,    /* query framebuffer geometry; mmap FB pages             */
    SYS_FB_FLIP  = 40,    /* copy the user's rendered buffer to the real scanout   */
    SYS_POLL_KEY = 41,    /* dequeue one keyboard event (0 = none)                 */
    SYS_POLL_MOUSE = 42,  /* dequeue one mouse event (dx,dy,buttons,wheel)         */
    SYS_TIME_MS  = 43,    /* monotonic millisecond uptime (for compositor fps)     */
    SYS_SLEEP    = 44,    /* block the calling task for ms (timer wakeup)          */
    SYS_EXEC     = 45,    /* replace the address space: exec(path, argv, envp)     */
    SYS_WM_CREATE = 46,   /* app: create a window surface (maps into wm too)       */
    SYS_WM_FLIP   = 47,   /* app: mark a surface dirty (needs recomposite)         */
    SYS_WM_SCAN   = 48,   /* wm:  list surfaces + clear dirty flags                */
    SYS_WM_FOCUS  = 49,   /* wm:  route keyboard to a task (0 = none)              */
    SYS_WM_DESTROY = 50,  /* app/wm: destroy a surface                              */
    SYS_SIGRETURN  = 51,  /* restore a frame saved by signal delivery               */
    SYS_WM_TITLE   = 52,  /* app: set the window title (drawn by the compositor)    */
    SYS_PIPE       = 53,  /* pipe(fds[2]): create an in-kernel byte pipe             */
    SYS_TCP_CONNECT = 54, /* TCP: connect (blocking handshake) to ip:port             */
    SYS_TCP_SEND    = 55, /* TCP: send bytes on a connection                           */
    SYS_TCP_RECV    = 56, /* TCP: receive available bytes (0 = none)                   */
    SYS_TCP_CLOSE   = 57, /* TCP: graceful close (FIN/ACK)                             */
    SYS_TCP_LISTEN  = 58, /* TCP: listen on a port (server)                            */
    SYS_TCP_ACCEPT  = 59, /* TCP: accept a completed connection (-2 = not yet)         */
    SYS_DNS_RESOLVE = 60, /* DNS: resolve a hostname to an IPv4 (blocking)               */
    SYS_NET_FW_ADD  = 61, /* firewall: add a rule (proto, dip, dport, drop)                */
    SYS_NET_FW_CLEAR = 62,/* firewall: clear all rules                                     */
    SYS_UDP_BIND    = 63, /* UDP: bind the socket to a local port                          */
    SYS_ICMP_PING   = 64, /* ICMP: echo request, blocking; RTT in ticks out                */
    SYS_ICMP6_PING  = 65, /* ICMPv6: ping an IPv6 address (16 bytes), blocking             */
    SYS_IPV6_INFO   = 66, /* IPv6: copy our address + router (16 bytes each)               */
    SYS_TLS_CONNECT = 67, /* TLS 1.2: connect (handshake) to ip:port, return conn id        */
    SYS_TLS_SEND    = 68, /* TLS: send plaintext over the encrypted stream                   */
    SYS_TLS_RECV    = 69, /* TLS: receive plaintext                                          */
    SYS_TLS_CLOSE   = 70, /* TLS: close_notify + close the connection                        */
    SYS_TLS_LISTEN  = 71, /* TLS server: listen on a port                                       */
    SYS_TLS_ACCEPT  = 72, /* TLS server: accept + server-side handshake (-2 = not yet)          */
    SYS_MAX
};

void syscall_install(void);
void syscall_install_percpu(void);
void doas_init(void);          /* seed the salted-SHA-256 user database */

/* open() flags */
#define O_RDONLY  0x0
#define O_WRONLY  0x1
#define O_RDWR    0x2
#define O_CREAT   0x40
#define O_TRUNC   0x200
#define O_APPEND  0x400

/* getdents entry layout */
typedef struct {
    u32  type;            /* 1 = file, 2 = dir                      */
    u32  reclen;
    u64  size;
    char name[96];
} yart_dirent_t;

/* stat result */
typedef struct {
    u32  type;
    u32  mode;
    u64  size;
    u64  mtime;
} yart_stat_t;

/* Window-surface info (SYS_WM_CREATE/SCAN).  app_va is the caller-side
 * mapping for CREATE and the WM-side mapping for SCAN (same layout, the
 * kernel picks the side). */
typedef struct {
    u32  id;
    u32  w, h;
    u32  win_x, win_y;     /* screen position the compositor uses       */
    u64  app_va;           /* mapped canvas address (side depends)      */
    u32  owner_pid;
    u32  dirty;
    char title[32];        /* window title (drawn by the compositor)    */
} wm_surf_info_t;

void doas_init(void);          /* seed the salted-SHA-256 user database */

/* Input fanout (called from the PS/2 + USB drivers, IRQ context) */
void sys_input_kbd(int ev);
void sys_input_mouse(const mouse_event_t *me);

/* Called by the scheduler when a task is reaped: free its surfaces. */
void wm_surface_owner_died(u32 pid);

/* Framebuffer info returned by SYS_FB_INFO.  After a successful call the
 * caller's address space has `size` bytes mapped starting at `addr` (which
 * is returned) as a 32bpp ARGB/RGBx back buffer that the compositor writes.
 * The framebuffer is double-buffered: the ring-3 wm renders into its
 * mmap'd region then calls SYS_FB_FLIP to blit it to the real scanout. */
typedef struct {
    u32 width, height, pitch, bpp, rgb;
} fb_info_t;

/* Mouse event returned by SYS_POLL_MOUSE. */
typedef struct {
    int dx, dy, wheel;
    u8  buttons;   /* bit0=left, bit1=right, bit2=middle */
} mouse_ev_t;

void syscall_install(void);
void syscall_install_percpu(void);   /* per-CPU MSR setup (BSP + each AP) */
