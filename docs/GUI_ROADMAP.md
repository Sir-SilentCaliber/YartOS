# YartOS GUI — Brutal Enterprise Roadmap

Honest assessment of every problem between the current compositor and a
smooth, GNOME/macOS-class desktop. Ordered by impact. Items marked **[DONE]**
are fixed; **[NOW]** are tackled first; **[ ]** remain.

---

## 0. Blocker bugs  (must be 0 before anything else matters)

- **[DONE] Double keypress (every key typed twice).**
  `kbd_init()` unmasked the 8259 PIC after `apic_init()` switched to the
  IOAPIC, so QEMU delivered IRQ1 twice. Fixed: only unmask PIC when APIC
  is unavailable.
- **[DONE] WM crash / SIGSEGV on boot.**
  `cursors.c` stored theme names as pointers to a stack array; `cursors_theme_by_name`
  dereferenced freed/garbage memory. Fixed with static storage.
- **[DONE] Apps unusable — no keyboard focus on launch.**
  `scan_windows()` now calls `wm_focus(owner_pid)` when a window first appears.
- **[DONE] Window blink on open.** Opening animation faded from alpha=0 causing
  a visible flash. Changed to scale-only (no alpha) on open.
- **[DONE] virtio-blk DMA buffer overflow.** 4 KiB bounce buffer accepted
  4 MiB transfers → memory corruption → page faults. Capped + chunked I/O.
- **[DONE] blkfs block-sector-0 treated as failure.** Valid first block was
  rejected; fixed.
- **[ ] Window close sometimes leaves zombie surface** — verify the 2-miss
  reap path tears down owner pid in all cases.
- **[DONE] Keyboard dead with nothing focused.** `sys_input_kbd()` called
  `sched_find(g_focus_pid)` with `g_focus_pid==0`, which resolves to the
  *idle* task (pid 0) — so every keystroke went to the idle task's queue and
  the WM never saw it. Fixed by gating the lookup on `g_focus_pid != 0`.
- **[DONE] Release events double-processed.** The WM/apps tested `ev & 0x80`
  (bit 7) but the kernel sets `KEY_RELEASE` at bit 16, so every release was
  treated as a second press. Fixed to `!(ev & (1<<16))`.
- **[DONE] Enter in launcher/terminal.** The keyboard produces LF (0x0A) for
  Enter, but the WM checked for CR (0x0D) only. Both are accepted now.
- **[DONE] PS/2 keyboard disabled by UEFI.** OVMF leaves the 8042 keyboard
  disabled after boot services; `kbd_init()` now sends the enable command
  (0xAE) + command-byte fix. USB HID usage codes are also translated to
  PS/2 set-1 scancodes + ASCII in `kbd_enqueue()`.

---

## 1. Input & feel

- **[NOW] App launcher vs dock menu separation** (user-specified design):
  - Top-left Activities button → unique full-screen **app grid + workspaces**
    (GNOME-style: app tiles + virtual-desktop selector on the right).
  - Dock grid button → **vertical dropdown menu** of apps with a top search
    bar (not the same full-screen grid).
- **[ ] Cursor not smooth / pixelated.** The Roblox 2013 sweezy PNGs are
  128px palette PNGs; verify the crop/hotspot/scale. Consider rendering at
  native 32px with the correct hotspot, and add a drop shadow.
- **[ ] Mouse acceleration curve.** Current driver has basic accel; add a
  proper GNOME-style adaptive flat acceleration + pointer speed setting.
- **[ ] No drag threshold.** A click that moves 1px starts a drag; add a
  few-pixel slop so clicks don't accidentally drag windows.
- **[ ] No touchpad scroll / two-finger gestures** (future hardware).
- **[ ] Keyboard shortcuts incomplete.** Need Super to open launcher,
  Alt+Tab window switcher, Super+D show desktop, Super+L lock, volume keys.
- **[ ] No input method / IME.** No compose, no dead keys, no CJK input.

---

## 1.5 Skift strata-shell visual parity (2026-08-15)

The desktop now matches Skift's actual design system (studied from
`skift-org/skift` strata-shell + `skift-org/hideo` hideo-shell + `karm-ui`):

- **[DONE] Palette.** Exact Skift dark theme: ZINC ramp neutrals
  (`GRAY50=#fafafa` text → `GRAY950=#09090b` background) + `BLUE500=#3b82f6`
  accent, taken verbatim from `karm-gfx/pixels/colors.cpp`.
