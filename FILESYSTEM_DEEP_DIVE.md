# YartFS v3 vs Real OS Filesystems (ext4, btrfs, etc.)

## What is SAME as a real OS

- **Inode model**: Each file is an inode with path, size, uid, mode, blocks. Like Unix V7/ext2.
- **Block pointers**: Direct (0-31) -> 16 KiB, Singly indirect (32 tables *128 = 4096 blocks = 2 MiB), Doubly indirect (1 table *128 indirec *128 data = 16384 blocks = 8 MiB). Total ~10 MiB/file = similar to ext2's triple indirect concept.
- **Bitmap allocation**: Inode bitmap (1024 bits) + data bitmap (one bit per sector) like ext2 block group bitmap.
- **Superblock geometry**: Validated at mount, non-overlapping regions: super, inode bitmap, inode table (1 sector per inode), data bitmap, data area, CRC table, journal. Like real superblock validation.
- **Crash consistency**: Write-ahead journal (30 records, multi-record redo) with sequence numbers, replay on mount. Similar to ext3/ext4 journal but simpler.
- **Bit-rot detection**: Per-data-sector CRC32 table (like btrfs checksums, but CRC32 not SHA). Verified on load, selftest checks.
- **Permissions**: Unix rwx owner/group/other + sticky + ACL per-user (8 entries) + umask + setuid/setgid/doas with salted SHA-256.
- **VFS layer**: RAM vnode tree is working copy, dirty_b0..dirty_b1 incremental dirty range (only changed 512B blocks written), sync every 100 ticks. Like Linux dentry cache.
- **Discard on delete**: Zero sector + CRC clear on free (like TRIM privacy).
- **Swap reservation**: Top of disk reserved for swap tier (like real swap partition).

## What is STILL MISSING vs real OS (honest)

| Feature | Real OS (ext4/btrfs) | YartFS v3 | Gap |
|---|---|---|---|
| **Extents** | ext4 uses extents (contiguous runs) not block pointers, faster large files | Only block pointers, no extents | Large files fragment more, slower sequential |
| **Directory indexing** | htree hash for fast lookup in large dirs | Linear linked list of children, O(n) lookup | Slow ls on 1000 files |
| **Hardlinks & symlinks** | inode link count, symlink inode type | No link count, path is full path stored in inode (no hardlink), symlink not implemented | Can't ln, can't symlink |
| **Journal checksums** | ext4 journal has checksums, commit blocks | Simple magic+seq, no checksum on journal header, data limited to 98 sectors | Journal corruption not detected |
| **Delayed allocation & writeback** | dirty pages in page cache, writeback threads | Immediate write-through on fsync + periodic sync, no page cache writeback | More disk I/O |
| **Fsck / recovery** | e2fsck scans and repairs | Only geometry validation, reformats on version mismatch, no repair | Corruption -> reformat |
| **Quotas, xattrs, ACL rich** | POSIX ACL, xattr, project quotas | Only 8 simple ACL entries, no xattr | No SELinux labels |
| **Compression, snapshots** | btrfs/zfs compression, snapshots | None |  |
| **Large file >10 MiB** | ext4 up to 16 TiB | Capped at 20512 blocks ~10 MiB (disk is 32 MiB) | Can't store 100 MiB ISO |
| **Mmap file cache coherency** | mmap'd files coherent with page cache | mmap arenas are anonymous only, files read via vfs_read into heap | Can't mmap a file directly |
| **Directory fsync durability** | fsync dir ensures rename durability | Dir ops are via journal but no explicit dir fsync | Might lose rename on crash (though journal helps) |
| **Case sensitivity, Unicode** | UTF-8 normalized, casefold optional | Raw byte strcmp, no normalization |  |

## Why 10 MiB is "MAXIMUM" for this OS
- QEMU disk default 32 MiB (yart-disk.img). Data area = total - inode table (1024) - bitmaps - CRC - journal 128 - swap reserve (~256 sectors). Leaves ~60k sectors ~30 MiB usable.
- One file 10 MiB = 33% of disk, so max sensible. With triple indirect (reserved[1]) we could reach 1 GiB theoretical but disk too small.
- 1024 inodes allows ~1000 files, more than original 512, pushing inode table to max before data area shrinks too much.

