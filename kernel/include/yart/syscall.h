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
    SYS_MAX
};

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

void syscall_install(void);
