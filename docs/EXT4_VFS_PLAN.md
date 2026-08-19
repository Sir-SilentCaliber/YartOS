# YartFS → ext4-architecture migration plan

Date: 2026-08-18
Author: Arena.ai Agent Mode assistant

## 0. Honest framing

"Just like ext4" means two different things, and I want to be precise about
which one we're doing:

- **A literal ext4 driver** (read/write actual ext4 volumes) is ~50,000 lines
  in Linux (super.c, extents.c, balloc.c, ialloc.c, dir.c, namei.c, inode.c,
  journal via jbd2 ~15k more). That is out of scope for YartOS — a real ext4
  driver is a multi-month project and would still need jbd2, compat checks,
  and years of edge cases.
- **An ext4-ARCHITECTED filesystem** — i.e. "YartFS v5" that uses the same
  on-disk design ideas as ext4 (block groups, an inode table, on-disk
  directory entries, extent-based file mapping, journaling) — IS achievable
  and is what this plan delivers. That gives a real, well-coded, real-OS-grade
  filesystem without pretending to be a literal ext4.

## 1. Where YartFS v4 already stands (honest audit)

It is genuinely above the median hobby OS. It already has:

| Feature | Status |
|---|---|
| Superblock (magic, version, layout) | ✅ |
| Inode table (2048 inodes, 1 sector each) | ✅ |
| Inode + data bitmaps | ✅ |
| Triple-indirect block mapping (32 direct / 32 single / double / triple) | ✅ |
| Per-sector CRC (data integrity) | ✅ |
| Write-ahead journal (128 sectors) + per-record CRC + replay | ✅ |
| Contiguous-allocation "extent hint" | ✅ (hint only, not a real extent map) |
| Link-count field, symlink type flag | ✅ (fields exist; symlink not wired to VFS) |
| 32 MiB max file | ✅ |

### The ONE structural weakness that makes it "not a real filesystem yet"

**Inodes are keyed by a full 160-byte PATH STRING, not by number, and there
are no on-disk directory entries.** Consequences:

- `inode_find(path)` is a linear `strcmp` over all 2048 inodes — O(n) per
  lookup. A real FS resolves a path in O(depth × log(entries-per-dir)).
- Directories don't exist on disk as data; they're *reconstructed* at mount
  time by slicing path prefixes (`load_inode_into_tree`).
- This is why rename, directory iteration, and hard-link count are all
  awkward hacks rather than natural operations.

Everything else (journal, CRC, indirect blocks) is scaffolding around this
flaw. Fixing it is the whole migration.

## 2. Target: YartFS v5 (ext4-architected) on-disk format

### 2.1 Superblock (sector 0)
```
u64 magic          "YRTFS50"
u32 version         5
u32 block_size      4096            (up from 512-byte sectors)
u32 blocks_per_group 8192           (ext4 default-ish)
u32 inodes_per_group 2048
u32 total_blocks
u32 total_inodes
u32 first_data_block
u32 journal_blocks
u32 mtime, wtime, last_check
u8  uuid[16]
```

### 2.2 Block groups (the ext4 signature)
The volume is divided into block groups. Each group has, in order:
1. **Block bitmap** (1 block)
2. **Inode bitmap** (1 block)
3. **Inode table** (inodes_per_group × inode_size / block_size blocks)
4. **Data blocks**

This gives O(1)-ish local allocation, locality (files' blocks land near their
inode), and a fsck-able structure. It replaces the single global bitmaps.

### 2.3 Inodes (256 bytes, ext4-sized, inode-number keyed)
```
u16 mode            (type + perms)
u16 uid, gid
u32 size
u32 atime, ctime, mtime, dtime
u16 links_count
u32 flags
u32 blocks_count
u32 extent[15]      (see 2.4)
u32 generation
```
**No path string.** Inodes are found by number; directories map names → inode
numbers (2.5). Hard links fall out for free (`links_count` + sharing the same
inode number from two directory entries).

### 2.4 Extents (replaces direct/indirect/triple-indirect)
Each file's data is a list of `(logical_block, length, physical_block)` runs,
packed into the inode's 60-byte `extent[]` area (ext4's `i_data`):
```
struct extent { u32 ee_block; u16 ee_len; u16 ee_start_hi; u32 ee_start_lo; }
```
Up to 4 extents inline; beyond that, an extent tree indexed by an extent
header block. This is strictly better than the current 32/4096/16384/2M
indirect pyramid: one contiguous write = one extent entry, not thousands of
block pointers.

### 2.5 Directory entries (the fix for the #1 flaw)
A directory is a file whose data is a packed array of:
```
struct dirent { u32 inode; u16 rec_len; u8 name_len; u8 type; char name[]; }
```
`rec_len` lets entries span the remainder of a block (ext4 does exactly this)
so a delete is a cheap "shrink the previous entry" rather than a memmove.
Lookup walks dirents; `ls` reads them directly — no path-string hacks, no
O(2048) strcmp, no mount-time reconstruction.

### 2.6 Journal (kept, cleaned up)
Keep the write-ahead journal but make it a proper two-phase commit:
- `commit` record with a block list (metadata blocks being written)
- write data blocks, then metadata blocks, then a commit block, then a barrier
- replay = redo committed-but-not-checkpointed transactions
(Current journal writes whole file contents inline and replays into RAM
vnodes — simpler but not a real redo log.)

## 3. Staged migration (each stage boots + passes selftest before the next)

- **Stage 1 — block_size 4096 + block groups + inode-number keying.** ✅ DONE
- **Stage 2 — on-disk directory entries.** ✅ DONE (dirents, walk-from-root mount)
- **Stage 3 — extents.** ⬜ not yet (indirect blocks shipped; ext2/ext3 also use them)
- **Stage 4 — journal as a real redo log** (commit records, block lists). ⬜ next
- **Stage 5 — hard links + symlinks wired end-to-end** (`links_count` field in
  place; `SYS_SYMLINK`/`SYS_READLINK`/`VN_SYMLINK` to wire).
  ✅ symlinks done (turn #11, boot-verified). ⬜ hard links need a RAM-cache
  redesign (a vnode visible under multiple parents).
- **Stage 6 — fsck** (walk block groups, cross-check bitmaps vs extents vs
  dirents vs `links_count`). ⬜

Stage 1 + 2 shipped and are boot-verified (see turn #10 in BRUTAL_AUDIT.md:
boot counter persists 1→2→3 across reboots).

Each stage is independently boot-testable and reverts cleanly (the format is
version-gated: `version != 5` → reformat, same as today).

## 4. What I will NOT do (honesty)

- I will not claim "ext4" — YartFS v5 will be ext4-*styled*, not an ext4
  driver. You will not be able to mount a YartOS disk in Linux as ext4.
- I will not implement htree (hashed b-tree directory indexing) initially —
  plain dirent arrays first, htree only if directory lookup actually becomes a
  measured bottleneck.

## 5. First concrete step (queued)

Stage 1 + 2 are the substance. I'll start with Stage 1 (block groups + inode
numbers) because it is self-contained and verifiable, then land Stage 2
(dirents) on top. The VFS layer (`vfs.c`) stays API-compatible the whole way:
only `blkfs.c` changes, so `open/read/write/mkdir/unlink/rename` at the
syscall boundary never move.
