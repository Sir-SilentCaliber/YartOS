# Yart OS - Architecture Notes

This document is the "why" behind every file under `kernel/`.  It is written
for someone who knows C and has read about Linux's overall layout once
(the `arch/`, `mm/`, `fs/`, `drivers/` split is the same on purpose) but
has never built a hobby OS before.

## 0.  Mental model

```
                   +-----------------------------------------+
                   |        Userland (ring 3) - future       |
                   +-----------------------------------------+
   syscall (int 0x80)              ^                ^
                                   |                |
   +-------+   +---------+   +------------+   +------------+
   | HAL   |   | Memory  |   |   VFS      |   |   GUI      |
   |  GDT  |   |  PMM    |   | USTAR ramdisk |  compositor |
   |  IDT  |   |  VMM    |   | vfs_lookup |   | windows    |
   |  PIC  |   |  heap   |   | vfs_read   |   | dock       |
   +-------+   +---------+   +------------+   +------------+
                                   ^                ^
                                   |                |
                          +-------------------------+
                          |         Drivers         |
                          | PIT  RTC  KBD  MOUSE    |
                          | (LAPIC/IOAPIC stubs)    |
                          +-------------------------+
                                   ^
                                   |
                          +-------------------------+
                          |   Limine bootloader     |
                          +-------------------------+
                                   ^
                                   |
                              UEFI / BIOS
```

Compared to Linux:

| Linux    | Yart equivalent | File |
|----------|-----------------|------|
| `arch/x86/`         | `kernel/arch/x86_64/`     | gdt.c, idt.c, isr.asm, pic.c, pit.c, rtc.c, apic.c, syscall.c, main.c |
| `mm/`               | `kernel/mm/`              | pmm.c, vmm.c, heap.c |
| `drivers/`          | `kernel/drivers/`         | keyboard.c, mouse.c |
| `fs/`               | `kernel/fs/`              | vfs.c, elf.c |
| `lib/`              | `kernel/lib/`             | string.c, console.c |
| `init/main.c`       | `kernel/arch/x86_64/main.c` | `kmain` |
| `drivers/video/fbdev/` + `drivers/gpu/drm/` | `kernel/gui/` | fb.c, font_data.c, cursor.c, desktop.c, apps.c |

Yart is *much* simpler (single-threaded, no preemption yet, no SMP), but
the **module layout is intentionally familiar**.

## 1.  Boot chain

```
UEFI/BIOS  ->  Limine  ->  kmain()  ->  desktop loop
```

Limine reads `limine.cfg`, enters long mode, builds a higher-half memory
map (HHDM) so we can address all of physical RAM via virtual addresses
`0xffff_8000_0000_0000 + phys`, opens a GOP framebuffer at the resolution
in `limine.cfg`, loads our `boot:///boot/yart.elf` ELF, copies the
`/boot/initrd.tar` module into RAM, and `jmp`s to `kmain`.

In `kmain` we:

1. Bring up the serial port for debug printing.
2. Sanity-check that the four Limine responses we asked for arrived.
3. Stash the HHDM offset into `g_hhdm_offset` (used by `phys_to_virt`).
4. Build a fresh GDT (we don't trust Limine's), then load an IDT and
   remap the 8259 PIC.
5. Initialise the PMM from the Limine memmap.
6. Adopt Limine's PML4 as ours so we can `vmm_map` new pages later.
7. Start the kernel heap.
8. Wrap the framebuffer, allocate a backbuffer, init text-mode console.
9. Mount the initrd into the VFS.
10. Sniff ACPI tables (XSDT/RSDT) for fun.
11. Install the `int 0x80` syscall stub.
12. Bring up PIT, keyboard, mouse.
13. Init the desktop (creates the welcome + terminal windows).
14. `sti` and enter the desktop loop.

The desktop loop is tiny:

```c
for (;;) {
    desktop_tick(now_ms);    // drain mouse + key queues, render
    __asm__ volatile("hlt"); // sleep until next interrupt
}
```

Because IRQs (PIT, KBD, MOUSE) all wake `hlt` immediately, this is
extremely power-efficient and gives us ~100 fps of compositor refresh
"for free".

## 2.  Memory layout

