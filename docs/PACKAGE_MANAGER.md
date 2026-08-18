# Porting a Linux package manager (Alpine's apk) to YartOS — effort estimate

Question: "how much work on syscalls and stuff to port a Linux package
manager, like maybe Alpine?"

Short answer: **to run Alpine's actual `apk` binary unmodified, ~2,500–3,500
lines of kernel work (a Linux ABI compatibility layer) + ~150 lines of ELF
loader work, because Alpine ships a statically-linked `apk.static`.** To
make the packages it installs actually *run*, you also need a dynamic
linker (~2,000–3,000 lines, mostly userland). Before any of it matters,
**the disk-write persistence bug must be fixed first** — packages that
vanish on reboot are useless.

Everything below is grounded in the current YartOS state: 89 syscalls
(custom numbers, POSIX semantics), real VFS + permission bits, TCP/UDP/TLS/
DNS in-kernel, PIE ELF64 loader with ASLR + NX, no threads, no fd-sockets,
no dynamic linker.

---

## 1. What apk actually needs (so the estimate is real, not vibes)

Alpine's `apk-tools` (C, ~30k LOC) is shipped by Alpine as **`apk.static`**
— a fully statically-linked binary (musl + zlib + libfetch + libressl baked
in). That is the key fact: no dynamic linker needed to run the tool itself.

`apk.static` does, in order:
1. read `/etc/apk/repositories`, `/etc/apk/world`
2. HTTPS download of `APKINDEX.tar.gz` + `.apk` packages → **TLS + TCP +
   DNS** (via BSD sockets + getaddrinfo in libfetch)
3. signature check → **RSA/Ed25519** (libressl, needs entropy → getrandom
   or `/dev/urandom`)
4. `.apk` = **gzipped tar**: gunzip (zlib) + its own tar reader
5. install = **file ops**: open/read/write/close/mkdir/unlink/rename/
   symlink/readlink/mknod/chmod/chown/utime/link/fchmod/fchown, `*at`
   variants (musl uses openat/fstatat/unlinkat everywhere), plus **statfs**
   for free-space checks and **mmap** for the package database (adb.c)
6. run `.post-install` scripts → `fork` + `execve` **/bin/sh** + `wait4` +
   **chroot** (into `--root`)

So the dependency chain to run apk.static = musl's syscall surface (single
threaded) + busybox-static (for /bin/sh) + entropy + our existing net/VFS.

## 2. The syscall gap (measured, not guessed)

YartOS already has the *semantics* for ~40 of the ~70–90 Linux syscalls a
static musl program touches (open/read/write/close/lseek/getdents/mkdir/
unlink/rename/chmod/stat/getcwd/chdir/fork/execve/wait4/kill/mmap/munmap/
brk/fsync/pipe/getpid/getuid/...). The real deltas:

| Category | Syscalls | LOC |
|---|---|---|
| **Linux number → handler mapping** (dispatch table) | ~70 numbers | 150 |
| **Struct layout compat** (stat, dirent64, statfs, sigaction/rt, rusage, sysinfo, timespec) | 6 structs | 200 |
| **fd-based sockets** (socket, connect, accept, bind, listen, send/recv, sendmsg/recvmsg, setsockopt/getsockopt, getpeername, shutdown) — plumbing over the existing TCP/UDP/DNS | 12 | 400 |
| **poll / select / pselect6** (blocking wait on fds) | 3 | 200 |
| **\*at family** (openat, fstatat, unlinkat, renameat, mkdirat, symlinkat, readlinkat, faccessat, fchmodat, fchownat, utimensat) | 11 wrappers | 150 |
| **real signals** (rt_sigaction, rt_sigprocmask, rt_sigreturn, sigaltstack, tkill) | 5 | 200 |
| **mmap upgrades** (MAP_ANON\|MAP_FIXED, PROT flags, mprotect, madvise, mremap) | 4 | 150 |
| **clocks** (clock_gettime/res, clock_nanosleep, gettimeofday, times, getrusage) | 6 | 120 |
| **misc** (uname, sysinfo, getrandom, dup3, pipe2, fchdir, chroot, setpgid, setsid, getpgrp, prlimit64, futex-1-op, link, mknod, fdatasync) | 15 | 250 |
| **ELF loader: PT_TLS + FS base** (musl static still needs a thread pointer) | — | 150 |
| **Total kernel + loader** | | **~2,000–2,500** |

Plus `ld-musl` not needed (static), and **futex/threads not needed for
apk+busybox** (both single-threaded; a single-threaded musl never calls
futex — provide a 1-op stub to be safe).

**Honest bottom line for "apk.static runs and can download/install into a
chroot": ~2–3k lines, roughly 3–5 focused days, then days of testing.**
Not an afternoon, not a month.

## 3. The catch nobody tells you: installed packages are DYNAMIC

`apk.static` is static, but **the packages you install are not** — they're
musl dynamic binaries expecting `ld-musl-x86_64.so.1`. So milestone 2 is
mandatory if you want installed software to actually *run*:

| Milestone | Deliverable | Work |
|---|---|---|
| **M0** | Fix virtio-blk data persistence (else installs die on reboot) | unknown — 5-line bug or driver rewrite; must go first |
| **M1** | `apk.static` + `busybox.static` run → can install into a chroot, scripts execute | ~2–3k kernel LOC (above) |
| **M2** | Dynamic linker (ld-musl port or minimal ldso): R_X86_64_RELATIVE/GLOB_DAT/JUMP_SLOT/COPY + TLS + symbol resolution | ~2–3k userland LOC |
| **M3** | Threads (clone/futex), tty/ioctl polish, own repo mirror, init integration | ~1–2k LOC |

After M2 you have a real package manager: `apk add` works and the results
run. M3 is polish.

## 4. The faster honest alternative: don't port apk, reuse its format

If the goal is "YartOS can install software" rather than "run Alpine's
binary," there's a much shorter path: a **native package tool (~1.5–2k
userland LOC)** that uses the syscalls we already have (SYS_TCP/TLS/DNS +
VFS) to download `.apk` files, gunzip (port zlib inflate, ~1.5k) + untar
(~200 lines), verify the signature (we already have RSA in kernel/lib
bignum+x509), and run `.post-install` scripts via fork/exec. Zero kernel
changes. It wouldn't be `apk` itself, but it would install Alpine packages.

**Recommendation:** M0 → native tool (fast win, real packages on disk) →
M1 in parallel (Linux ABI layer, unlocks apk.static + busybox + curl) → M2
(makes installed binaries run). That sequencing gets a working "package
manager" to you the soonest while building the durable Linux-compat
foundation.

## 5. Disk-size note

The current disk image is 64 MiB. Alpine base (busybox+musl) is ~5 MB,
a usable console system ~50–150 MB. Bump the image (trivial: one dd size)
when M0 lands. The initrd is already ~100 MB, so the boot side has room.