- **[DONE] Taskbar.** Skift's black bar with a "Search…" pill (left), the
  centered calendar button (`Aug. 14, 2026, 12:32`), and a status cluster
  (wifi / volume / battery / "100%") on the right.
- **[DONE] Window chrome.** Skift's minimal titlebar — app icon + title on
  the left, a single ✕ close button on the right (no traffic lights),
  1px `GRAY800` border, 8px radii, and an `elevated()`-style soft shadow.
  Min/max moved to double-click (maximize) and right-click (minimize).
- **[DONE] Launcher.** Skift's centered 500×440 `appsLauncher` panel
  (GRAY900 surface, GRAY800 border, elevated shadow) with a "Search for
  anything…" field and app rows using per-app colour-ramp tiles
  (border ramp[5] / background ramp[6] / icon ramp[1]).
- **[DONE] Font.** Replaced DejaVu Sans Bold with **Inter Regular** (Skift's
  UI font), antialiased 12×18 cells — fixes the "cartoony/thick" look.
- **[DONE] Wallpaper.** Skift's default `abstract` wallpaper (deep indigo)
  is now wallpaper index 0.
- **[DONE] App surfaces.** Nyra + all GUI apps recoloured to GRAY900/GRAY950
  surfaces, GRAY50 text, BLUE400 accent.
- **[DONE] Super key** opens the launcher (Skift: Super+Space); ESC closes.
- **[DONE] Drag slop** so clicks don't jiggle windows.

Not matched (honest): Skift's titlebar lives *inside* the app surface
(apps draw their own chrome via Karm `Scaffold`); YartOS draws chrome in the
WM around the app buffer. HiDPI and the Karm widget/event tree are not ported.

### Ninth wave (2026-08-15, cursor/icon/battery pass)

- **[DONE] Roblox cursor restored + pinned.** The environment had wiped
  kora/cursors/roblox-*.png (white arrow = cursor, dark arrow = pointer).
  Restored from /uploads and reapply_fixes.py now re-copies them if missing.
- **[DONE] Icon pixelation fixed.** sf_icon_scaled was nearest-neighbour;
  now bilinear-filtered with premultiplied-alpha, so the 22px tray icons
  scale to 30px smoothly instead of blocky.
- **[DONE] Top-right icons all equal size.** wifi/volume/battery are drawn
  with sf_icon_sz(...,30) — measured identical 36×36 ink bounding boxes.
- **[DONE] Honest battery.** SYS_BATTERY reports present/charging/level.
  QEMU q35 has no ACPI battery, so the panel now shows "AC" (wall power)
  instead of the fake "100%". When a battery exists, the full state machine
  (level%, low<20 accent, critical<10 red, charging) renders from the real
  value.

### Eighth wave (2026-08-15, de-faking — substance, same look)

The desktop LOOK is unchanged. These fix "fake stuff" with real mechanisms.

- **[DONE] Real lock auth.** SYS_AUTH_VERIFY checks the salted-SHA256
  account hash without elevating (shared with doas). Lock screen: Enter
  opens a password field, wrong password shows "Wrong password" and stays
  locked, correct password ("yart") unlocks. Verified in QEMU.
- **[DONE] Real volume.** SYS_AUDIO_VOL drives the HDA codec's output-amp
  gain verb (0.75 dB steps + mute). The quick-settings slider and Settings
  app now actually change the hardware mixer instead of a dead variable.
- **[DONE] Real, persistent Settings.** The Settings app writes
  /home/yart/settings.conf (accent/cursor/wallpaper/dock/volume); the
  compositor re-reads it every 2s and applies it. Verified: changing
  "Wallpaper" in Settings really changes the desktop wallpaper.
- **[DONE] Settings keyboard nav fixed.** Arrow scancodes arrived with the
  E0 flag in bits 8..15 (`ev>>8` = 0x104D), so Left/Right/Up/Down never
  matched. Now masked to the raw scancode.
- **[DONE] Real notifications (pipeline).** SYS_NOTIFY / SYS_NOTIFY_POLL
  implement a kernel notification ring; the Console has a `notify <msg>`
  command; the WM drains the ring, toasts, and lists notifications in the
  calendar instead of "all caught up". (Verified end-to-end via kernel +
  WM logs.)
- **[DONE] Calendar opens on the real month/year** (was hardcoded Aug 2026).
- **[DONE] Dock visibility is a real setting** (settings.conf dock=0/1).

