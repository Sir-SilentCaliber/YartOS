# Yart OS

A custom 64-bit hobby operating system written in C11 + x86_64 assembly, booted
by **Limine**, with a custom desktop environment (framebuffer compositor,
windows, dock, mouse cursor), a VFS layered over a USTAR initrd, paging, a
slab-ish heap, PS/2 keyboard + mouse, PIT, RTC, and ring-3 userland support.

It is heavily inspired by Linux + classic UNIX, but it is its own thing:
unique color identity (deep purple `#1B0A2E`, sleek black `#0A0A12`,
neon cyan accents `#00F0FF`), a custom 8x16 bitmap font baked into the
kernel, and the **YartFS** initrd format.

```
+-----------------------------------------------------------+
|                        Yart Desktop                       |
|  [Window: Welcome]   [Window: Terminal]                   |
|                                                           |
|  Compositor (double-buffered) -> Framebuffer (Limine GOP) |
|  GUI: drawing prims, font, cursor, windows, dock          |
|  Drivers: PS/2 KBD + AUX mouse, PIT, RTC, serial          |
|  FS: VFS + USTAR initrd (YartFS-compatible)               |
|  MM: PMM (bitmap) + VMM (4-level paging) + kheap          |
|  HAL: GDT, IDT, ISR/IRQ stubs, PIC remap                  |
|  Bootloader: Limine (UEFI + BIOS)                         |
+-----------------------------------------------------------+
```

## Quick start

```bash
# 1. Build the cross-compiler (only once, ~20 min)
./scripts/build-toolchain.sh

# 2. Add it to PATH for this shell
export PATH="$HOME/opt/cross/bin:$PATH"

# 3. Build kernel + ISO (parallel)
make -j16 iso

# 4. Run in QEMU with KVM
make run

# 5. Burn to USB (DESTROYS the target drive!)
sudo ./scripts/usb-deploy.sh /dev/sdX
```

See `scripts/`, `Makefile`, and the per-stage notes in `docs/` (inline
in the source files).
