# Building a real Linux static binary for YartOS (musl + busybox)

Proven working: a **static musl busybox** runs on YartOS — `ls`, `cat`, `echo`,
`seq`, `uname`, `pwd`, and `sh` all work through the kernel's Linux-ABI layer.

## Why this works
YartOS's `linux_dispatch()` (in `kernel/arch/x86_64/syscall.c`) understands the
**Linux x86_64 syscall numbers**. musl on x86_64 issues those exact numbers via
the `syscall` instruction, so a static musl binary needs no interpreter and no
runtime — it runs directly. (Dynamically-linked programs need the loader work
in Phase 2 of `docs/LINUX_COMPAT_ROADMAP.md`.)

## Recipe (reproducible)

```sh
# 1. musl (static-only build)
cd /home/user && mkdir musl && cd musl
curl -sSL https://musl.libc.org/releases/musl-1.2.5.tar.gz -o musl.tar.gz
tar xzf musl.tar.gz && cd musl-1.2.5
./configure --prefix=/home/user/musl-out --disable-shared
make -j$(nproc) && chmod +x tools/install.sh && make install

# 2. Linux UAPI headers (busybox needs linux/*.h that musl doesn't ship)
sudo apt-get install -y linux-libc-dev
M=/home/user/musl-out/include
cp -r /usr/include/{linux,asm-generic,mtd,rdma,misc,sound,video,xen,scsi,drm} $M/
cp -rL /usr/include/x86_64-linux-gnu/asm $M/asm   # dereference the symlinks

# 3. busybox, static
cd /home/user && curl -sSL https://busybox.net/downloads/busybox-1.36.1.tar.bz2 -o bb.tar.bz2
tar xjf bb.tar.bz2 && cd busybox-1.36.1
make defconfig
sed -i 's/^# CONFIG_STATIC is not set/CONFIG_STATIC=y/' .config
sed -i 's/^CONFIG_PIE=y/# CONFIG_PIE is not set/' .config
sed -i 's/^CONFIG_TC=y/# CONFIG_TC is not set/' .config   # tc needs extra headers
make CC=/home/user/musl-out/bin/musl-gcc LDFLAGS="-static" -j$(nproc)

# 4. install into the OS image
cp busybox /home/user/YartOS/initrd_root/bin/busybox
chmod 755 /home/user/YartOS/initrd_root/bin/busybox
cd /home/user/YartOS && make iso
```

Boot, open the Console, and:
```
/bin/busybox echo hello
/bin/busybox ls /bin
/bin/busybox sh -c pwd
```

## Kernel fixes this required (all in the Linux-ABI layer)
1. **AT_PHDR was wrong for ET_EXEC** — passed `phoff` instead of the runtime
   `vaddr + phoff`; musl's `__copy_tls` read unmapped 0x40 → SIGSEGV.
2. **`mkdir` was mis-mapped to syscall 81** (Linux 81 = `fchdir`, 83 = `mkdir`).
3. **Region table exhaustion** — musl mallocng maps one region per allocation
   group; `MAX_USER_REGIONS` 32→256 plus **adjacent-region coalescing**.
4. **`getdents64` never advanced `f->pos`** — `readdir` looped forever, the
   heap grew to OOM. This is what broke `ls`.
5. **`MAP_FIXED` returned ENOSYS** — musl donates the brk heap to malloc via
   `mmap(MAP_FIXED)`; now accepts already-mapped targets.
6. **`setuid/setgid/chown/fchmod/...`** mapped to safe no-ops (single-user OS).

## What still returns ENOSYS (watch the `linux: ENOSYS #N` traces)
- `ioctl` (16) — terminal size/attrs; apps degrade gracefully.
- `sendfile` (40) — `cat` fell back to read/write.
- `mount/umount`, `chroot`, `mknod`, `link`, xattrs — not needed yet.

Run `python3 scripts/syscall_matrix.py` to see the live coverage.
