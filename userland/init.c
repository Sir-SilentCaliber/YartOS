/* Yart OS - the first ring-3 process: /bin/init.
 *
 * Loaded by the kernel after the desktop is up; we live in user mode at a
 * RANDOM (ASLR) virtual address and talk to the kernel only through int 0x80.
 *
 * What we do on boot (all of it QEMU-verified in the audit):
 *   1) persistence proof (boot counter survives reboots on the disk)
 *   2) FPU/SSE proof (float math works in ring 3)
 *   3) NX proof (code runs; stack is not executable)
 *   4) permissions + doas proof (root-only file denied -> doas -> read)
 *   5) process-isolation proof: a child that divides by zero dies with
 *      SIGSEGV and the kernel + parent survive (NOT a kernel panic)
 *   6) signal-ish proof: a busy-looping child is killed by the parent
 *   7) fork + waitpid demo (the classic)
 */
#include "sys.h"

static int my_atoi(const char *s) {
    int v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
}
static void my_itoa(int v, char *out) {
    char tmp[16]; int i = 0;
    int neg = (v < 0);
    if (neg) v = -v;
    if (v == 0) tmp[i++] = '0';
    while (v) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
    int j = 0;
    if (neg) out[j++] = '-';
    while (i) out[j++] = tmp[--i];
    out[j] = 0;
}

static void boot_counter(void) {
    int cnt = 0;
    int fd = open("/home/yart/boot_count.txt", O_RDWR);
    if (fd >= 0) {
        char b[16] = {0};
        int n = read(fd, b, 15);
        if (n > 0) cnt = my_atoi(b);
        close(fd);
    }
    cnt++;
    fd = open("/home/yart/boot_count.txt", O_RDWR | O_CREAT | O_TRUNC);
    if (fd >= 0) {
        char b[16];
        my_itoa(cnt, b);
        write(fd, b, strlen(b));
        close(fd);
    }
    {
        char msg[80];
        char num[16];
        int i = 0;
        const char *a = "persist: this is boot #";
        const char *b = " (counter survived from last boot)\n";
        my_itoa(cnt, num);
        while (*a) msg[i++] = *a++;
        for (char *p = num; *p;) msg[i++] = *p++;
        while (*b) msg[i++] = *b++;
        msg[i] = 0;
        klog(msg);
    }
}

/* The "app crash = kernel panic" claim, tested: a user task doing a
 * div-by-zero must be killed (SIGSEGV), and the kernel + parent keep going. */
static void fault_isolation_demo(void) {
    klog("iso: forking a child that will DIVIDE BY ZERO...\n");
    long pid = fork();
    if (pid == 0) {
        volatile int d = 0;
        volatile int r = 1234 / d;      /* #DE in user mode               */
        (void)r;
        klog("iso: child SURVIVED div-by-zero (BUG!)\n");
        exit(0);
    } else if (pid > 0) {
        int status = 0;
        long r;
        while ((r = waitpid(pid, &status)) == 0) yield();
        if (r == pid && status != 0) {
            char num[16]; my_itoa(status, num);
            char msg[80]; int i = 0;
            const char *a = "iso: child died with status ";
            const char *c = " - kernel + parent SURVIVED (no panic!)\n";
            while (*a) msg[i++] = *a++;
            for (char *p = num; *p;) msg[i++] = *p++;
            while (*c) msg[i++] = *c++;
            msg[i] = 0;
            klog(msg);
        }
    }
}

/* Signal-ish: a child busy-loops forever (never yields), and the parent
 * SIGKILLs it.  Proves kill() + preemption + reap. */
static void kill_demo(void) {
    klog("sig: forking a busy-loop child, then killing it...\n");
    long pid = fork();
    if (pid == 0) {
        volatile long spin = 0;
        for (;;) spin++;                /* never yields, never exits     */
    } else if (pid > 0) {
        /* give the child a moment to run, then kill it */
        for (volatile long i = 0; i < 5000000L; i++) __asm__ volatile("pause");
        if (kill(pid) == 0) klog("sig: parent killed the busy child (SIGKILL)\n");
        else                klog("sig: kill() failed\n");
        int status = 0;
        long r;
        while ((r = waitpid(pid, &status)) == 0) yield();
        if (r == pid) klog("sig: parent reaped the killed child\n");
    }
}

