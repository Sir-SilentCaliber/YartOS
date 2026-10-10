# Changelog

## 0.7.0-apps  (2026-08-03 - compositor fixes, photo cursors, first real app)

Compositor / app pass on top of 0.6.0.  Focus: the reported dock-hover
freeze, real photo cursors chosen from a real Settings app, and the first
real ring-3 app.

### The dock-hover freeze (fixed)
The compositor's frame pacing was `while (time_ms() < target) { yield(); }`:
every `yield()` parks the task until the next timer tick, so with a slow or
stalled tick the loop never converged (and any mouse movement made it exit
WITHOUT yielding, monopolizing the CPU).  The pacing is now a FIXED 2
yields per frame with mid-slice mouse polling - the frame always completes
and the compositor always gives the CPU back, independent of the clock.
Also: the dock now caches rest-size icon sprites (the per-pixel scaled
blend of 6 icons every hover frame was the biggest per-frame cost), the
tween math is unchanged but can no longer be starved, and cursor
restore/window draws are window-aware (no wallpaper holes through apps).

### Real photo cursors from the web + Settings chooser
- `kora/cursors/` now holds REAL cursor art (downloaded PNGs: a white
  arrow with black outline, a flat-style arrow, a pointing-finger hand).
- `scripts/gen_cursors.py` crops to the alpha bbox, LANCZOS-downscales to
  32px (smooth anti-aliased edges), computes hotspots, and emits
  `build/cursors.bin` + `cursor_assets.h` (2 themes: photo-white, photo-flat).
- The compositor draws the photo cursors (arrow + hand) at the pointer;
  the classic procedural cursors remain the "classic" theme and the
  fallback for kinds without photos (ibeam, resize, ...).
- `/home/yart/cursor.conf` selects the theme (default `theme=photo-white`);
  the compositor polls it once a second and swaps live.

### First REAL app: /bin/settings (window surface protocol)
- New kernel surface protocol (SYS_WM_CREATE/FLIP/SCAN/FOCUS/DESTROY):
  a canvas is mapped into BOTH the app's and the compositor's address
  space; the kernel owns refcounts, computes a centered window position,
  and cleans up on app death (reap hook).
- Per-task input queues: keyboard goes to the focused task, mouse is
  COPIED to the compositor (cursor) AND the focused app - each drains its
  own queue, no event consumed twice (SPSC rings, lock-protected).
- `userland/settings.c` is the first real app: a windowed cursor-theme
  chooser with previews of every theme, click or 1-9 to select, Esc/X to
  close; it writes cursor.conf and the compositor applies the change.
- The dock now contains ONLY real things: **Settings** (launches the app,
  refocuses if running) and **Trash** (shows "Trash is empty").  All fake
  app icons (Files/Terminal/Browser/Launchpad) are gone.
- Focus model: clicking a window focuses it; clicking the desktop drops
  focus back to the compositor.

### Also fixed in the kernel while wiring this up
- `sys_sigaction` was fixed earlier; the input fanout now uses a
  lock-protected SPSC ring per task (no push/pop race between the IRQ
  producer and syscall consumers).

## 0.6.0-auditfix  (2026-08-03 - brutal-audit fix pass)

This release fixes the concrete defects found by the external audit of the
committed tree.  Every item below was a real, verified defect in the code,
not a doc change.

### Critical fixes
- **blkfs on-disk layout bug (data-loss class).**  `format()` computed
  `crc_start_sector` BEFORE subtracting the CRC sectors from the data
  area, so the per-block CRC32 table overlapped the ENTIRE write-ahead
  journal (and spilled into the swap tier on the default 32 MiB disk).
  Consequences: `crc_store()`'s read-modify-writes corrupted journal
  records, and `jrn_clear_range()` (run after every sync) zeroed the CRC
  table every ~1 s, silently disabling bit-rot detection.  The layout is
  now computed with strict non-overlap (verified to tile exactly on
  16/32/64/128 MiB disks), the journal start is persisted in the
  superblock, and mount validates the whole geometry (reformats with a
  loud warning if the on-disk format is old or invalid).  Format version
  bumped to 2.
- **Silent 16 KiB file truncation on disk is gone.**  Files were capped at
  32 direct blocks (16 KiB) and silently truncated on sync with no error
  to the caller.  Inodes now have 32 indirect tables (128 entries each),
  so files up to ~2 MiB persist correctly; the dirty-tracking bitmap
  became a dirty RANGE (`dirty_b0..b1`) because a 32-bit bitmap silently
  missed block 33+ under the new layout.  Files still too large now stay
  dirty and re-log the warning on every sync instead of silently losing
  data.  Also fixed: stale direct-block pointers were not cleared on
  shrink, so a shrink-then-grow could have reused a freed sector.
- **doas accepted ANY non-empty password.**  `sys_doas` checked
  `kpw[0] != 0` while `kernel/lib/sha256.c` sat completely unused.  Now
  every admin account stores salt + SHA-256(salt || password), the
  comparison is constant-time, failures are counted with a temporary
  lockout, and the default `demo` password (`yart`) is hashed at boot.
- **The fast syscall/sysret path is now actually used.**  The kernel's
  LSTAR entry (EFER.SCE + STAR/LSTAR/SFMASK, slot-5 user CS 0x2B) was
  armed but `userland/sys.h` still emitted `int $0x80` - userland now
  emits `syscall` (verified: 249 `syscall` sites, 0 `int $0x80` in the
  built binaries).  The kernel's segment check accepts CS=0x2B.

