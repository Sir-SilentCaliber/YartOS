# Yart OS

A calm, minimal x86_64 operating system written from scratch — SMP scheduler,
ring-3 software compositor with the Kora icon theme, RTC wall clock,
tweened dock with frosted glass, smooth 60fps software rendering.  Also:
per-process page tables, CoW fork, demand paging + disk-backed swap,
exec(2) with argv/envp, blocking waitpid/sleep, fast syscall/sysret, a
journaled + CRC32'd persistent filesystem on virtio-blk, e1000 networking
(ARP/IPv4/ICMP/UDP/DHCP), Intel HDA audio, REAL photo cursors (selectable
from the Settings app) and the first real ring-3 app: /bin/settings with
its own window surface (per-window input routing).

## Quick start

The project is fully **portable**: clone it anywhere, install the host
dependencies, and `make iso` builds a bootable image.  No absolute paths,
no committed build artifacts, all assets vendored in the tree.

```bash
git clone <url> yartos && cd yartos
```

**1. Install host dependencies** (one-time):

```bash
./bootstrap.sh             # auto-detects Debian/Ubuntu/Arch/Fedora/macOS
```
Or install manually: `gcc/clang, binutils, nasm, xorriso, git, python3,
python3-pil (Pillow), librsvg2-bin (rsvg-convert), qemu-system-x86, ovmf`.

**2. Build + boot:**

```bash
make -j$(nproc) iso       # fetches Limine automatically the first run
./scripts/run-qemu.sh     # boots with KVM if available, TCG fallback otherwise
```

If the build stops with `ERROR: rsvg-convert not found` or
`ERROR: Pillow not found`, run `./bootstrap.sh` (or install the listed package)
and retry.

**Reproducible container build** (no host toolchain needed — Docker):

```bash
docker build -t yartos .
docker run --rm -v "$PWD":/src yartos make -j"$(nproc)" iso
```

Verify the tree has no stray build artifacts: `make portable-check`.

## Linux compatibility layer (static + dynamic ELF)

YartOS runs **genuine Linux x86_64 ELF binaries** through a translated
syscall ABI (`kernel/arch/x86_64/syscall.c`) and a from-scratch **dynamic
linker** (`tests/ld-yart.c`, installed as `/lib/ld-yart.so`):

- **Static ET_EXEC** binaries — file/console/process syscalls, `stat`,
  `getdents64`, `uname`, `mmap`, `writev`, `clock_gettime`, the auxiliary
  vector, and **threads** (`clone` + `futex`), **sockets** (TCP/UDP),
  **execve**, and **TLS** (`arch_prctl ARCH_SET_FS` → `%fs`).
- **Dynamically-linked PIE** binaries — `PT_INTERP` loading, `DT_NEEDED`
  shared objects, and `RELATIVE/GLOB_DAT/JUMP_SLOT/64` relocations.

Prove it from the terminal (Nyra): `linuxtest`, `linuxtest2`, `dynhello`,
`tlstest`.  Honest limit: the linker resolves the common relocations + TLS
base; glibc/musl-class TLS *data* relocations and IFUNC are the next step.

## Package manager (apk)

`apk add/del/list/search/info` installs native `.ypkg` packages; GUI packages
drop a `/usr/share/applications/*.desktop` entry the compositor auto-scans,
so installed apps appear in the Super launcher (`apk add calc` → press Super).

## Layout

```
kernel/                Ring-0 kernel (SMP, scheduler, VFS, drivers, fb)
userland/              Ring-3 compositor/wm + freestanding libc
  wm.c                 Compositor (panel, dock, cursor, tween animations)
  gfx.c                Software renderer (blit, blend, rounded rects, fonts, icons)
  sys.h                Syscalls + freestanding libc helpers
  init.c               Entry point
kora/                  All visual assets
  icons/kora/          Kora icon theme SVGs — trimmed to exactly the ~102
                       SVGs the build resolves (the full theme is ~6.5k
                       files; restore from the upstream Kora repo to add
                       more icons)
  wallpapers/          generated procedurally at build time
  cursors/             cursor source PNGs (tracked build inputs)
scripts/               Asset generators, Limine bootstrap, QEMU runner
initrd_root/           Skeleton filesystem (bin/, etc/, home/yart/)
build/                 (generated) compiled objects, kora.bin, init.elf
limine/                (generated, fetched by make) Limine bootloader
iso_root/              (generated) staged ISO tree
yart.iso               (generated) bootable hybrid ISO
```

## Make targets
- `make` / `make all`    — build kernel + compositor
- `make assets`          — regenerate kora.bin / wallpaper.bin from kora/ sources
- `make iso`             — build bootable ISO (fetches Limine if missing)
- `make run`             — build ISO and boot in QEMU
- `make clean`           — remove build artifacts
- `make distclean`       — also remove the downloaded Limine checkout
- `make portable-check`  — verify the tree carries no build/scratch cruft

## Portability notes

- All generated artifacts (`build/`, `iso_root/`, `yart.iso`,
  `yart-disk.img`, `runlogs/`, `limine/`, `initrd_root/bin/*`,
  screenshots) are `.gitignore`d and removed by `make clean`.
- `kora/icons/` (SVG sources), the embedded font, the cursor source PNGs,
  and the WiFi firmware blobs under `initrd_root/lib/firmware/` are
  **tracked**, so the build is self-contained (no Linux source tree needed).
- Limine is fetched automatically by `make` on first run.
- `bootstrap.sh` installs host deps on Debian/Ubuntu, Arch, Fedora and
  macOS; QEMU + OVMF paths are auto-detected; KVM is used when available
  (TCG fallback otherwise).

## In-OS runtime paths
| Asset                  | Path                |
|------------------------|---------------------|
| Kernel                 | `/boot/yart.elf`    |
| Compositor + assets    | `/bin/init`         |
| exec() demo binary     | `/bin/hello`        |
| Settings app (real)    | `/bin/settings`     |
| Cursor theme config    | `/home/yart/cursor.conf` |
| Fallback wallpaper BMP | `/etc/wallpaper.bmp`|
| Home                   | `/home/yart/`       |

(`/etc/yart.conf` is still shipped on the initrd for future use; the kernel
no longer parses it since the GUI moved to ring 3.)

Icons and the main wallpaper are statically linked into `/bin/init` for instant
boot (no disk I/O). `/etc/wallpaper.bmp` exists for a future live-switcher.
