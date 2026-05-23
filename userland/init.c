/* Yart OS - the first ring-3 process: /bin/init.
 *
 * Loaded by the kernel's ELF loader after the desktop is up; we live in
 * user mode at virtual address 0x40000000.  We talk to the kernel only
 * through int 0x80 (see userland/sys.h).
 *
 * What we do:
 *   1) klog so the serial log proves we are in ring 3
 *   2) write a banner to stdout (kernel kputs)
 *   3) create /home/yart/INIT_RAN.txt so the desktop file manager can
 *      see proof of life
 *   4) loop forever, yielding so the desktop keeps refreshing
 */
#include "sys.h"

void _start(void) {
    klog("init: hello from ring 3, pid=1\n");
    puts("init(1) booted in user mode.");

    int fd = open("/home/yart/INIT_RAN.txt", O_RDWR | O_CREAT | O_TRUNC);
    if (fd >= 0) {
        const char *msg =
            "This file was written by /bin/init running in ring 3.\n"
            "It proves the syscall path works end-to-end:\n"
            "  user -> int 0x80 -> kernel dispatcher -> VFS -> here.\n";
        write(fd, msg, strlen(msg));
        close(fd);
        klog("init: wrote /home/yart/INIT_RAN.txt\n");
    } else {
        klog("init: open failed\n");
    }

    klog("init: exiting cleanly\n");
    exit(0);
}