Known remaining bug (honest): the OSD toast's on-screen rendering does not
reliably appear (the draw path runs but the pixel result is not landing in
the presented frame); notifications still land in the calendar list. The
fade animation was removed to isolate it — root cause not yet found.

### Seventh wave (2026-08-15, user corrections 2)

- **[DONE] Roblox 2013 cursors as default.** The uploaded SweezyCursors
  photos (white arrow = idle, dark arrow = pointer/hover) are now the
  system default cursor theme ("roblox"), replacing the Skift vector arrow.
- **[DONE] Bigger top-bar status icons** (26px wifi / volume / battery).
- **[DONE] Dock background.** The dock now has the same translucent
  GRAY950 pill + 1px GRAY800 outline as the top bar (consistent chrome).
- **[DONE] Removed About and Hello** from the launcher (Console, Files,
  Settings, Text remain).
- **[DONE] Skift right-click menus.** Empty desktop → "Personalize..." +
  "Settings". Window titlebar → Restore / Maximize / Minimize / Snap Left /
  Snap Right / Close.
- **[DONE] Skift titlebar controls.** Minimize / Maximize / Close buttons
  on the right (subtle: transparent, GRAY300 glyphs, GRAY700 hover), with
  MDI-style line glyphs (bar / square / X) and restore glyph when maximized.

### Sixth wave (2026-08-15, user corrections)

- **[DONE] One icon registry.** The launcher no longer renders monochrome
  "ramp tiles"; every surface (dock, launcher, dock dropdown, Alt+Tab,
  Overview, window titlebar) now draws the SAME colour Kora icon via the
  app's single icon id — same app, same icon, everywhere.
- **[DONE] Dock pill removed.** The dock is now bare icons floating on the
  wallpaper (no frosted rectangle, no border ring). Hover highlight and
  running-dots remain.
- **[DONE] Bigger top-right status icons** (22px wifi/volume/battery/100%).
- **[DONE] Show Desktop actually works.** It previously called
  wm_move(-10000,-10000), but the kernel clamps positions to x>=-100 — so
  windows just piled up in the top-left corner and could never be restored.
  Now `hidden` is a compositing flag: F3 hides all windows (skips painting
  and hit-testing), F3 again or clicking their dock icon restores them.
- **[DONE] Calendar shows the real month/year** (was hardcoded to Aug 2026).
- **[DONE] Terminal branding**: "Welcome to Console" / "Console Help".

### Fifth wave (2026-08-15, detail pass — user-requested)

- **[DONE] Top bar matches Skift.** No search pill; translucent GRAY950@60%
  over the wallpaper (see-through, not opaque); date+time is the only
  centred item; wifi / volume / battery / "100%" icons at the top-right in
  Skift's order.
- **[DONE] Skift date format.** "Aug. 15 2026, 12:32" (no comma before the
  year) and it updates from the real RTC — QEMU now boots with
  `-rtc base=localtime` so the bar shows today's actual date/time.
- **[DONE] Proportional font.** The UI font was monospaced (every glyph in a
  12px cell), which is why letters looked too far apart. It now uses
  per-glyph advance widths (Inter Medium metrics), so text is tight and
  natural, with caret placement updated to match.
- **[DONE] Desktop icons bare.** Removed the black label backdrop; the name
  now sits directly under the icon (raised ~3px closer).

### Fourth wave (2026-08-15, visual parity)

- **[DONE] Skift vector cursor.** The desktop now uses Skift's actual cursor:
  black fill + white 1.6px stroke, the rounded-arrow path copied verbatim
  from strata-shell's `defs/rounded-cursor.path`, plus the classic arrow for
  interactive elements and the 4-way resize cursor over window edges.
  Rasterised at native 28-34px with exact hotspots (a `.hot` sidecar), so it
  matches Skift pixel-for-pixel. (Also fixed a latent blob-header off-by-one
  in cursors.c that had been reading the version field as the theme count.)