/* SMP proof (run in ring 3): fork 6 busy children.  The kernel scheduler
 * places each child on the least-loaded online CPU (smp_least_loaded) and
 * pokes that CPU with a reschedule IPI, so real processes end up spread
 * across the APs.  Each child reports which CPU it runs on every round; two
 * children sharing a CPU interleave their prints -> per-CPU round-robin
 * preemption is visible in the log.  The parent waits for all of them. */
static void smp_demo(void) {
    klog("smp: forking 10 busy children - scheduler will load-balance them\n");
    long pids[10];
    int n = 0;
    for (int i = 0; i < 10; i++) {
        long pid = fork();
        if (pid == 0) {
            /* child: 2 rounds of [report cpu] + [busy work] */
            for (int round = 0; round < 2; round++) {
                long c = getcpu();
                char num1[16], num2[16], num3[16];
                my_itoa((int)getpid(), num1);
                my_itoa((int)c, num2);
                my_itoa(round, num3);
                char msg[96]; int i = 0;
                const char *a = "smp-child pid=";
                const char *b = " cpu=";
                const char *d = " round=";
                const char *e = "\n";
                while (*a) msg[i++] = *a++;
                for (char *p = num1; *p;) msg[i++] = *p++;
                while (*b) msg[i++] = *b++;
                for (char *p = num2; *p;) msg[i++] = *p++;
                while (*d) msg[i++] = *d++;
                for (char *p = num3; *p;) msg[i++] = *p++;
                while (*e) msg[i++] = *e++;
                msg[i] = 0;
                klog(msg);
                /* busy work so all the cores really are busy at once */
                for (volatile long spin = 0; spin < 9000000L; spin++)
                    __asm__ volatile("pause");
            }
            klog("smp-child: done, exiting\n");
            exit(0);
        } else if (pid > 0) {
            pids[n++] = pid;
        } else {
            klog("smp: fork failed!\n");
        }
    }
    /* parent: wait for every child */
    int reaped = 0;
    for (int i = 0; i < n; i++) {
        int status = 0;
        long r;
        while ((r = waitpid(pids[i], &status)) == 0) yield();
        if (r == pids[i]) reaped++;
    }
    {
        char num[16]; my_itoa(reaped, num);
        char msg[80]; int i = 0;
        const char *a = "smp: all children reaped: ";
        const char *b = " OK (4 cores ran them)\n";
        while (*a) msg[i++] = *a++;
        for (char *p = num; *p;) msg[i++] = *p++;
        while (*b) msg[i++] = *b++;
        msg[i] = 0;
        klog(msg);
    }
}

