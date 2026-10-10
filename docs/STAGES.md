# Yart OS - the six stages, mapped to the source tree

## Stage 1 - Toolchain & booting
- `scripts/build-toolchain.sh`   - builds `x86_64-elf-gcc` 14.2.0 + binutils 2.42 in `~/opt/cross`
- `scripts/get-limine.sh`        - vendors Limine v7.x (binary branch) and the host helper
- `Makefile`                     - parallel build (`make -j16 iso`)
- `limine.cfg`                   - bootloader menu, theme colours, kernel + initrd modules
- `kernel/linker.ld`             - higher-half ELF, special Limine request sections
- `kernel/arch/x86_64/main.c`    - `kmain` entry, banner, "Welcome to Yart OS"
- `make iso` -> `yart.iso`, `make run` -> QEMU + KVM

## Stage 2 - CPU & memory
- `kernel/arch/x86_64/gdt.c` + `gdt_load.asm`  - GDT (kernel + user code/data, TSS, IST1)
- `kernel/arch/x86_64/idt.c` + `isr.asm`        - IDT, 256 ISR stubs, exception panic
- `kernel/arch/x86_64/pic.c`                    - 8259 PIC remap to 0x20/0x28
- `kernel/include/yart/io.h`                    - port IO + cli/sti/hlt/cr2/cr3/invlpg
- `kernel/mm/pmm.c`                             - bitmap PMM over Limine memmap
- `kernel/mm/vmm.c`                             - 4-level paging on the Limine PML4 + HHDM
- `kernel/mm/heap.c`                            - linked-list `kmalloc/kfree` with coalescing

## Stage 3 - Drivers
- `kernel/arch/x86_64/pit.c`     - PIT @ 100 Hz, `pit_ticks`, `sleep_ms`
- `kernel/arch/x86_64/rtc.c`     - CMOS RTC, BCD-aware
- `kernel/drivers/keyboard.c`    - PS/2 set-1 scancodes, IRQ1, ring buffer
- `kernel/drivers/mouse.c`       - PS/2 AUX 3-byte packets, IRQ12, button bitmap

## Stage 4 - GUI / desktop
- `kernel/gui/fb.c`              - GOP framebuffer wrapper + double-buffered backbuffer
- `kernel/gui/font_data.c`       - 256x16 byte 8x16 monochrome bitmap font (auto-generated)
- `kernel/gui/cursor.c`          - 12x18 neon arrow cursor
- `kernel/gui/desktop.c`         - wallpaper, status bar, windows, dock, compositor

Drawing API (kernel/include/yart/gui.h):
`draw_pixel`, `draw_hline`, `draw_vline`, `draw_rect`, `draw_rect_outline`,
`draw_rect_gradient_v`, `draw_char`, `draw_text`, `draw_text_shadow`.

Compositor flow (`desktop_render`):
1) wipe backbuffer (`draw_wallpaper`)
2) status bar
3) windows back-to-front
4) dock
5) cursor
6) `fb_present()` (line-by-line 64-bit copy → hardware fb)

## Stage 5 - VFS + initrd
- `kernel/fs/vfs.c`                              - VFS tree built from a USTAR initrd
- `Makefile` initrd_root rule + `module` line in `limine.cfg`
- Userland scaffolding doc: `userland/README.md` (GDT user descriptors are already set
  up; one `iretq` to ring 3 + an `int 0x80` IDT slot is all that's missing)

## Stage 6 - Deployment
- `scripts/run-qemu.sh`          - KVM-accelerated boot, OVMF, USB-XHCI keyboard/mouse
- `scripts/usb-deploy.sh`        - safely `dd` `yart.iso` to `/dev/sdX`
- The hybrid ISO boots both BIOS and UEFI, so it works on the HP ProBook out of the box.

## Verified
- Compiles with `x86_64-elf-gcc` (cross) AND with the host `x86_64-linux-gnu-gcc`
  (only the freestanding C subset is used).
- Boots in QEMU UEFI: serial log shows banner -> PMM -> VMM -> heap -> fb -> initrd
  -> "kernel up; entering desktop loop." with no panics.
- Screenshot in `docs/screenshot-desktop.png` captured live from the running ISO.
