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
