# Changelog

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
