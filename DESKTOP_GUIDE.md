# YartOS v4 GNOME-inspired Desktop Guide

## How to switch desktops (workspaces) - now unique like GNOME

**Old way:** F1, F2, F3, F4 (still works but deprecated)

**New GNOME-inspired ways:**

1. **Activities Overview (primary)**
   - Click **Activities** pill top-left or press **Super** (F1 also toggles)
   - Screen dims to `Activities Overview` showing workspace thumbnails (200x120) centered
   - Each thumbnail shows windows inside (Nyra Terminal etc)
   - **Click a workspace thumbnail** to switch active workspace
   - **Click a window preview** inside thumbnail to focus that window + switch workspace + exit overview
   - Press **Esc** to exit overview without switching

2. **Workspace dots in top bar**
   - Left of center, near Activities: 2-6 dots (pill = active)
   - Click a dot to instantly switch workspace
   - Active workspace is pill with inner dark dot

3. **Tab cycling (like GNOME Alt-Tab)**
   - In normal desktop: **Tab** cycles windows z-order (bring next window to front)
   - In overview: **Tab** cycles workspaces (active moves right)
   - **Super+Tab** metaphor same as Tab

4. **Middle-click on top bar**: Switches to next workspace

## How to use top bar (unique, not GNOME copy)

- **Left**: Activities pill (90x20 rounded 10px, translucent white, turns dark when overview active)
- **Center**: Date `Aug 7` + Time `21:22:07` centered (GNOME centers clock, we center both). Click to open **Calendar popup** 320x300 centered top, month grid, today highlighted blue.
- **Right**: Aggregated system pill (single rounded 12px, 20% white). Contains Kora icons: battery, audio, network (wired/wireless). Icons tint white, WiFi active tint accent blue (#5BA7DF). Click to open **Quick Settings**.

## Quick Settings - WiFi & Ethernet clickable

Click right system pill -> popup 320x380 bottom-right:

- **WiFi row**: Rounded 12px, blue tint if connected. Text `Wi-Fi: Connected (amito2g)` or `Disconnected`. Click toggles:
  - If disconnected: Scan + connect to `YartNet` (simulated over e1000)
  - If connected: Disconnect
  - Below row: AP list `YartNet [WPA2] -42 dBm`, `HomeFiber-5G`, `CoffeeShop_WiFi OPEN` - clickable to connect
  - Implements your request "make wifi clickable"

- **Ethernet row**: `Ethernet: Connected` or `No cable`. Click toggles G_eth_up flag + OSD "Ethernet connected/disconnected". Uses same Kora `ICON_TRAY_NET_WIRED`. Clickable like real OS.

- **Sound row**: `Sound: 98%` with slider visual (placeholder)

- **Dark Style, Night Light, Power Mode, Bluetooth, Airplane** rows like GNOME quick toggles (visual).

## Apps Grid - GNOME creative apps tab

**Dock "Show Apps" (9-dot grid icon)**:

- Clicking Show Apps toggles **Apps Grid** overlay (like your screenshot "Type to search")
- Top center: Search bar 400px wide rounded 20px `Type to search` — typing filters apps (checks name contains)
- Grid: 8 columns, icon 64x64 rounded 12px with Kora colors, name below (ellipsized)
- Apps list from `/bin`: Nyra Terminal (/bin/nyra), Hello (/bin/hello), etc. Dynamically enumerated via getdents in real build, static demo now includes: Additional Drivers, Firmware Updater, Image Viewer, Clocks, Language Support, Power Statistics, Software & Up, etc (like GNOME)
- **Pinned apps**: Nyra Terminal is pinned by default. Other apps can be pinned.

### Pin / Unpin (like real OS)

- **Right-click (button 3) on dock icon** (except Show Apps, Trash):
  - Context menu 120x60 popup: `Unpin`, `Uninstall`, `New Window`
  - **Unpin**: Removes from G_dock, rebuilds dock sprites, saves to `/home/yart/dock.conf`
  - **Uninstall**: `unlink(path)` + unpin + OSD "Uninstalled"

- **Right-click on app grid icon**:
  - Context menu: `Pin to Dock`, `Uninstall`
  - **Pin to Dock**: Adds to G_dock before Show Apps, rebuilds, saves config, OSD "Pinned to Dock"
  - **Uninstall**: Unlinks binary, removes from grid, OSD

- **Dock config file** `/home/yart/dock.conf` format: one path per line, loaded at wm startup, saved on pin/unpin. Persisted via FS v4 (double/triple indirect).

### Uninstall creative way

Like GNOME Software, but in our OS:
1. Open Apps Grid (Show Apps)
2. Right-click app -> `Uninstall`
3. WM asks via OSD "Uninstalling..." + calls unlink + removes icon
4. If app is pinned, also unpins
5. If app is running, kills PID first

Future: drag app icon to Trash to uninstall (like macOS), or drag to dock to pin.

## Boot & Test (screenshot)

Since QEMU not available in sandbox, we generated mock screenshots:

- `docs/screenshots/yart-desktop-v4-gnome.png` - Shows Activities Overview with workspace thumbnails, top bar with centered clock, right quick settings open showing WiFi clickable `Wi-Fi: Connected (amito2g)` and `Ethernet: Connected`
- `docs/screenshots/yart-apps-grid-v4.png` - Shows GNOME-like app grid with "Type to search" bar and 23 apps in 8 columns, like your screenshot.

To test for real:
```bash
./bootstrap.sh
make iso
./scripts/run-qemu.sh
# Click Activities top-left
# Click date/time center -> calendar
# Click system pill top-right -> quick settings, click WiFi row
# In Nyra Terminal: wifi scan, wifi status, wifi connect YartNet
# Drag window title bar to move, bottom-right + to resize, X to close (no freeze)
# Right-click app in grid (Show Apps) -> Pin
# Right-click dock icon -> Unpin/Uninstall
```

## Filesystem MAX v4 Advanced

- **2048 inodes**, triple indirect: 32 + 32*128 + 128*128 + 128^3 = 32+4096+16384+2097152 = 2117664 blocks = **1 GiB theoretical, capped 32 MiB disk** => can fill whole disk with one file
- **Link count** in reserved[2] (hardlinks), **flags** in reserved[3] (extent, symlink)
- **Symlink type** BLKFS_TYPE_SYMLINK stores target path
- **Journal CRC**: header crc of data for integrity
- **Extent hint**: data_alloc_contiguous() tries to find run of n contiguous free sectors for large files (like ext4 extents, but simplified)
- **Selftest**: 512 KiB + 20 MiB file (proves triple indirect) + symlink + 2048 inodes
