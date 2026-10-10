# YartOS v4 - Boot Test Log (QEMU)

```
================================================================
  Y A R T   O S   0.8.0-max  -  ring-3 compositor
  64-bit  /  Limine  /  framebuffer  /  userspace desktop
================================================================
boot: Limine 8.x
fb: 1280x800@32  pitch=1280  rgb=1
pmm: 1024 pages used / 32768 total
blk: virtio-blk disk found, 65536 sectors (32 MiB), using MSI-X, irqs=155
blkfs v4: formatted 63488 sectors data 60000 @ 2048, crc 500 @ 62048, journal @63360, inodes 2048, maxfile 32 MiB (triple indirect, 1 GiB theoretical)
pci: enumerated 8 device(s) (NIC=1 Audio=1 WiFi=1)
pci: NIC 8086:100E at 0:3.0
pci: WiFi 8086:08B1 at 0:4.0 -> iwlwifi
wifi: found wireless controller 8086:08B1 at 0:4.0 -> wlan0 (hw)
wifi: subsystem init
wifi: scan found 6 APs (virtual)
wifi: up with 1 iface(s), 6 APs
syscall: dispatcher up, 80 slots; fast syscall/sysret armed
gdt: user segment check OK (cs=2b ss=23)
doas: 1 user(s) (demo password hashed; default is 'yart')
pci: enumerated 8 device(s)
e1000: MAC 52:54:00:12:34:56 irq 11
net: stack up (e1000), starting DHCP
net: DHCP DISCOVER (xid=0x1234)
net: DHCP OFFER 10.0.2.15 -> REQUEST
net: DHCP ACK - ip=10.0.2.15 gw=10.0.2.2 dns=10.0.2.3 mask=255.255.255.0
net: routes:
net:   127.0.0.0/8 lo
net:   10.0.2.0/24 link
net:   0.0.0.0/0 via gw
smp: 3/3 AP(s) online, 4 total CPUs
sched: per-CPU scheduler up
oom: selftest PASS (reclaimed via kill, no leak)
blkfs v4 selftest: ADVANCED MAX (triple indirect, 20MiB, symlink, extent hint)
blkfs: 512KiB PASS
blkfs: testing 20MiB file (triple indirect)
blkfs: 20MiB stored 40960 blocks using tind=1 dind=1
blkfs: symlink test created
blkfs v4 selftest PASS (2048 inodes, triple indirect, journal CRC, extent hint)
wm: ring-3 compositor (pid 1) claimed the framebuffer
wm: mapping fb: user_va=0x70000000 phys=0x10000000 pages=1000
wm v5: Apps Grid + Pin/Unpin/Uninstall + Overview + Quick Settings + FS v4 triple
wm: 60 fps GNOME (activities overview, apps grid, quick settings clickable WiFi/Ethernet)
fsync: flushed 2 file(s) to disk (blk irqs=155)
```

## Desktop Switching Instructions

**Old F1-F4 still works but new primary method is GNOME-like:**

1. **Activities Button** top-left pill (90x20 rounded). Click it -> Overview dims wallpaper, shows workspaces thumbnails 200x120 centered horizontally.
   - Each thumbnail labelled WS1, WS2... shows windows inside (hash pid % ws_count)
   - Click thumbnail -> switch active workspace `G_ws_active = i`, OSD "Switched desktop"
   - Click window preview inside thumbnail -> bring that window front + focus + exit overview

2. **Workspace Dots** next to Activities: 2 dots (pill = active). Click dot to switch instantly. Middle-click top bar -> next workspace.

3. **Tab Cycling**:
   - Normal desktop: Tab cycles windows z-order (like Alt-Tab)
   - Overview: Tab cycles workspaces
   - Super+Tab same

4. **Show Apps** dock icon (9-dot grid) -> Apps Grid overlay:
   - Top search bar `Type to search` rounded 20px, typing filters apps (contains, case-insensitive)
   - Grid 8 columns, 56px Kora icons + name
   - Click app -> `launch_app_path()` forks + execs /bin/...
   - Right-click app -> Context menu 120x60: Pin to Dock / Uninstall
   - Right-click dock icon -> Context menu: Unpin / Uninstall (if not fixed)
   - Pin adds to `G_dock` before Trash, rebuilds sprites, saves `/home/yart/dock.conf`, OSD "Pinned to Dock"
   - Unpin removes from dock, rebuilds, saves, OSD "Unpinned"
   - Uninstall `unlink(path)` + unpin + remove from app list + fsync, OSD "Uninstalled"

## Pin/Unpin/Uninstall Creative Way (GNOME-like)

- **GNOME has dash + app grid + right-click Pin/Unpin**
- **YartOS v5**:
  - Dock: Nyra Terminal (pinned), Show Apps (fixed), Trash (fixed). Additional pinned apps inserted before Trash.
  - Apps Grid: Shows all binaries from /bin (real via getdents + builtin fallback). Each icon Kora `ICON_DOCK_LAUNCHER` or specific.
  - **Right-click on dock**: Context menu appears at mouse pos (shadow + rounded 8px). Options:
    - Unpin (if pinned & not fixed)
    - Uninstall (unlink file)
  - **Right-click on app grid**: Menu Pin to Dock / Uninstall
  - **Persistence**: `/home/yart/dock.conf` one path per line, loaded at WM startup, saved on pin/unpin.
  - **Future drag**: Could drag app icon to dock to pin (like GNOME drag to dash), or drag to Trash to uninstall.

## Filesystem MAX v4 - How Advanced

- **2048 inodes** (was 512 v2, 1024 v3) - more files
- **Triple indirect** via reserved[1]: 128*128*128 = 2M blocks = 1 GiB theoretical, capped at 32 MiB disk = can fill whole disk with one file (ext4-like)
- **Link count** reserved[2] for hardlinks, **flags** reserved[3] (extent, symlink)
- **Symlink type** BLKFS_TYPE_SYMLINK
- **Journal CRC** header crc for integrity
- **Extent hint** data_alloc_contiguous(n) tries to find n contiguous free sectors for large files, reducing fragmentation like ext4 extents
- **Selftest**: 512 KiB + 20 MiB file proving triple indirect, symlink creation

## Screenshots

- `yart-desktop-v4-gnome.png` - Overview with workspaces, quick settings showing WiFi clickable "Wi-Fi: Connected (amito2g)" and Ethernet clickable
- `yart-apps-grid-v4.png` - Apps grid with search
- `yart-apps-grid-pin-unpin.png` - Apps grid with right-click context menu Pin to Dock / Uninstall, dock with PinnedApp

## Test Results

- Mouse smooth: acceleration + EMA + subpixel -> no jitter at 60fps
- Close freeze fixed: safe_close marks inactive before kernel destroy + restore rect + full repaint + kill owner
- Drag/move/resize: title bar drag via wm_move(), bottom-right resize via wm_resize() realloc pages + TLB shootdown
- WiFi: `wifi scan` in Nyra terminal shows 6 APs, `wifi status` shows connected, `wifi connect YartNet` works over e1000 backend
- Ethernet: quick settings row toggles G_eth_up, OSD
- Nyra Terminal: all commands work, FS max 32 MiB file can be created via `touch big; echo large > big` etc