### Real process-model additions
- **exec(2): SYS_EXEC (45).**  `sys_exec` copies argv/envp into kernel
  memory, `user_exec` builds a fresh private address space (ASLR code +
  stack), writes a real SysV process image (argc/argv[]/envp[]/strings)
  onto the stack, frees the old address space and returns straight into
  the new program.  `userland/start.c` is a proper crt0 that calls
  `main_entry(argc, argv, envp)`; `/bin/hello` (a new second binary)
  proves exec + argv + envp + exit-status end-to-end from init.
- **Blocking waitpid.**  `sched_waitpid` parks the parent
  (TASK_BLOCKED + waiting) instead of returning 0 for a busy userland
  loop; `sched_exit`/`sched_kill` wake it.  The zombie check and the
  waiting flag are atomic under `g_tasks_lock` (no lost wakeup), and a
  woken task is queued BEFORE its state flips to READY (release store)
  so it can never be queued twice - this also fixes a latent SMP bug in
  the old wake path that could double-queue a RUNNING parent.
- **sleep(ms): SYS_SLEEP (44).**  A timer-driven sleep queue parks the
  task; the BSP tick wakes due sleepers.  `kill_demo` in init uses it.

### Hardening
- **Kernel heap/data NX.**  The whole HHDM direct map (heap, kernel
  stacks, framebuffer, initrd, DMA bounce buffers) is marked NX at boot;
  the kernel image mapping is untouched.  Closes audit row 6's "kernel
  heap/data NX" gap.
- `sys_sigaction` accepted only signals < 8 (dead `sig == 9` check);
  now the full POSIX range 1..31 with SIGKILL/SIGSTOP excluded.
- Watchdog's hung-task scan only flags READY-but-starved tasks;
  BLOCKED (sleeping/waiting) tasks are no longer falsely reported.

### Cleanup
- Removed dead code: `kernel/fs/config.c` (config was parsed at boot and
  read by nobody after the GUI moved to ring 3), `kernel/fs/elf.c`
  (superseded by `arch/x86_64/user.c`), `userland/init.asm`
  (superseded by start.c), `patch_settings.py` (patched a deleted file).
- `docs/BRUTAL_AUDIT.md` rows 9 and 11 updated to match the fixed tree;
  ARCHITECTURE.md gained a status banner (it described a pre-SMP,
  in-kernel-GUI kernel for ~10 stages).

## 0.4.0-quartz  (Stage 9 - real assets, real fonts, real fps)

The "polish + bugfix" release.  Three concrete bugs from the previous
build are fixed; the look is upgraded across the board.

### Fixed
- **All text was capitalized.**  The old `gen_font.py` emitted lowercase
  glyphs as identical copies of their uppercase counterparts.  The new
  generator renders DejaVu Sans Mono Bold @ 13pt directly from the TTF
  on the build host, with a high alpha threshold to avoid antialias
  fringe pixels turning into spurious dots.  Every printable ASCII
  glyph is now distinct, has a real lowercase shape, and descenders for
  g/p/q/y fit cleanly in the 8x16 cell.
- **Freeze after boot.**  `desktop_tick()` was redrawing every PIT
  interrupt (100 fps target), overwhelming TCG.  Now there's a 30 fps
  cap (`FRAME_INTERVAL_MS = 33`) plus a global `g_dirty` flag that any
  state change flips.  Skipped frames let `hlt` actually sleep.  Apps
  that need continuous repaint (clock, sysmon, terminal caret, editor)
  set `WIN_ANIM` and the compositor knows to redraw them every frame.
- **Drawer pixel-darken was O(W*H) per frame.**  Rewritten as a
  bit-trick `(rgb >> 1) & 0x007F7F7F` halve-each-channel pass: one
  shift + one AND per pixel instead of three masks/shifts/divides.
  Still touches every pixel but at a fraction of the per-pixel cost.
- **PIT IRQs stopped firing after `/bin/init` exited.**  When `sys_exit`
  longjmp'd back to the kernel from inside the int 0x80 ISR, we
  inherited IF=0 (CPU clears IF on int entry).  Added an explicit
  `sti()` after the user-mode launch path in `kmain` so the desktop
  loop's `hlt` actually wakes on the next tick.

### New: real raster assets
- **`scripts/gen_assets.py`** generates three C source files at build
  time, holding raw ARGB pixel arrays:
  - `kernel/gui/asset_icons.c`     - 12 icons, 32x32 each, drawn with
    Pillow shapes (folders, terminal, editor with pencil overlay,
    clock with hands, calculator with keypad, monitor with live bars,
    home, etc.).
  - `kernel/gui/asset_cursor.c`    - 12x18 mouse pointer.
  - `kernel/gui/asset_wallpaper.c` - 480x300 wallpaper with vertical
    gradient + dot grid + warm corner glow + cool counter-glow.
- ASCII-art icons removed.  Cursor is no longer a hand-drawn arrow;
  it is a clean Pillow-rasterised pointer with shadow.
- Wallpaper is stretched to the framebuffer with nearest-neighbour
  sampling (one tight memory walk per frame, gated by the dirty flag).
- New `draw_icon_sized(x, y, id, size)` API for arbitrary scaling
  (used in the title bars at 18 px and the app drawer at 48 px).

### Changed
- Dock cell size grew to 48 px to accommodate 32 px icons.
- Title bars use a smaller 18 px icon.
- File manager bookmarks use 22 px sidebar icons; file/folder grid
  uses 32 px native.
- `kernel/gui/icons.c` rewritten to do alpha blending against the
  backbuffer for proper anti-aliased edges.
