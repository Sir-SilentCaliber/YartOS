# Stage 10 - implementation notes & honest scope

> **STATUS BANNER (2026-08-03):** this document predates stages 11+.
> Since it was written the kernel gained SMP (per-CPU scheduler), a
> preemptive scheduler with CoW fork, demand paging + swap, a private
> PML4 per task, exec(2) with argv/envp, blocking waitpid/sleep, fast
> syscall/sysret, a real virtio-blk disk + journaled/CRC'd filesystem,
> e1000 networking (ARP/IPv4/ICMP/UDP/DHCP) and HDA audio, and the GUI
> moved to ring 3 entirely.  Section 9 below ("networking = vapor")
> is specifically outdated: see `kernel/net/` + `kernel/drivers/e1000.c`.
> See `BRUTAL_AUDIT.md` for the current state.

## What shipped

1. **Pinning / unpinning** — done.
   - `/etc/yart.conf` parser at `kernel/fs/config.c`; default config
     written at build time.
   - `config_pin()` / `config_unpin()` persist back to the initrd image
     (note: the *initrd is in RAM*, so changes vanish on reboot — to
     persist across boots we'd need a writable disk-backed FS, which
     is a separate stage).
   - Right-click any dock entry: "Pin/Unpin from Dock" on a pinned app,
     or "Pin App to Dock" / "Close Window" on a running app.

2. **Drawer search** — done. Open drawer (F1 or dock button), start
   typing. List filters live; Enter launches the first match; Esc
   closes and clears the query.

3. **Ctrl+S bug** — fixed. Root cause was the keyboard driver
   *swallowing* Ctrl press events at `case 0x1D` (so the editor never
   knew Ctrl was held). The driver now exposes modifier state via
   `kbd_ctrl_held()` *and* tags every keyboard event with the current
   modifier bits (`KEY_SHIFT`, `KEY_CTRL`, `KEY_ALT`). The editor's
   on_key now checks `mods & KEY_CTRL` first and returns immediately
   on save, so the 's' never reaches `ed_insert()`.

4. **Real image decoding** — done.
   - New `kernel/fs/bmp.c` decodes 24- and 32-bit uncompressed BMPs.
   - Wallpaper now loads `/etc/wallpaper.bmp` (1280x800 BMP shipped on
     the initrd, generated at build time by Pillow).
   - `bmp_blit()` supports nearest-neighbour scaling and proper alpha
     blending.

5. **Multiple fonts** — done.
   - `scripts/gen_fonts.py` emits three font tables
     (`yart_font_default`, `yart_font_mono`, `yart_font_term`) and a
     registry `yart_fonts[]`.
   - `fb_set_font(name)` swaps the active font at runtime.
   - The Settings panel "Fonts" tab is a segmented chooser; selecting
     changes the active font *and* writes `font.system` to yart.conf.

6. **Settings panel rewrite** — done. 9 tabs:
   Appearance, Dock, Top Bar, Wallpaper, Fonts, Input, Display,
   Time, Power. Every control mutates `g_config` and immediately
   calls `config_save("/etc/yart.conf")`.

8. **Window snap + decoupled cursor** — partial.
   - Edge-snap (drag to top = maximize, left/right = half) was already
     present; kept it.
   - Cursor is drawn *after* `apply_night_light()` in `desktop_render()`,
     so it stays crisp.  *True* hardware/asynchronous cursor would
     need either (a) a separate thread or (b) HW cursor sprite support
     in the framebuffer hardware, neither of which we have on Limine's
     plain linear FB. The cursor will still stutter if an app's paint
     callback blocks the main loop (which they don't, by design).

9. **Networking** — *foundations only*.
   - `kernel/drivers/pci.c` enumerates the PCI bus and exposes NIC +
     audio presence in the top status bar (e.g. shows "e1000" or
     "RTL8139" in amber when a NIC is detected).
   - Actual MAC/RX/TX/DHCP/lwIP is not in this stage. That is
     genuinely weeks of work and would require: NIC ring buffers, MAC
     address probing, an Ethernet frame layer, ARP, IPv4, ICMP, UDP,
     TCP, DHCP client, DNS. Adding it now would be vapor.

10. **Audio** — *foundations only*. PCI scan reports the device
    (AC97/HDA). No mixer, no playback path yet.

11. **Workspaces (4)** — done. **Ctrl+Alt+Left/Right** switches.
    The status bar shows 4 pips, the active one in your accent
    colour. Windows live on the workspace they were created in;
    the dock shows only tasks on the current workspace.

## What I deliberately did NOT ship

### 7. IPC / multithreaded compositor with userspace apps

This is the single biggest change in the entire project. Doing it
*correctly* requires, in roughly this order:

  1. A **preemptive scheduler** — currently Yart is cooperative,
     single-task. Need a per-task `kstack`, `rsp`, registers struct,
     a runqueue, and a PIT-driven `schedule()`.
  2. **fork() / exec()** — duplicate or replace an address space.
  3. **Shared memory** — `shm_open()` / `mmap()` so an app's pixel
     buffer is visible to the compositor.
  4. **An IPC protocol** — Unix-domain-socket-like channels. Messages
     for `surface_attach`, `surface_damage`, `input_event`.
  5. A **compositor refactor** — replace the per-window
     `void (*paint)(window_t *)` callback with a "blit the client's
     shared buffer" pass.

Realistically that's 4 to 6 stages of work. If I'd faked it
(e.g. just renamed callbacks or added stub IPC structs), you'd see
"IPC compositor" in the changelog but get exactly the same behaviour
as today. Better to be honest: same compositor as Stage 9, plus the
features above. Stage 11 can start the scheduler.

If you'd like, I can begin scheduler+fork+exec in the next round —
that's the right next step before networking and audio drivers
mature, because a real net stack and a real audio stack want their
own kernel threads.