## Mechanics Same As Real OS

- **Fork CoW**: Like Linux, private PML4 per process, CoW on fork, demand paging, guard page between mmap regions.
- **Scheduler**: Per-CPU FIFO + work stealing like Linux CFS simplified, SMP IPI wake.
- **Net**: e1000 + ARP/IPv4/ICMP/UDP/DHCP/TCP/TLS 1.2 like real stack, routing table longest-prefix, firewall rules, loopback queue to avoid deadlock (same as real lo).
- **Mouse/Keyboard**: PS/2 IRQ -> per-task input queues with fanout lock like Linux input subsystem.

## Top Bar - GNOME Inspired but Unique

Not a GNOME clone, but takes ideas:
- **Left: Activities pill** (rounded, highlights when overview active) + workspace dots (active is pill not dot)
  - GNOME has Activities top-left, we have Yart logo + Activities text pill that glows.
  - Click toggles **Overview** - dims wallpaper with C_OVERVIEW_BG 80% dark, shows workspaces as large 200x120 thumbnails centered, like GNOME overview grid.
  - Workspace switching in overview: click thumbnail switches active workspace, click window preview focuses + exits overview. Unique: windows distributed across workspaces via pid hash, visible as small bars.

- **Center: Date/Time centered** (GNOME centers clock). Format: "Aug 07" + "21:22:07" centered, not right. Click opens calendar popup (320x300) with month grid, today highlighted in accent blue, like GNOME calendar.

- **Right: Aggregated quick settings pill** (single pill grouping net/wifi, audio, power) like GNOME 43+ quick settings.
  - One pill background with 20% white translucency, contains Kora icons (battery, audio hi/mute, network wired/idle) tinted white, WiFi active tinted accent blue.
  - Click opens **Quick Settings popup** 320x380 bottom-right of panel, rounded 16px, shadow, with rows:
    - WiFi row: shows connected/open, click toggles scan/connect/disconnect to YartNet, shows AP list (clickable names) — makes WiFi **clickable**
    - Ethernet row: shows "Ethernet: Connected" if e1000 link up, click toggles G_eth_up flag + OSD — makes **Ethernet clickable**
    - Sound slider placeholder row (98%)
    - Dark Style toggle, Night Light, Airplane Mode, Auto Rotate rows (visual, like GNOME quick toggles)
  - Each row is rounded 12px, hover highlight, Kora icons.

- **Kora icons**: Everywhere — dock (Nyra Terminal reuses settings icon but tinted native), tray uses Kora symbolic icons (battery/network/audio) recolored white for panel, quick settings uses same icons.

- **Modern font**: DejaVu Sans Mono 14px antialiased via PIL, alpha per pixel (0-255) blended in sf_putc. Old 1-bit bitmap replaced. Font is modern mono, clean curves, like GNOME's Cantarell/Monospace but mono for terminal.

- **Desktop switching unique vs F1-F4**:
  - Primary: **Activities overview** — click Activities or press `Super` or `F1`? We kept F1 but added overview as main. In overview, workspaces shown as horizontal strip (GNOME shows vertical). Click workspace to switch.
  - Secondary: **Tab cycles** — Tab in normal mode cycles windows (Alt-Tab like), Tab in overview cycles workspaces.
  - Tertiary: Workspace dots still clickable (pill active). Middle-click on panel switches workspace.
  - No longer F1-F4 exclusive — F1 now toggles overview (GNOME-style), not direct workspace. F2/F3 add/remove workspace (like GNOME dynamic workspaces).

## Result
Top bar feels GNOME but unique: Activities pill + centered clock/date + single system pill -> overview + quick settings + calendar popups, all with Kora icons, modern AA font, smooth 60fps, WiFi/Ethernet clickable.