- **[DONE] Inter Medium font.** Regenerated the UI font from Inter Medium
  (Skift's label weight) instead of Inter Regular — crisper, less thin.
- **[DONE] Skift app names.** Launcher/dock/window titles now use Skift's
  vocabulary: Console (terminal), Files, Settings, Text (editor), About.
- **[DONE] Skift abstract wallpaper restored as index 0** (the workspace
  environment had reverted it; `scripts/reapply_fixes.py` now re-fetches and
  re-applies it idempotently).

### Third wave (2026-08-15, architecture)

- **[DONE] Module split.** `wm.c` (1478 lines) is split into focused modules,
  each owning one screen concern, sharing `wm.h`:
  `wm_damage.c` (dirty-rect list), `wm_windows.c` (window state machine +
  chrome + animations), `wm_dock.c` (dock + desktop icons + frosted glass),
  `wm_panel.c` (taskbar), `wm_launcher.c` (launcher + search),
  `wm_overlays.c` (menu, quick settings, calendar, OSD, switcher, overview),
  with `wm.c` as the orchestrator (main loop, input, cursor, backdrop,
  config, processes).
- **[DONE] Lock screen.** Super+L locks (dark veil + clock + date +
  "Press Enter to unlock"); Enter unlocks. Clicks are ignored while locked.
- **[DONE] Super-as-modifier tracking** (for Super+L, matching Skift).

Still not Skift-parity (honest): the modules are C files, not a Karm-style
node/event tree; there is no IPC (direct syscalls); window chrome is
WM-drawn rather than app-drawn (Scaffold); no HiDPI; no widget toolkit.

### Second wave (2026-08-15, later)

- **[DONE] Frosted-glass dock.** The backdrop behind the dock pill is
  cached, gaussian box-blurred (radius 12, 2 passes) and lightened — a real
  backdrop filter, rebuilt only when the backdrop changes (not per frame).
- **[DONE] Alt+Tab live previews.** The switcher now shows scaled live
  previews of each window's surface (nearest-neighbour `sf_blit_scaled`),
  with an accent ring on the selected card.
- **[DONE] Mission-Control-style Overview.** Ctrl+Alt+Up/Down opens a
  zoomed-out grid of large live window previews; click to focus, Esc to
  close. (Previously unreachable; now wired up.)
- **[DONE] Minimize-to-dock animation.** Right-clicking a titlebar flies
  the window toward its dock icon (scale+fade+translate, 240ms); clicking
  the dock icon springs it back open.
- **[DONE] Springy window open.** Open animation now uses an ease-out-back
  curve (tiny overshoot settle).
- **[DONE] Resize from the top edge** (was only left/right/bottom).
- **[DONE] Global shortcuts while an app is focused.** The kernel now fans
  keyboard events to the WM *and* the focused app (Skift strata-shell model:
  the shell sees every key), and the WM stops routing to the app while any
  overlay is open — so Alt+Tab / Super / F1 / Ctrl+Alt+arrows work even with
  a terminal focused, and launcher typing doesn't leak into the app.
- **[DONE] Launcher search reset** on open (stale query no longer blocks
  launching the same app twice).

---

## 2. Rendering architecture  (the "smooth" part)

- **[DONE] Real damage tracking / dirty-rectangles (Skift strata-shell model).**
  The compositor now keeps a `Vec<GfxRect> G_dirty` list with collide-merge
  (`damage_add`), a backdrop buffer (`G_backdrop` = wallpaper + desktop icons,
  rebuilt only when dirty), and a `composite_rect(r)` pass that sets the
  global drawing clip to each damaged rect (Skift `g.clip(r)`) and repaints
  only the intersecting windows/overlays. Cursor movement damages only the
  prev+new cursor rects.
- **[DONE] Frame pacing.** Fixed 60 Hz pacing: the loop sleeps until
  `lastFrame + 16ms` (Skift `sleepAsync(lastFrame + 16ms)`) instead of
  spinning on `yield()`.
- **[DONE] Partial present.** New `SYS_FB_PRESENT(addr, rects, n)` syscall
  and kernel `fb_present_rects()` copy only the damaged rectangles from the
  back buffer to the scanout (Skift `blitUnsafe(front.clip(r), back.clip(r))`),
  replacing the whole-framebuffer copy.
- **[DONE] Wallpaper blit every frame.** The static base scene is baked once
  into `G_backdrop`; only damaged rectangles are blitted out of it.
- **[ ] No hardware acceleration.** Everything is software blit. For
  enterprise smoothness this is acceptable on QEMU but will need a GPU
  driver (virtio-gpu) eventually. Tracked as a future milestone.
- **[ ] No shadow/alpha caching.** Window shadows are recomputed every
  frame. Cache the window decoration surface.
- **[ ] No real page flip** — the partial present is still a copy (no
  vsync/scanline sync); a real double-buffered flip would avoid tearing.

---

## 3. Window management

- **[DONE] Virtual desktops / workspaces.** 9 workspaces, Ctrl+Alt+Left/
  Right to add/switch, panel dots, per-workspace window assignment.
- **[DONE] Alt+Tab window switcher** (with app icons).
- **[DONE] Window snapping.** Drag to left/right edge → half-tile; drag to
  top → maximize; drag away → restore.
- **[ ] No minimize-to-tray / window list.** Minimized windows have no
  visible affordance to restore (only dock running-dot).
- **[ ] No window resize from all edges/corners.** Only bottom-right grip.
- **[ ] No always-on-top / fullscreen / shade states.**
- **[ ] No parent/modal dialogs.**
- **[ ] Z-order is a flat list; no window grouping.**
- **[ ] Title bar text can overflow on narrow windows.** Ellipsize.
- **[ ] No window animations for minimize-to-dock** (genie effect).

---

## 4. App launcher / grid / dock

- **[NOW] Two launchers as described above** (see §1).
- **[ ] App grid is a fixed 5-col grid** with no pagination, no folders,
  no drag-to-reorder. GNOME has pagination + app folders.
- **[ ] No favorites pinning drag-reorder** in the grid.
- **[ ] App search only filters names.** Should search .desktop comments,
  keywords, and execute actions.
- **[ ] No recently-used / frequently-used apps.**
- **[ ] Dock running indicators are tiny dots.** Add a window-count
  indicator and progress bars (like Unity).
- **[ ] Dock doesn't autohide.**
- **[ ] No dock peek / window preview on hover.**
- **[ ] Tooltips appear after 350 ms but have no animation/hysteresis.**
- **[ ] Trash doesn't open a real trash location** (just shows empty OSD).

---

## 5. Panel / system tray / indicators

- **[ ] No real clock/calendar applet.** Calendar is hardcoded August 2026
  and shows no events; clock is correct from RTC but has no timezone.
- **[ ] No notifications center / notification daemon.**
- **[ ] No network menu** (list Wi-Fis, connect, VPN).
- **[ ] No audio/sound menu** with device selection and per-app volume.
- **[ ] No battery indicator.**
- **[ ] No Bluetooth menu.**
- **[ ] No power menu** (restart/shutdown/suspend/lock).
- **[ ] Quick Settings redesign** started (icons + volume slider) but needs
  the Wi-Fi menu, brightness slider, and power buttons.
- **[ ] No system tray protocol** (XEmbed/SNI equivalent) for apps to add
  icons.
- **[ ] Panel height fixed at 30 px; not HiDPI-aware.**

---

## 6. Menus & context

- **[ ] Menus have icons now** but no keyboard navigation (arrows,
  Enter, Escape, accelerator underlines).
- **[ ] No submenus.**
- **[ ] No separators / sections.**
- **[ ] Menus don't close on outside-click reliably across overlays.**
- **[ ] Right-click desktop menu is minimal** (terminal, wallpaper, show
  desktop). Needs New Folder, Paste, Display Settings, Change Background.
- **[ ] Right-click file/folder in Files needs Open/Open with/Cut/Copy/
  Paste/Rename/Delete/Properties.**

---

## 7. Files app (Files.elf)

- **[ ] It is currently a minimal directory lister, not a file manager.**
  Missing: navigation buttons (back/forward/up), path bar breadcrumbs,
  sidebar (Home/Documents/Downloads/Trash/Volumes), icon view, file
  selection (single+range), copy/cut/paste, rename (F2), delete (Del),
  new folder (Ctrl+Shift+N), properties, drag-and-drop, thumbnails,
  executable prompt, file associations.
- **[ ] No MIME icon resolution** (some icons fall back).
- **[ ] No file monitoring / live refresh.**
- **[ ] No trash / restore.**
- **[ ] No search.**

---

## 8. Text Editor (editor.elf)

- **[ ] Minimal line editor.** Needs: multi-buffer, line numbers,
  word wrap, find/replace (Ctrl+F), syntax highlighting, save-as,
  dirty-dot, Ctrl+S, Ctrl+Q, selection, copy/paste, undo/redo,
  auto-indent, monospace font.

---

## 9. Terminal (nyra)

- **[DONE] Prompt moved to top after output** (was pinned to bottom).
- **[ ] No ANSI escape parsing.** No colors, cursor movement, clear
  screen. Real terminals need a VT100/VT52 parser.
- **[ ] No scrollback buffer / scrollbar / scroll wheel.**
- **[ ] No selection / copy/paste.**
- **[ ] No tabbed terminals.**
- **[ ] No configurable font size / colorscheme.**
- **[ ] No blinking cursor toggle; block vs I-beam.**
- **[ ] Title says "Nyra Terminal [MAX FS + WiFi]"** — user wants just
  "Nyra". (Small fix, do it.)

---

## 10. Typography

- **[ ] Font is an embedded bold bitmap (DejaVu Sans Bold 10x18).**
  Described as "cartoony/thick". To reach enterprise look:
  - Embed a regular-weight sans at a smaller size (e.g. Inter/Noto Sans
    Regular 11-12 px with anti-aliasing), and
  - generate the `font_*.h` from a TTF with a proper rasterizer
    (stb_truetype or a pre-baked atlas at build time).
- **[ ] No font fallback** for missing glyphs (CJK, emoji).
- **[ ] No subpixel / hinting options.**
- **[ ] No HiDPI scaling** — all sizes are hardcoded px.

---

## 11. Theme & customization

- **[DONE] Theme engine** (theme.h/c) with 32 semantic colors + ini loader
  + F4 live reload.
- **[ ] Need a Settings GUI** to pick colors/accent/wallpaper/cursor/dock
  size/font, instead of hand-editing theme.ini.
- **[ ] No icon theme switching.**
- **[ ] No cursor theme switching / size.**
- **[ ] No dark/light auto mode.**
- **[ ] Window decoration radius/shadow not themeable yet.**

---

## 12. Desktop icons

- **[ ] Not interactive filesystems objects.** Double-click should open
  the file/folder; right-click should give the file menu.
- **[ ] No drag-align to grid with live reflow.**
- **[ ] No rename inline (F2).**
- **[ ] No rubber-band multi-select on desktop** (marquee exists but
  doesn't act on desktop icons).
- **[ ] Icons are hardcoded** (Home/Documents/Downloads/Trash); user
  can't add arbitrary files/folders to desktop beyond "Add to Desktop".

---

## 13. Notifications & OSD

- **[ ] OSD is a single text line** (volume/brightness feedback needs an
  icon + bar).
- **[ ] No notification queue, history, or action buttons.**
- **[ ] No "do not disturb".**

---

## 14. Lock / auth / session

- **[ ] No lock screen.**
- **[ ] No login manager** (auto-login is fine for now, but enterprise
  needs a greeter).
- **[ ] No screen saver / display sleep.**
- **[ ] `doas` exists but no polkit-style auth dialog.**

---

## 15. Architecture / code health

- **[ ] wm.c is ~800 lines doing everything.** Split into:
  `wm/` (core, windows, focus), `dock.c`, `panel.c`, `desktop.c`,
  `launcher.c`, `menu.c`, `theme.c`, `anim.c`, `input.c`.
- **[ ] gfx.c is a single 270-line software renderer.** Split into
  blit, blend, text, primitives, rectangles.
- **[ ] No unit tests** for geometry/damage/list logic.
- **[ ] No logging levels**; klog/printf everywhere.
- **[ ] Hardcoded constants** (1280x800, sizes, timings) should come
  from a config/capabilities struct.
- **[ ] No assertions / invariants.**
- **[ ] Several `G_full=1` shotgun repaints** remain; replace with
  precise damage once damage tracking lands.

---

## 16. Reliability / enterprise checklist

- **[ ] No crash reporter.**
- **[ ] No update/AB partition mechanism.**
- **[ ] No system journal / persistent log.**
- **[ ] No backup/restore of settings.**
- **[ ] No multi-monitor.**
- **[ ] No localization/i18n.** English only.
- **[ ] No accessibility** (screen reader, high-contrast, large text,
  sticky keys, focus ring).

---

## Suggested execution order

1. **[NOW]** Launcher/dock-menu separation + Nyra rename + remove grid
   note — user-facing design correctness.
2. Damage tracking + vsync (the "smooth").
3. Window management: snap, Alt+Tab, minimize restore, all-edge resize.
4. Workspaces + Activities overview.
5. Files app to usable file manager.
6. Terminal ANSI + scrollback.
7. Font replacement (regular weight, proper raster).
8. Panel indicators + sound/network/power menus.
9. Module split of wm.c.
10. Lock/login, notifications, settings GUI.

