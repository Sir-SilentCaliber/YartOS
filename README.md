# Yart OS

A calm, minimal x86_64 operating system written from scratch — SMP scheduler,
ring-3 software compositor with the full Kora icon theme, RTC wall clock,
tweened dock with frosted glass, smooth 60fps software rendering.

## Quick start

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

## Layout

```
kernel/                Ring-0 kernel (SMP, scheduler, VFS, drivers, fb)
userland/              Ring-3 compositor/wm + freestanding libc
  wm.c                 Compositor (panel, dock, cursor, tween animations)
  gfx.c                Software renderer (blit, blend, rounded rects, fonts, icons)
  sys.h                Syscalls + freestanding libc helpers
  init.c               Entry point
kora/                  All visual assets
  icons/kora/          Full Kora icon theme (scalable/ + symbolic/ SVGs, ~7000 icons)
  wallpapers/          default.png (+ more when added)
  cursors/             (reserved for cursor themes)
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

## In-OS runtime paths
| Asset                  | Path                |
|------------------------|---------------------|
| Kernel                 | `/boot/yart.elf`    |
| Compositor + assets    | `/bin/init`         |
| Fallback wallpaper BMP | `/etc/wallpaper.bmp`|
| Config                 | `/etc/yart.conf`    |
| Home                   | `/home/yart/`       |

Icons and the main wallpaper are statically linked into `/bin/init` for instant
boot (no disk I/O). `/etc/wallpaper.bmp` exists for a future live-switcher.