```
   0xffff_ffff_8000_0000 ------+-------------------------+
                               |       kernel image      |
                               |      (.text/.rodata/    |
                               |       .data/.bss)       |
                               +-------------------------+
                               | kernel heap, dynamic    |
                               | (PMM-backed via HHDM)   |
                               +-------------------------+

   0xffff_8000_0000_0000 ------+-------------------------+
                               |  HHDM: phys + offset    |
                               |  - physical RAM         |
                               |  - framebuffer + back   |
                               |  - initrd module        |
                               +-------------------------+

   0x0000_0000_4000_0000 ------+-------------------------+   (future)
                               |   userland mappings     |
                               +-------------------------+
```

* HHDM means "Higher Half Direct Map".  Limine sets up a 1:1 mapping so
  that `*(uint32_t*)(phys + g_hhdm_offset)` reads physical address `phys`.
  This is how the PMM bitmap and framebuffer backbuffer are addressed.
* The kernel itself is linked at `0xffffffff80000000` (the canonical
  higher-half kernel base).  See `kernel/linker.ld`.

## 3.  Interrupt path, end-to-end

1. CPU receives IRQ1 (keyboard) from the 8259 master.
2. Because we remapped PIC1 to vector 0x20, the CPU vectors to IDT[0x21].
3. `isr_stub_table[0x21]` (in `isr.asm`) pushes a fake error code, the
   vector number, then `PUSHALL` and `call isr_dispatch`.
4. `isr_dispatch` (in `idt.c`) sees `vector >= 32`, calls the registered
   `handlers[0x21]` which is `kbd_irq` from `drivers/keyboard.c`.
5. `kbd_irq` reads port `0x60`, decodes shift/caps/etc, and pushes an
   event into a small ring buffer.
6. Returns, `pic_eoi(1)` fires on the way out.
7. Stubs `POPALL`, `add rsp, 16`, `iretq`.
8. The desktop's main loop later calls `kbd_poll_event()` to drain.

The same pattern applies to PIT (IRQ0 → `pit_irq` → tick++) and mouse
(IRQ12 → `mouse_irq` → 3-byte packet decode).

CPU exceptions (vectors 0..31) take a different branch in `isr_dispatch`:
they print a full register dump on the serial console and switch the
framebuffer to a panic screen with the cause and `CR2` if it was a page
fault.

## 4.  Compositor

The compositor is single-buffered from the user's POV (one continuous
moving image) but **double-buffered** in implementation.  We never draw
directly into the hardware framebuffer; everything goes into
`g_fb.pixels` (allocated via PMM) and `fb_present()` does a single
pitch-aware 64-bit memcpy to `g_fb.fb`.  Result: zero tearing.

Each frame:

```
draw_wallpaper()   // gradient + grid + scanlines + watermark
draw_statusbar()   // brand + memory bar + RTC clock + uptime
for each window in z-order back-to-front:
    draw shadow, body, gradient titlebar, border, traffic lights, title
    win->paint(win)         // user content
draw_dock()        // hover-aware app launchers
cursor_draw()      // 12x18 neon arrow (last so it's always on top)
fb_present()       // blit
```

Window focus/drag/close logic lives in `desktop_handle_mouse` in
`desktop.c`.  Keyboard is routed only to the focused window if it is the
terminal (a deliberate KISS decision until there's a real input system).

## 5.  Why YartFS = USTAR

USTAR (the format used by classic `tar`) is:

* Trivial to parse: one 512-byte header per entry, fields are octal ASCII.
* Trivial to *produce*: `tar --format=ustar -cf initrd.tar .`.
* Path-aware (full paths in the header), so the VFS tree is naturally
  derivable.
* Round-trip compatible with any host's `tar` so you can mount the
  initrd on Linux for inspection.

Once a real disk driver lands, YartFS-on-disk will use the same vnode
shape but a block-backed cache.  The `vfs_*` API does not change.

## 6.  Future work (mapped to source)

| Feature | Where it goes | Note |
|---------|---------------|------|
| SMP                  | `kernel/arch/x86_64/apic.c` + new smp.c | LAPIC stub already exists |
| Scheduler            | new `kernel/sched/` | one-task event loop today |
| Ring-3 process       | extend `arch/x86_64/main.c` after `elf_load` | TSS/IST already set |
| Disk driver (AHCI)   | `kernel/drivers/ahci.c` | PCI enum needed first |
| Real on-disk YartFS  | `kernel/fs/yartfs.c` | block-backed inode cache |
| Network              | `kernel/drivers/e1000.c` + `kernel/net/` | huge undertaking |
| GPU acceleration     | `kernel/drivers/virtio_gpu.c` | desktop already uses backbuffer |

The point is that none of these require restructuring; they slot in.
