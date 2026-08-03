#pragma once
#include <yart/types.h>

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
