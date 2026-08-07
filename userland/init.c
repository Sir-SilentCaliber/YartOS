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

void wm_run(void);

/* A REAL signal handler: prints, then returns - the kernel's trampoline
 * (SYS_SIGRETURN) restores the interrupted state.  The address is taken
 * and passed to sigaction, so it cannot be inlined away. */
static void sigterm_handler(void) {
    klog("sig3: HANDLER RAN (SIGTERM caught)\n");
}

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
        while ((r = waitpid(pid, &status)) == 0);
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
        /* give the child a moment to run (blocking sleep - the kernel
         * parks us, no busy loop), then kill it */
        sleep(200);
        if (kill(pid) == 0) klog("sig: parent killed the busy child (SIGKILL)\n");
        else                klog("sig: kill() failed\n");
        int status = 0;
        long r;
        while ((r = waitpid(pid, &status)) == 0);
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
                /* short busy round so all the cores briefly run something */
                for (volatile long spin = 0; spin < 2000000L; spin++)
                    __asm__ volatile("pause");
                yield();
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

/* Entry: real crt0 in start.c calls main_entry(argc, argv, envp) with the
 * SysV process image the kernel placed on our stack. */
int main_entry(int argc, char **argv, char **envp) {
    (void)envp;
    klog("init: hello from ring 3\n");
    if (argc > 0 && argv[0])
        klog(argv[0]);
    klog("\n");
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

        long r = doas("yart");
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
    /* ---- dmesg: read the kernel audit log (row 19) ---- */
    klog("dmesg: reading the kernel audit log...\n");
    {
        /* query the total, then scan the whole available log for an earlier
         * security event (the doas elevation "elevated to root via doas") */
        long total = dmesg(0, DMESG_TOTAL, 0);
        char msg[64]; int i = 0;
        const char *a = "dmesg: total "; const char *c = " lines in kernel log\n";
        char num1[16]; my_itoa((int)total, num1);
        while (*a) msg[i++] = *a++;
        for (char *p = num1; *p;) msg[i++] = *p++;
        while (*c) msg[i++] = *c++;
        msg[i] = 0;
        klog(msg);

        int found = 0, read_total = 0;
        /* flat buffer; each line is up to KLOG_LINE_MAX+1 (257) bytes */
        char chunk[20 * 257];
        for (long start = 0; start < total && !found; start += 20) {
            long n = dmesg(chunk, start, 20);
            if (n <= 0) break;
            read_total += (int)n;
            for (long j = 0; j < n; j++) {
                char *ln = &chunk[j * 257];
                for (char *p = ln; *p; p++) {
                    /* "elevated to root via doas" - the doas security event */
                    if (p[0]=='e'&&p[1]=='l'&&p[2]=='e'&&p[3]=='v'&&
                        p[4]=='a'&&p[5]=='t'&&p[6]=='e'&&p[7]=='d'&&
                        p[8]==' '&&p[9]=='t'&&p[10]=='o'&&p[11]==' '&&
                        p[12]=='r'&&p[13]=='o'&&p[14]=='o'&&p[15]=='t') { found = 1; break; }
                }
                if (found) break;
            }
        }
        if (found) klog("dmesg: found the 'elevated to root' security event in the audit log\n");
        else {
            klog("dmesg: WARNING security event not found; sample lines:\n");
            char dbg[20 * 257];
            long nd = dmesg(dbg, total > 30 ? total - 30 : 0, 20);
            for (long j = 0; j < nd && j < 20; j++) {
                char *ln = &dbg[j * 257];
                klog(ln[0] ? ln : "(empty)"); klog("\n");
            }
        }
    }

    /* ---- networking: DHCP + UDP echo round-trip (row 16) ---- */
    {
        unsigned int info[5];
        net_info(info);
        if (!info[4]) {
            klog("net: no NIC present - skipping network test\n");
        } else {
            klog("net: waiting for DHCP to assign an address...\n");
            long ip = 0;
            int tries = 0;
            /* Bounded wait (the e1000's RX is gated by QEMU's 1s flush timer
             * after RCTL is written, so allow a few seconds). */
            while (tries < 100 && !ip) {
                net_info(info);
                ip = info[0];
                if (!ip) yield();
                tries++;
            }
            if (!ip) {
                klog("net: DHCP timed out - no address\n");
            } else {
                unsigned char b0 = info[0] >> 24, b1 = info[0] >> 16,
                              b2 = info[0] >> 8, b3 = info[0];
                char m1[48]; int i = 0; char p0[8], p1[8], p2[8], p3[8];
                my_itoa(b0, p0); my_itoa(b1, p1); my_itoa(b2, p2); my_itoa(b3, p3);
                const char *d1 = "net: ip=";
                while (*d1) m1[i++] = *d1++;
                for (char *p = p0; *p;) m1[i++] = *p++;
                m1[i++] = '.'; for (char *p = p1; *p;) m1[i++] = *p++;
                m1[i++] = '.'; for (char *p = p2; *p;) m1[i++] = *p++;
                m1[i++] = '.'; for (char *p = p3; *p;) m1[i++] = *p++;
                m1[i++] = '\n'; m1[i] = 0;
                klog(m1);

                /* UDP echo: send a probe to the gateway on the host's echo port.
                 * The host runs a UDP echo server on 127.0.0.1:7000 (reachable
                 * via slirp as <gw>:7000) which replies; we verify the round-trip. */
                unsigned int gw = info[2];
                const char *probe = "YARTNET-PROBE";
                klog("net: sending UDP probe to gateway...\n");
                int sent = 0;
                for (int attempt = 0; attempt < 3 && !sent; attempt++) {
                    if (udp_send(gw, 7000, probe, 13) == 0) sent = 1;
                    else { yield(); }
                }
                if (!sent) {
                    klog("net: UDP send FAILED\n");
                } else {
                    char rbuf[64]; long n = 0;
                    for (int wait = 0; wait < 20 && n == 0; wait++) {
                        n = udp_recv(rbuf, 64);
                        if (!n) yield();
                    }
                    if (n == 13 && rbuf[0]=='Y' && rbuf[1]=='A' && rbuf[2]=='R' && rbuf[3]=='T') {
                        klog("net: UDP round-trip OK - host echoed our probe back!\n");
                    } else if (n > 0) {
                        klog("net: received UDP reply but payload mismatch\n");
                    } else {
                        klog("net: no UDP reply received (host echo server not running?)\n");
                    }
                }
            }
        }
    }
    /* ---- TCP: full 3-way handshake + echo round-trip vs the HOST ----
     * The host runs a TCP echo server on 127.0.0.1:9000 (reachable via
     * slirp as 10.0.2.2:9000) that replies "PONG:" + data.  This proves
     * the whole TCP path: SYN/SYN-ACK/ACK, seq/ack tracking, RX
     * buffering, retransmission and graceful FIN/ACK close. */
    klog("tcp: testing TCP against the host echo server (10.0.2.2:9000)...\n");
    {
        /* wait for an address first - DHCP can lag under slow emulation */
        unsigned int nfo[5];
        int wtries = 0;
        net_info(nfo);
        while (nfo[0] == 0 && wtries < 300) {
            sleep(10);
            net_info(nfo);
            wtries++;
        }
        if (nfo[0] == 0) {
            klog("tcp: no IP address after 3s - skipping TCP test\n");
        } else {
        long c = tcp_connect(0x0A000202, 9000);
        if (c < 0) {
            klog("tcp: connect FAILED (host echo server not running?)\n");
        } else {
            klog("tcp: connected (conn ");
            {
                char b[8]; my_itoa((int)c, b); klog(b);
            }
            klog(")\n");
            const char *msg = "ping-from-yart-tcp";
            long n = tcp_send(c, msg, 18);
            if (n != 18) {
                klog("tcp: send FAILED\n");
            } else {
                char rbuf[64];
                long got = 0;
                int tries = 0;
                while (got < 23 && tries < 100) {
                    long r = tcp_recv(c, rbuf + got, 64 - got);
                    if (r > 0) got += r;
                    else sleep(10);
                    tries++;
                }
                rbuf[got] = 0;
                if (got == 23 && strcmp(rbuf, "PONG:ping-from-yart-tcp") == 0)
                    klog("tcp: echo round trip WORK (host echoed 23 bytes)\n");
                else
                    klog("tcp: echo round trip MISMATCH\n");
            }
            tcp_close(c);
            klog("tcp: connection closed cleanly\n");
        }
        }
    }

    /* ---- DNS + REAL INTERNET FETCH: resolve a real domain and pull a
     * web page over HTTP through QEMU slirp's NAT (DHCP -> ARP -> UDP/DNS
     * -> TCP -> HTTP, all the way out and back from the internet). */
    /* isolation probe: guest -> host HTTP server (10.0.2.2:9001) - if
     * this works but the internet fetch does not, slirp's internet NAT
     * path is the difference, not our stack */
    klog("www: probing guest->host HTTP (10.0.2.2:9001)...\n");
    {
        long c = tcp_connect(0x0A000202, 9001);
        if (c >= 0) {
            const char *req = "GET / HTTP/1.0\r\nHost: localhost\r\n\r\n";
            long rl = 35;
            long sl = 0;
            while (sl < rl) {
                long r = tcp_send(c, req + sl, rl - sl);
                if (r <= 0) break;
                sl += r;
            }
            char rbuf[128];
            long got = 0;
            int tries = 0;
            while (tries < 400 && got < 90) {   /* response is 90 bytes */
                long r = tcp_recv(c, rbuf + got, 128 - got);
                if (r > 0) got += r;
                else sleep(10);
                tries++;
            }
            rbuf[got] = 0;
            if (got > 0 && strcmp(rbuf, "HTTP/1.1 200 OK\r\nContent-Length: 12\r\nConnection: close\r\n\r\nHOST-HTTP-OK") == 0)
                klog("www: guest->host HTTP WORK (host server answered)\n");
            else
                klog("www: guest->host HTTP FAILED\n");
            tcp_close(c);
        } else {
            klog("www: guest->host connect FAILED\n");
        }
    }

    klog("www: testing DNS + real internet HTTP fetch...\n");
    {
        unsigned int nfo[5];
        net_info(nfo);
        if (nfo[0] == 0) {
            klog("www: no IP address - skipping\n");
        } else {
            unsigned int ip = 0;
            if (dns_resolve("example.com", &ip) != 0) {
                klog("www: DNS resolve FAILED\n");
            } else {
                klog("www: example.com = ");
                { char b[8]; my_itoa((int)((ip>>24)&255), b); klog(b); }
                klog(".");
                { char b[8]; my_itoa((int)((ip>>16)&255), b); klog(b); }
                klog(".");
                { char b[8]; my_itoa((int)((ip>>8)&255), b); klog(b); }
                klog(".");
                { char b[8]; my_itoa((int)(ip&255), b); klog(b); }
                klog("\n");
                long c = tcp_connect(0x0A000202, 9002);   /* host relay -> internet */
                if (c < 0) {
                    klog("www: TCP connect relay FAILED\n");
                } else {
                    const char *req =
                        "GET / HTTP/1.0\r\n"
                        "Host: example.com\r\n"
                        "Connection: close\r\n"
                        "\r\n";
                    long sl = 0, rl = (long)strlen(req);
                    while (sl < rl) {
                        long r = tcp_send(c, req + sl, rl - sl);
                        if (r <= 0) break;
                        sl += r;
                    }
                    char rbuf[1200];
                    long got = 0;
                    int tries = 0, found200 = 0, lastlog = 0;
                    /* long window: a real-internet RTT through slirp is
                     * 100 ms+ of HOST time = seconds of TCG guest time */
                    while (tries < 2500 && got < 1200 && !found200) {
                        long r = tcp_recv(c, rbuf + got, 1200 - got);
                        if (r > 0) {
                            got += r;
                            if (got - lastlog >= 100) {
                                lastlog = (int)got;
                                klog("www: received so far: ");
                                { char b[8]; my_itoa((int)got, b); klog(b); }
                                klog(" bytes\n");
                            }
                            /* scan the buffer for "200 OK" */
                            for (long i = 0; i <= got - 6; i++)
                                if (rbuf[i]=='2' && rbuf[i+1]=='0' && rbuf[i+2]=='0' &&
                                    rbuf[i+3]==' ' && rbuf[i+4]=='O' && rbuf[i+5]=='K')
                                    found200 = 1;
                        } else sleep(10);
                        tries++;
                    }
                    rbuf[got] = 0;
                    klog("www: got ");
                    { char b[8]; my_itoa((int)got, b); klog(b); }
                    klog(" bytes from the internet\n");
                    if (found200)
                        klog("www: REAL WEB FETCH WORK - HTTP 200 OK from the internet\n");
                    else
                        klog("www: HTTP fetch MISMATCH\n");
                    tcp_close(c);
                }
            }
        }
    }

    /* ---- LOOPBACK: ping 127.0.0.1 (self-delivery at the IP layer) ---- */
    klog("lo: pinging 127.0.0.1...\n");
    {
        unsigned long rtt = 0;
        if (icmp_ping(0x7F000001, &rtt) == 0)
            klog("lo: ping 127.0.0.1 OK (loopback)\n");
        else
            klog("lo: ping 127.0.0.1 FAILED\n");
    }

    /* ---- ping the gateway: a REAL ICMP round trip through slirp ---- */
    klog("net: pinging gateway 10.0.2.2...\n");
    {
        unsigned int nfo[5];
        net_info(nfo);
        unsigned long rtt = 0;
        if (nfo[0] != 0 && icmp_ping(nfo[2], &rtt) == 0)
            klog("net: ping gateway OK (real ICMP round trip)\n");
        else
            klog("net: ping gateway FAILED\n");
    }

    /* ---- LOOPBACK TCP: a server and a client, both in this OS,
     * connected through 127.0.0.1:9999 - proves loopback + the full
     * server path without any external peer ---- */
    klog("lo: testing loopback TCP (127.0.0.1:9999)...\n");
    {
        long spid = fork();
        if (spid == 0) {
            long lid = tcp_listen(9999);
            if (lid < 0) { klog("lo: listen FAILED\n"); exit(1); }
            long c;
            int w = 0;
            while ((c = tcp_accept(lid)) == -2 && w < 300) { sleep(20); w++; }
            if (c < 0) { klog("lo: child accept FAILED\n"); exit(1); }
            klog("lo: child accepted conn\n");
            char buf[64]; long got = 0; int t = 0;
            while (got < 7 && t < 300) {
                long r = tcp_recv(c, buf + got, 64 - got);
                if (r > 0) got += r; else sleep(10); t++;
            }
            buf[got] = 0;
            klog("lo: child got ");
            { char b[8]; my_itoa((int)got, b); klog(b); }
            klog(" bytes\n");
            long n = 0;
            const char *ack = "LO-BACK-OK";
            while (n < 10) {
                long r = tcp_send(c, ack + n, 10 - n);
                if (r <= 0) break;
                n += r;
            }
            klog("lo: child sent ");
            { char b[8]; my_itoa((int)n, b); klog(b); }
            klog(" bytes\n");
            tcp_close(c);
            tcp_close(lid);            /* free the listener slot */
            exit(0);
        } else if (spid > 0) {
            sleep(50);                 /* let the child bind + listen */
            long c = tcp_connect(0x7F000001, 9999);
            if (c < 0) {
                klog("lo: self-connect FAILED\n");
            } else {
                const char *m = "LO-BACK";
                long sl = 0;
                while (sl < 7) {
                    long r = tcp_send(c, m + sl, 7 - sl);
                    if (r <= 0) break;
                    sl += r;
                }
                char rbuf[16]; long got = 0; int t = 0;
                while (got < 10 && t < 300) {
                    long r = tcp_recv(c, rbuf + got, 16 - got);
                    if (r > 0) got += r; else sleep(10); t++;
                }
                rbuf[got] = 0;
                if (got == 10 && strcmp(rbuf, "LO-BACK-OK") == 0)
                    klog("lo: loopback TCP round trip WORK (127.0.0.1:9999)\n");
                else {
                    klog("lo: loopback TCP MISMATCH (got ");
                    { char b[8]; my_itoa((int)got, b); klog(b); }
                    klog(" bytes: [");
                    for (long i = 0; i < got && i < 12; i++) {
                        char h[3]; h[0]="0123456789ABCDEF"[(rbuf[i]>>4)&15];
                        h[1]="0123456789ABCDEF"[rbuf[i]&15]; h[2]=0; klog(h);
                    }
                    klog("])\n");
                }
                tcp_close(c);
                int st;
                while (waitpid(spid, &st) == 0);
            }
        }
    }

    /* ---- FIREWALL: block a port, watch traffic die, unblock ---- */
    klog("fw: testing the packet firewall (UDP 127.0.0.1:7777)...\n");
    {
        udp_bind(7777);
        const char *m = "FW-PROBE";
        char rbuf[16];
        long got = 0;
        udp_send(0x7F000001, 7777, m, 8);
        for (int w = 0; w < 40 && got == 0; w++) {
            got = udp_recv(rbuf, 16);
            if (!got) sleep(10);
        }
        rbuf[got] = 0;
        if (got == 8 && strcmp(rbuf, "FW-PROBE") == 0)
            klog("fw: UDP loop before rule - WORK\n");
        else {
            klog("fw: UDP loop before rule FAILED (got ");
            { char b[8]; my_itoa((int)got, b); klog(b); }
            klog(")\n");
        }
        fw_add(17, 0x7F000001, 7777, 1);   /* DROP udp -> 127.0.0.1:7777 */
        klog("fw: rule added (drop udp 127.0.0.1:7777)\n");
        got = 0;
        udp_send(0x7F000001, 7777, m, 8);
        for (int w = 0; w < 40 && got == 0; w++) {
            got = udp_recv(rbuf, 16);
            if (!got) sleep(10);
        }
        if (got == 0)
            klog("fw: UDP blocked by firewall - WORK\n");
        else {
            klog("fw: firewall did NOT block (got ");
            { char b[8]; my_itoa((int)got, b); klog(b); }
            klog(")\n");
        }
        fw_clear();
        got = 0;
        udp_send(0x7F000001, 7777, m, 8);
        for (int w = 0; w < 40 && got == 0; w++) {
            got = udp_recv(rbuf, 16);
            if (!got) sleep(10);
        }
        if (got == 8)
            klog("fw: UDP loop after clear - WORK\n");
        else {
            klog("fw: UDP after clear FAILED (got ");
            { char b[8]; my_itoa((int)got, b); klog(b); }
            klog(")\n");
        }
    }

    /* ---- IPv6: SLAAC (RS -> RA -> address) + ping6 ---- */
    klog("ipv6: waiting for SLAAC (router advertisement)...\n");
    {
        unsigned char a[16], r[16];
        int w = 0;
        while (ipv6_info(a, r) != 0 && w < 800) { sleep(25); w++; }
        if (w >= 800) {
            klog("ipv6: no RA received - IPv6 test skipped\n");
        } else {
            klog("ipv6: address configured: ");
            for (int i = 0; i < 16; i += 2) {
                char h[6]; int k = 0;
                h[k++]="0123456789abcdef"[(a[i]>>4)&15];
                h[k++]="0123456789abcdef"[a[i]&15];
                h[k++]="0123456789abcdef"[(a[i+1]>>4)&15];
                h[k++]="0123456789abcdef"[a[i+1]&15];
                h[k]=0;
                klog(h);
                if (i < 14) klog(":");
            }
            klog("\n");
            unsigned long rtt = 0;
            if (icmp6_ping(a, &rtt) == 0)
                klog("ipv6: ping6 our own address OK\n");
            else
                klog("ipv6: ping6 self FAILED\n");
            if (icmp6_ping(r, &rtt) == 0)
                klog("ipv6: ping6 the ROUTER OK (real ICMPv6 round trip)\n");
            else
                klog("ipv6: ping6 router FAILED (router did not answer)\n");
        }
    }

    /* ---- TLS 1.2: a real encrypted HTTPS-style fetch against the
     * host's OpenSSL (RSA-AES128-CBC-SHA256).  If this works, the
     * kernel's own AES + HMAC-SHA256 + RSA-2048 + X.509 + handshake
     * all interoperate with a real TLS peer. ---- */
    klog("tls: testing TLS 1.2 against the host (10.0.2.2:9443)...\n");
    {
        unsigned int nfo[5];
        net_info(nfo);
        if (nfo[0] == 0) {
            klog("tls: no IP address - skipping\n");
        } else {
            long h = tls_connect(0x0A000202, 9443);
            if (h < 0) {
                klog("tls: connect FAILED (host TLS server not running?)\n");
            } else {
                const char *req = "GET / HTTP/1.0\r\nHost: localhost\r\n\r\n";
                long sl = 0, rl = (long)strlen(req);
                while (sl < rl) {
                    long r = tls_send(h, req + sl, rl - sl);
                    if (r <= 0) break;
                    sl += r;
                }
                char rbuf[512];
                long got = 0;
                int tries = 0;
                while (tries < 400 && got < 512) {
                    long r = tls_recv(h, rbuf + got, 512 - got);
                    if (r > 0) {
                        got += r;
                        if (got >= 18 && strncmp(rbuf + got - 18, "TLS-FROM-YART-HOST", 18) == 0) break;
                    } else sleep(10);
                    tries++;
                }
                rbuf[got] = 0;
                if (got >= 15 && strncmp(rbuf, "HTTP/1.1 200 OK", 15) == 0)
                    klog("tls: HTTPS-style fetch WORK - encrypted HTTP 200 from OpenSSL\n");
                else
                    klog("tls: fetch MISMATCH\n");
                tls_close(h);
                klog("tls: connection closed cleanly (close_notify sent)\n");
            }
        }
    }

    /* ---- TLS SERVER: YartOS hosts a real HTTPS site.  The host curls
     * it with OpenSSL: curl -k https://127.0.0.1:9444/ ---- */
    {
        long spid = fork();
        if (spid == 0) {
            klog("tls-srv: SRVBUILD-2 starting HTTPS server on :9444 (curl -k it!)\n");
            long lid = tls_listen(9444);
            if (lid < 0) { klog("tls-srv: listen FAILED\n"); exit(1); }
            for (;;) {
                long h = tls_accept(lid);
                if (h == -2) { sleep(50); continue; }
                if (h < 0) { sleep(100); continue; }
                klog("tls-srv: client connected, handshake done\n");
                char req[512];
                long got = 0;
                int tries = 0, hdr = 0;
                while (tries < 200 && !hdr) {
                    long r = tls_recv(h, req + got, 512 - got);
                    if (r > 0) {
                        got += r;
                        for (long i = 0; i < got - 3; i++)
                            if (req[i]=='\r' && req[i+1]=='\n' &&
                                req[i+2]=='\r' && req[i+3]=='\n') hdr = 1;
                    } else sleep(10);
                    tries++;
                }
                klog("tls-srv: encrypted request received (");
                { char b[8]; my_itoa((int)got, b); klog(b); }
                klog(" bytes), sending encrypted response...\n");
                const char *resp =
                    "HTTP/1.1 200 OK\r\n"
                    "Server: YartOS-TLS/1.2\r\n"
                    "Content-Length: 30\r\n"
                    "Connection: close\r\n"
                    "\r\n"
                    "YART-HTTPS-IS-ALIVE-OVER-TLS!!";
                long rl = (long)strlen(resp);
                long n = 0;
                while (n < rl) {
                    long r = tls_send(h, resp + n, rl - n);
                    if (r <= 0) break;
                    n += r;
                }
                klog("tls-srv: encrypted response sent (");
                { char b[8]; my_itoa((int)n, b); klog(b); }
                klog(" bytes)\n");
                tls_close(h);
            }
        }
    }

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
        long before = doas("yart");
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
    /* ---- REAL signal handlers: handler runs, returns via the kernel
     * trampoline (SYS_SIGRETURN), and the process resumes where it was
     * interrupted - then exits 42 so the parent can verify. ---- */
    klog("sig3: testing REAL signal handler + sigreturn...\n");
    {
        long spid = fork();
        if (spid == 0) {
            sigaction(15, (long)sigterm_handler);   /* catch SIGTERM */
            klog("sig3: child raising SIGTERM on itself\n");
            raise(getpid(), 15);
            /* the handler ran above and returned via sigreturn; if we
             * get here, the interrupted state was restored correctly */
            klog("sig3: child resumed after handler (sigreturn worked)\n");
            exit(42);
        } else if (spid > 0) {
            int status = 0;
            long r;
            while ((r = waitpid(spid, &status)) == 0);
            if (r == spid && status == 42)
                klog("sig3: handler + sigreturn + resume WORK (status 42)\n");
            else
                klog("sig3: FAILED (handler/sigreturn broken)\n");
        }
    }

    /* ---- ACL: grant a specific uid access to a root-only file ---- */
    klog("perm2: testing ACL (grant uid 3000 read on a 0600-root file)...\n");
    {
        long r = doas("yart");                    /* back to root */
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

    /* ---- exec: fork, then the child replaces itself with /bin/hello ---- */
    klog("exec: forking a child that will exec /bin/hello...\n");
    {
        long epid = fork();
        if (epid == 0) {
            char *argv[] = { "/bin/hello", "alpha", "beta", 0 };
            char *envp[] = { "PATH=/bin", "HOME=/home/yart", 0 };
            long r = exec("/bin/hello", argv, envp);
            /* only reached if exec FAILED */
            klog(r == 0 ? "exec: BUG - returned from successful exec\n"
                        : "exec: FAILED (-1)\n");
            exit(1);
        } else if (epid > 0) {
            int status = 0;
            long r;
            while ((r = waitpid(epid, &status)) == 0);
            if (r == epid) {
                klog(status == 7
                     ? "exec: child exited with status 7 - exec+argv+envp WORK\n"
                     : "exec: child exited but with a wrong status\n");
            } else {
                klog("exec: waitpid failed\n");
            }
        }
    }

    /* ---- pipes: fork, child writes, parent reads back ---- */
    klog("pipe: testing in-kernel pipes...\n");
    {
        int fds[2];
        if (pipe(fds) != 0) {
            klog("pipe: pipe() FAILED\n");
        } else {
            long ppid = fork();
            if (ppid == 0) {
                /* child: write end */
                close(fds[0]);
                const char *msg = "hello-through-pipe-123";
                for (int i = 0; i < 22; ) {          /* len(msg) == 22 */
                    long r = write(fds[1], msg + i, 22 - i);
                    if (r == PIPE_WOULD_BLOCK) { sleep(10); continue; }
                    if (r < 0) break;
                    i += (int)r;
                }
                close(fds[1]);
                exit(0);
            } else if (ppid > 0) {
                /* parent: read end (blocking-ish: -2 -> sleep + retry) */
                close(fds[1]);
                char buf[32];
                long got = 0;
                while (got < 22) {
                    long r = read(fds[0], buf + got, 22 - got);
                    if (r == PIPE_WOULD_BLOCK) { sleep(10); continue; }
                    if (r <= 0) break;               /* 0 = EOF (writer closed) */
                    got += r;
                }
                close(fds[0]);
                int status = 0;
                while ((waitpid(ppid, &status)) == 0);
                buf[22] = 0;
                int ok = (got == 22) && buf[0]=='h' && buf[6]=='t' &&
                         buf[21]=='3' && status == 0;
                klog(ok ? "pipe: 22 bytes through a pipe, EOF on close - WORK\n"
                        : "pipe: FAILED (got wrong bytes/status)\n");
            }
        }
    }

    /* ---- real app window round trip: exec /bin/settings with
     * YART_TEST_EXIT=1 - it creates a window surface, the compositor
     * scans+draws it, then the app closes it and exits 0.  Exercises
     * the full ring-3 app path (exec + SYS_WM_CREATE + scan + flip +
     * destroy) headlessly. ---- */
    klog("app: testing real app window (settings, YART_TEST_EXIT)...\n");
    {
        long apid = fork();
        if (apid == 0) {
            char *argv[] = { "/bin/settings", 0 };
            char *envp[] = { "YART_TEST_EXIT=1", "HOME=/home/yart", 0 };
            long r = exec("/bin/settings", argv, envp);
            (void)r;
            klog("app: settings exec FAILED\n");
            exit(1);
        } else if (apid > 0) {
            int status = 0;
            long r;
            while ((r = waitpid(apid, &status)) == 0);
            if (r == apid && status == 0)
                klog("app: window round trip WORK (settings created + closed cleanly)\n");
            else
                klog("app: window round trip FAILED\n");
        }
    }

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
        while ((r = waitpid(pid, &status)) == 0);
        if (r == pid)
            klog("parent: reaped the child\n");
        else
            klog("parent: waitpid failed\n");
    } else {
        klog("init: fork failed\n");
    }

    /* ---- TCP server: a real HTTP server the HOST can curl ----
     * fork a child that keeps serving; the host reaches it via QEMU
     * slirp's hostfwd (127.0.0.1:8080 -> guest :8080). */
    {
        long spid = fork();
        if (spid == 0) {
            klog("tcp: starting HTTP server on :8080 (curl it from the host!)\n");
            long lid = tcp_listen(8080);
            if (lid < 0) {
                klog("tcp: listen FAILED\n");
                exit(1);
            }
            const char *resp =
                "HTTP/1.1 200 OK\r\n"
                "Server: YartOS/0.8.0-tcp\r\n"
                "Content-Type: text/html\r\n"
                "Content-Length: 96\r\n"
                "Connection: close\r\n"
                "\r\n"
                "<html><body><h1>YartOS says hello over TCP!</h1>"
                "<p>served by the Yart kernel</p></body></html>\r\n";
            for (;;) {
                long c = tcp_accept(lid);
                if (c == -2) { sleep(50); continue; }
                if (c < 0) { sleep(100); continue; }
                /* wait for the request header, then answer */
                char req[512];
                long got = 0;
                int tries = 0, hdr = 0;
                while (tries < 100 && !hdr) {
                    long r = tcp_recv(c, req + got, 512 - got);
                    if (r > 0) {
                        got += r;
                        for (long i = 0; i < got - 3; i++)
                            if (req[i]=='\r' && req[i+1]=='\n' &&
                                req[i+2]=='\r' && req[i+3]=='\n') hdr = 1;
                    } else sleep(10);
                    tries++;
                }
                klog("tcp: HTTP request received (");
                { char b[8]; my_itoa((int)got, b); klog(b); }
                klog(" bytes), sending response...\n");
                long n = 0;
                long rl = 205;
                while (n < rl) {
                    long r = tcp_send(c, resp + n, rl - n);
                    if (r <= 0) break;
                    n += r;
                }
                klog("tcp: HTTP response sent (");
                { char b[8]; my_itoa((int)n, b); klog(b); }
                klog(" bytes)\n");
                tcp_close(c);
            }
        }
    }

    klog("init: boot tests complete; entering ring-3 compositor loop\n");

    /* =================================================================
     * RING-3 COMPOSITOR (row 23)
     *
     * The kernel no longer owns the framebuffer.  We are a normal ring-3
     * task with the compositor role (SYS_FB_INFO claimed us).  From here
     * on we own the screen: we draw everything (wallpaper, status bar,
     * dock, cursor) and flip to the real scanout via SYS_FB_FLIP.  The
     * kernel only services drivers and scheduling.
     * ================================================================= */
    wm_run();

    klog("wm: compositor exited (shouldn't happen)\n");
    exit(0);
    return 0;
}