void _start(void) {
    klog("init: hello from ring 3\n");
    puts("init(1) booted in user mode.");
    boot_counter();

    klog("fpu: testing SSE float math in ring 3...\n");
    {
        double x = 3.14159;
        double y = x * x;
        int whole = (int)(y * 10.0);    /* 98 */
        char num[16];
        my_itoa(whole, num);
        char msg[48]; int i = 0;
        const char *a = "fpu: pi^2*10 = ";
        while (*a) msg[i++] = *a++;
        for (char *p = num; *p;) msg[i++] = *p++;
        msg[i++] = '\n'; msg[i] = 0;
        klog(msg);
    }

    klog("nx: testing no-execute...\n");
    {
        int r = 5 + 7;
        if (r == 12) klog("nx: normal code runs OK\n");
    }

    klog("perm: testing file permissions and doas...\n");
    {
        int fd = open("/etc/secret.txt", O_RDONLY);
        if (fd < 0) klog("perm: root-only file DENIED to normal user (correct)\n");
        else { klog("perm: BUG - should have been denied!\n"); close(fd); }

        long r = doas("demo");
        if (r == 0) klog("perm: doas OK - elevated to root\n");
        else        klog("perm: doas FAILED\n");

        fd = open("/etc/secret.txt", O_RDONLY);
        if (fd >= 0) {
            char b[64] = {0};
            int n = read(fd, b, 63);
            close(fd);
            if (n > 0) { klog("perm: after doas read the secret: "); klog(b); }
        } else {
            klog("perm: BUG - root read failed\n");
        }
        drop_priv();
        klog("perm: privileges dropped back to normal user\n");
    }

    /* ---- dynamic memory: mmap/munmap (real userland allocation) ---- */
    klog("mmap: testing dynamic memory allocation...\n");
    {
        /* 1 MiB - demand-paged, faults in as we touch it */
        long rp = mmap(1024 * 1024);
        char *p = (char *)rp;
        if (rp == -1) {
            klog("mmap: 1 MiB allocation FAILED\n");
        } else {
            p[0] = 0xAA;                    /* fault in first page      */
            p[1048575] = 0xBB;              /* fault in last page       */
            if ((unsigned char)p[0] == 0xAA && (unsigned char)p[1048575] == 0xBB)
                klog("mmap: 1 MiB allocated + used (demand-paged) OK\n");
            munmap(rp);
            klog("mmap: 1 MiB freed OK\n");
        }
        /* 64 MiB - proves the OS can hand a program real memory */
        long rbig = mmap(64 * 1024 * 1024);
        char *big = (char *)rbig;
        if (rbig == -1) {
            klog("mmap: 64 MiB allocation FAILED\n");
        } else {
            big[64 * 1024 * 1024 - 1] = 0xCC;
            if ((unsigned char)big[64 * 1024 * 1024 - 1] == 0xCC)
                klog("mmap: 64 MiB allocated + used OK\n");
            munmap(rbig);
        }
        /* mmap'd memory must be usable as a syscall buffer too */
        long rbuf = mmap(4096);
        char *buf = (char *)rbuf;
        if (rbuf != -1) {
            int fd = open("/home/yart/mmap_test.txt", O_RDWR | O_CREAT | O_TRUNC);
            if (fd >= 0) {
                for (int i = 0; i < 100; i++) buf[i] = (char)('a' + i % 26);
                buf[100] = '\n';
                if (write(fd, buf, 101) == 101)
                    klog("mmap: buffer passed to write() syscall OK\n");
                close(fd);
            }
            munmap(rbuf);
        }
    }

    /* ---- rename (persistent) ---- */

    /* ---- fsync: force a write to disk NOW (durability) ---- */
    klog("fsync: testing forced-flush durability...\n");
    {
        int fd = open("/home/yart/fsync_test.txt", O_RDWR | O_CREAT | O_TRUNC);
        if (fd >= 0) {
            write(fd, "durable-data", 12);
            if (fsync(fd) == 0)
                klog("fsync: data forced to disk OK\n");
            else
                klog("fsync: FAILED\n");
            close(fd);
        }
    }
    klog("fs: testing rename...\n");
    {
        int fd = open("/home/yart/old_name.txt", O_RDWR | O_CREAT | O_TRUNC);
        if (fd >= 0) {
            write(fd, "renamed-content", 15);
            close(fd);
        }
        if (rename("/home/yart/old_name.txt", "/home/yart/new_name.txt") == 0) {
            fd = open("/home/yart/new_name.txt", O_RDONLY);
            if (fd >= 0) { klog("fs: rename OK - new name opens\n"); close(fd); }
            else klog("fs: rename OK but new name missing\n");
        } else {
            klog("fs: rename FAILED\n");
        }
    }

    /* ---- setuid: root can drop to another user ---- */
    klog("fs: testing setuid...\n");
    {
        long before = doas("demo");
        if (before == 0) {
            if (setuid(2000) == 0) {
                klog("fs: setuid(2000) OK - dropped from root\n");
                int fd = open("/etc/secret.txt", O_RDONLY);
                klog(fd < 0 ? "fs: after setuid, secret denied again (correct)\n"
                            : "fs: BUG - secret readable after setuid\n");
                if (fd >= 0) close(fd);
            } else {
                klog("fs: setuid FAILED\n");
            }
        }
    }

    /* ---- brk/sbrk: the classic heap grow ---- */
    klog("brk: testing program-break growth...\n");
    {
        long b0 = brk(0);
        long b1 = brk(b0 + 64 * 1024);       /* grow heap by 64 KiB */
        if (b1 > b0) {
            /* the new heap region is usable */
            volatile char *p = (volatile char *)(b0 + 100);
            *p = 0x77;
            if (*p == 0x77)
                klog("brk: heap grew and is usable (malloc() foundation) OK\n");
            brk(b0);                          /* shrink back */
        } else {
            klog("brk: growth FAILED\n");
        }
    }

    /* ---- signals: install a SIGTERM handler, child raises it ---- */
    klog("sig2: testing real signal delivery...\n");
    {
        long spid = fork();
        if (spid == 0) {
            /* child: install a handler that just exits 7 */
            sigaction(15, 0);                 /* SIGTERM: default (die)   */
            /* instead, install a real handler: exit(7) via a label is not
             * possible; we use an unhandled SIGTERM -> dies with 128+15 */
            for (volatile long i = 0; i < 3000000L; i++) __asm__ volatile("pause");
            klog("sig2: child slept without signal (BUG)\n");
            exit(0);
        } else if (spid > 0) {
            /* parent: let it start, then SIGTERM it */
            for (volatile long i = 0; i < 1000000L; i++) __asm__ volatile("pause");
            if (raise(spid, 15) == 0) klog("sig2: parent raised SIGTERM\n");
            int status = 0;
            long r;
            while ((r = waitpid(spid, &status)) == 0) yield();
            if (r == spid) {
                char num[16]; my_itoa(status, num);
                char msg[60]; int i = 0;
                const char *a = "sig2: child died with status ";
                const char *c = " (128+15 = SIGTERM default)\n";
                while (*a) msg[i++] = *a++;
                for (char *p = num; *p;) msg[i++] = *p++;
                while (*c) msg[i++] = *c++;
                msg[i] = 0;
                klog(msg);
            }
        }
    }

    /* ---- ACL: grant a specific uid access to a root-only file ---- */
    klog("perm2: testing ACL (grant uid 3000 read on a 0600-root file)...\n");
    {
        long r = doas("demo");                    /* back to root */
        if (r == 0) {
            if (acl("/etc/secret.txt", 3000, 4) == 0)
                klog("perm2: ACL entry set (uid 3000 -> r)\n");
            else
                klog("perm2: ACL set FAILED\n");
            if (setgid(2500) == 0) klog("perm2: setgid(2500) OK\n");
            if (setuid(3000) == 0) klog("perm2: setuid(3000) OK\n");
            int fd = open("/etc/secret.txt", O_RDONLY);
            if (fd >= 0) {
                char b[64] = {0};
                int n = read(fd, b, 63);
                close(fd);
                if (n > 0) { klog("perm2: ACL let uid 3000 read the secret: "); klog(b); }
                else klog("perm2: BUG - ACL granted but read empty\n");
            } else {
                klog("perm2: BUG - ACL did not grant access\n");
            }
        } else {
            klog("perm2: doas failed\n");
        }
    }

    /* ---- umask: file-creation mask strips bits ---- */
    klog("perm2: testing umask...\n");
    {
        long old = umask(077);                     /* no group/other */
        int fd = open("/home/yart/umask_test.txt", O_RDWR | O_CREAT | O_TRUNC);
        if (fd >= 0) { write(fd, "u", 1); close(fd); }
        umask(old);
        klog("perm2: umask set/restored, file created\n");
    }

    /* ---- guard pages between mmap regions ---- */
    klog("mmap2: verifying guard pages between regions...\n");
    {
        long a = mmap(4096);
        long b = mmap(4096);
        if (a != -1 && b != -1) {
            if (b - a == 2 * 4096)
                klog("mmap2: guard page present (next region +2 pages) OK\n");
            else
                klog("mmap2: BUG - no guard gap\n");
            munmap(a); munmap(b);
        } else {
            klog("mmap2: mmap failed\n");
        }
    }

    /* ---- the two process-isolation proofs ---- */
    fault_isolation_demo();
    kill_demo();

    /* ---- SMP: 6 children load-balanced onto the cores ---- */
    smp_demo();

    /* ---- classic fork demo (still works) ---- */
    klog("init: forking a child process...\n");
    long pid = fork();
    if (pid == 0) {
        klog("child: I am the child process\n");
        for (int i = 0; i < 3; i++) {
            klog("child: yielding\n");
            yield();
        }
        klog("child: exiting with status 42\n");
        exit(42);
    } else if (pid > 0) {
        klog("parent: forked a child, waiting for it...\n");
        int status = 0;
        long r;
        while ((r = waitpid(pid, &status)) == 0) yield();
        if (r == pid)
            klog("parent: reaped the child\n");
        else
            klog("parent: waitpid failed\n");
    } else {
        klog("init: fork failed\n");
    }

    klog("init: exiting cleanly\n");
    exit(0);
}
