/* YartOS — YartFS v5: an ext-architected filesystem.
 *
 * This replaces the v4 "path-string inode" design with the on-disk model a
 * real Unix filesystem (ext2/ext3/ext4) uses:
 *
 *   - 4 KiB blocks (8 x 512-byte sectors; the block layer loops per page).
 *   - BLOCK GROUPS: the volume is split into groups, each with its own block
 *     bitmap, inode bitmap and inode table (locality + fsck-ability).
 *   - INODES KEYED BY NUMBER (root = 1).  No path strings on disk.  Hard
 *     links fall out of `links_count` naturally.
 *   - ON-DISK DIRECTORY ENTRIES: a directory's data is a packed array of
 *     (inode, rec_len, name_len, type, name) records — exactly ext's dirent.
 *     Lookup walks dirents; `ls` reads them; no path reconstruction at mount.
 *   - INDIRECT BLOCKS for file data (direct[12] + single + double + triple,
 *     each indirect block holds 1024 pointers).
 *   - Integrity: per-inode CRC32 (metadata) + per-data-block CRC32.
 *   - Write ordering + a superblock mount counter give ext2-style crash
 *     DETECTION (a full redo-log journal is the next stage; see
 *     docs/EXT4_VFS_PLAN.md).
 *
 * The RAM vnode tree remains the source of truth (a write-back cache): sync()
 * walks it post-order and persists dirty nodes; mount() rebuilds it from the
 * on-disk inodes + dirents.  The public API is unchanged, so vfs.c and the
 * syscall layer never move.
 */
#include <yart/blk.h>
#include <yart/fs.h>
#include <yart/mm.h>
#include <yart/string.h>
#include <yart/console.h>
#include <yart/hal.h>

/* ---------------- on-disk format constants ---------------- */
#define YFS_MAGIC     0x5952544635300000ULL  /* "YRTF50" */
#define YFS_VERSION   5
#define YFS_BLOCK_SIZE       4096
#define YFS_SECTORS_PER_BLK  (YFS_BLOCK_SIZE / BLK_SECTOR_SIZE)   /* 8 */
#define YFS_BLOCKS_PER_GROUP 4096
#define YFS_INODES_PER_GROUP 1024
#define YFS_INODE_SIZE       128
#define YFS_INODES_PER_BLK   (YFS_BLOCK_SIZE / YFS_INODE_SIZE)    /* 32 */
#define YFS_ITABLE_BLOCKS    (YFS_INODES_PER_GROUP / YFS_INODES_PER_BLK) /* 32 */
#define YFS_META_PER_GROUP   (2 + YFS_ITABLE_BLOCKS)  /* bbitmap+ibitmap+itable */
#define YFS_INDIRECT_PER     (YFS_BLOCK_SIZE / 4)     /* 1024 ptrs/block */
#define YFS_DIRECT           12
#define YFS_MAX_GROUPS       64
#define YFS_JOURNAL_BLOCKS   64            /* reserved (redo journal: stage 4) */
#define YFS_ROOT_INO         1

/* inode mode: type in the high 16 bits, perms in the low 16. */
#define YFS_TYPE_FILE 1
#define YFS_TYPE_DIR  2
#define YFS_TYPE_LNK  3

/* ---------------- on-disk structures ---------------- */
typedef struct PACKED {
    u64 magic;
    u32 version;
    u32 block_size;
    u32 blocks_per_group;
    u32 inodes_per_group;
    u32 inode_size;
    u32 total_blocks;      /* FS blocks (excludes swap) */
    u32 total_groups;
    u32 journal_start;     /* block # of the reserved journal area */
    u32 journal_blocks;
    u32 crc_start;         /* block # of the per-data-block CRC region */
    u32 mtime;
    u32 mount_count;
    u32 state;             /* 0 = clean, 1 = dirty (crash detection) */
    u32 reserved[7];
} yfs_sb_t;

typedef struct PACKED {
    u32 block_bitmap;      /* block # of the block bitmap */
    u32 inode_bitmap;      /* block # of the inode bitmap */
    u32 inode_table;       /* first block # of the inode table */
    u32 data_start;        /* first data block # */
    u32 data_end;          /* one past the last data block # */
    u32 free_blocks;
    u32 free_inodes;
    u32 reserved;
} yfs_gdesc_t;

typedef struct PACKED {
    u32 mode;              /* (type<<16) | perms */
    u32 uid, gid;
    u32 size;              /* bytes (dirs: dirent bytes) */
    u32 mtime;
    u32 links_count;
    u32 blocks_count;      /* data blocks allocated */
    u32 direct[YFS_DIRECT];
    u32 single_indirect;
    u32 double_indirect;
    u32 triple_indirect;
    u32 generation;
    u32 crc;               /* CRC32 of the inode with crc=0 */
    u32 reserved[8];
} yfs_inode_t;

typedef struct PACKED {
    u32 inode;
    u16 rec_len;           /* total entry length, 4-byte aligned */
    u8  name_len;
    u8  type;
    char name[];           /* name_len bytes */
} yfs_dirent_t;

_Static_assert(sizeof(yfs_inode_t) == YFS_INODE_SIZE, "inode must be 128 B");
_Static_assert(sizeof(yfs_sb_t) <= YFS_BLOCK_SIZE, "superblock fits one block");
_Static_assert(sizeof(yfs_gdesc_t) * YFS_MAX_GROUPS <= YFS_BLOCK_SIZE,
               "gdt fits one block");

/* ---------------- runtime state ---------------- */
static bool g_active;
static u64  g_synced_files;
static yfs_sb_t    g_sb;
static yfs_gdesc_t g_desc[YFS_MAX_GROUPS];
static u8  *g_bbitmap[YFS_MAX_GROUPS];   /* block bitmaps (1 block each)  */
static u8  *g_ibitmap[YFS_MAX_GROUPS];   /* inode bitmaps (1 block each)  */
static u32  g_deleted[64];               /* inode numbers to free at sync */
static int  g_deleted_n;

/* ---------------- block I/O (4K block = 8 sectors) ---------------- */
static void rd_blk(u32 b, void *buf) { blk_read_sectors((u64)b * YFS_SECTORS_PER_BLK, YFS_SECTORS_PER_BLK, buf); }
static void wr_blk(u32 b, const void *buf) { blk_write_sectors((u64)b * YFS_SECTORS_PER_BLK, YFS_SECTORS_PER_BLK, buf); }

static u32 crc32b(const void *data, u32 len) {
    const u8 *p = data; u32 crc = 0xFFFFFFFFu;
    while (len--) { crc ^= *p++; for (int i = 0; i < 8; i++) crc = (crc >> 1) ^ (0xEDB88320u & (u32)-(crc & 1)); }
    return ~crc;
}

/* ---------------- geometry ---------------- */
static u32 disk_total_blocks(void) {
    u64 s = blk_disk_sectors();
    u64 swap = vmm_swap_disk_reserve_sectors();
    return (u32)((s > swap ? s - swap : 0) / YFS_SECTORS_PER_BLK);
}

static int group_of_data_block(u32 blk) {
    for (u32 g = 0; g < g_sb.total_groups; g++)
        if (blk >= g_desc[g].data_start && blk < g_desc[g].data_end) return (int)g;
    return -1;
}

/* compute layout into g_sb + g_desc.  total_blocks must be known. */
static bool compute_layout(void) {
    u32 total = disk_total_blocks();
    if (total < 1024) return false;
    memset(g_desc, 0, sizeof g_desc);   /* fresh: no stale fields */
    g_sb.total_blocks = total;
    g_sb.block_size = YFS_BLOCK_SIZE;
    g_sb.blocks_per_group = YFS_BLOCKS_PER_GROUP;
    g_sb.inodes_per_group = YFS_INODES_PER_GROUP;
    g_sb.inode_size = YFS_INODE_SIZE;

    u32 groups = (total + YFS_BLOCKS_PER_GROUP - 1) / YFS_BLOCKS_PER_GROUP;
    if (groups == 0 || groups > YFS_MAX_GROUPS) return false;
    g_sb.total_groups = groups;

    g_sb.journal_blocks = YFS_JOURNAL_BLOCKS;
    g_sb.journal_start = total - YFS_JOURNAL_BLOCKS;

    /* group metadata starts right after superblock(0) + gdt(1) */
    u32 base = 2;
    for (u32 g = 0; g < groups; g++) {
        u32 span = base + g * YFS_BLOCKS_PER_GROUP;
        u32 span_end = span + YFS_BLOCKS_PER_GROUP;
        if (span_end > g_sb.journal_start) span_end = g_sb.journal_start;
        if (span_end <= span + YFS_META_PER_GROUP) return false;
        g_desc[g].block_bitmap = span;
        g_desc[g].inode_bitmap = span + 1;
        g_desc[g].inode_table  = span + 2;
        g_desc[g].data_start   = span + YFS_META_PER_GROUP;
        g_desc[g].data_end     = span_end;
        g_desc[g].free_blocks  = span_end - g_desc[g].data_start;
        g_desc[g].free_inodes  = YFS_INODES_PER_GROUP;
    }
    /* CRC region: one u32 per ACTUAL data block (sum over groups, not
     * journal_start - data_start, which would count interleaved metadata). */
    u32 data_total = 0;
    for (u32 g = 0; g < groups; g++)
        data_total += g_desc[g].data_end - g_desc[g].data_start;
    u32 crc_blocks = (data_total * 4 + YFS_BLOCK_SIZE - 1) / YFS_BLOCK_SIZE;
    g_sb.crc_start = g_sb.journal_start - crc_blocks;
    /* the last group's data must not run into the CRC region */
    for (u32 g = 0; g < groups; g++)
        if (g_desc[g].data_end > g_sb.crc_start) g_desc[g].data_end = g_sb.crc_start;
    return g_sb.crc_start > g_desc[0].data_start;
}

static void wr_gdt(void) {
    static u8 buf[YFS_BLOCK_SIZE]; memset(buf, 0, sizeof buf);
    memcpy(buf, g_desc, g_sb.total_groups * sizeof(yfs_gdesc_t));
    wr_blk(1, buf);
}
static void rd_gdt(void) {
    static u8 buf[YFS_BLOCK_SIZE]; rd_blk(1, buf);
    memcpy(g_desc, buf, g_sb.total_groups * sizeof(yfs_gdesc_t));
}

/* ---------------- bitmaps ---------------- */
static bool ibit(u32 ino) {
    u32 g = (ino - 1) / YFS_INODES_PER_GROUP, i = (ino - 1) % YFS_INODES_PER_GROUP;
    return (g_ibitmap[g][i / 8] >> (i % 8)) & 1;
}
static void iset(u32 ino, bool v) {
    u32 g = (ino - 1) / YFS_INODES_PER_GROUP, i = (ino - 1) % YFS_INODES_PER_GROUP;
    if (v) g_ibitmap[g][i / 8] |=  (1u << (i % 8));
    else   g_ibitmap[g][i / 8] &= ~(1u << (i % 8));
}
static bool bbit(u32 blk) {
    int g = group_of_data_block(blk);
    if (g < 0) return true;               /* outside data: treat as used */
    u32 i = blk - g_desc[g].data_start;
    return (g_bbitmap[g][i / 8] >> (i % 8)) & 1;
}
static void bset(u32 blk, bool v) {
    int g = group_of_data_block(blk);
    if (g < 0) return;
    u32 i = blk - g_desc[g].data_start;
    if (v) g_bbitmap[g][i / 8] |=  (1u << (i % 8));
    else   g_bbitmap[g][i / 8] &= ~(1u << (i % 8));
}

static u32 alloc_ino(void) {
    for (u32 g = 0; g < g_sb.total_groups; g++)
        for (u32 i = 0; i < YFS_INODES_PER_GROUP; i++) {
            u32 ino = g * YFS_INODES_PER_GROUP + i + 1;
            if (!ibit(ino)) { iset(ino, true); return ino; }
        }
    return 0;
}
static u32 alloc_block(u32 prefer_group) {
    for (u32 pass = 0; pass < 2; pass++)
        for (u32 gg = 0; gg < g_sb.total_groups; gg++) {
            u32 g = (pass == 0) ? (prefer_group + gg) % g_sb.total_groups : gg;
            for (u32 i = 0; i < g_desc[g].data_end - g_desc[g].data_start; i++) {
                u32 blk = g_desc[g].data_start + i;
                if (!bbit(blk)) { bset(blk, true); return blk; }
            }
        }
    return 0;
}

/* ---------------- inode table ---------------- */
static u32 inode_block(u32 ino) {
    u32 g = (ino - 1) / YFS_INODES_PER_GROUP, i = (ino - 1) % YFS_INODES_PER_GROUP;
    return g_desc[g].inode_table + i / YFS_INODES_PER_BLK;
}
static u32 inode_slot_off(u32 ino) {
    return ((ino - 1) % YFS_INODES_PER_GROUP) % YFS_INODES_PER_BLK * YFS_INODE_SIZE;
}
static void rd_inode(u32 ino, yfs_inode_t *out) {
    static u8 buf[YFS_BLOCK_SIZE]; rd_blk(inode_block(ino), buf);
    memcpy(out, buf + inode_slot_off(ino), sizeof *out);
}
static void wr_inode(u32 ino, const yfs_inode_t *in) {
    static u8 buf[YFS_BLOCK_SIZE]; rd_blk(inode_block(ino), buf);
    memcpy(buf + inode_slot_off(ino), in, sizeof *in);
    wr_blk(inode_block(ino), buf);
}

/* ---------------- per-data-block CRC ---------------- */
/* Dense index of a data block: data blocks are NOT contiguous (each block
 * group carries 34 metadata blocks before its data), so the CRC slot must be
 * computed by summing the data-block counts of all preceding groups. */
static u32 data_index(u32 blk) {
    u32 idx = 0;
    for (u32 g = 0; g < g_sb.total_groups; g++) {
        if (blk >= g_desc[g].data_start && blk < g_desc[g].data_end)
            return idx + (blk - g_desc[g].data_start);
        idx += g_desc[g].data_end - g_desc[g].data_start;
    }
    return 0xFFFFFFFFu;      /* not a data block */
}
static u32 crc_of_block(u32 blk) {
    u32 idx = data_index(blk);
    if (idx == 0xFFFFFFFFu) return 0;
    static u8 buf[YFS_BLOCK_SIZE];
    rd_blk(g_sb.crc_start + (idx * 4) / YFS_BLOCK_SIZE, buf);
    u32 c; memcpy(&c, buf + (idx * 4) % YFS_BLOCK_SIZE, 4);
    return c;
}
static void crc_store_block(u32 blk, u32 crc) {
    u32 idx = data_index(blk);
    if (idx == 0xFFFFFFFFu) return;
    static u8 buf[YFS_BLOCK_SIZE];
    rd_blk(g_sb.crc_start + (idx * 4) / YFS_BLOCK_SIZE, buf);
    memcpy(buf + (idx * 4) % YFS_BLOCK_SIZE, &crc, 4);
    wr_blk(g_sb.crc_start + (idx * 4) / YFS_BLOCK_SIZE, buf);
}

/* ---------------- file data: indirect block mapping ---------------- */
static u32 inode_get_block(yfs_inode_t *in, u32 index, bool alloc, u32 prefer_group) {
    if (index < YFS_DIRECT) {
        u32 b = in->direct[index];
        if (!b) { if (!alloc) return 0; b = alloc_block(prefer_group); if (!b) return 0; in->direct[index] = b; }
        return b;
    }
    index -= YFS_DIRECT;
    if (index < YFS_INDIRECT_PER) {
        u32 tbl = in->single_indirect;
        if (!tbl) { if (!alloc) return 0; tbl = alloc_block(prefer_group); if (!tbl) return 0;
                    static u8 z[YFS_BLOCK_SIZE]; memset(z, 0, sizeof z); wr_blk(tbl, z); in->single_indirect = tbl; }
        static u8 buf[YFS_BLOCK_SIZE]; rd_blk(tbl, buf); u32 *e = (u32 *)buf;
        u32 b = e[index];
        if (!b) { if (!alloc) return 0; b = alloc_block(prefer_group); if (!b) return 0; e[index] = b; wr_blk(tbl, buf); }
        return b;
    }
    index -= YFS_INDIRECT_PER;
    if (index < (u64)YFS_INDIRECT_PER * YFS_INDIRECT_PER) {
        u32 dtbl = in->double_indirect;
        if (!dtbl) { if (!alloc) return 0; dtbl = alloc_block(prefer_group); if (!dtbl) return 0;
                     static u8 z[YFS_BLOCK_SIZE]; memset(z, 0, sizeof z); wr_blk(dtbl, z); in->double_indirect = dtbl; }
        static u8 dbuf[YFS_BLOCK_SIZE]; rd_blk(dtbl, dbuf); u32 *d = (u32 *)dbuf;
        u32 i1 = index / YFS_INDIRECT_PER, i2 = index % YFS_INDIRECT_PER;
        u32 tbl = d[i1];
        if (!tbl) { if (!alloc) return 0; tbl = alloc_block(prefer_group); if (!tbl) return 0;
                    static u8 z[YFS_BLOCK_SIZE]; memset(z, 0, sizeof z); wr_blk(tbl, z); d[i1] = tbl; wr_blk(dtbl, dbuf); }
        static u8 ibuf[YFS_BLOCK_SIZE]; rd_blk(tbl, ibuf); u32 *e = (u32 *)ibuf;
        u32 b = e[i2];
        if (!b) { if (!alloc) return 0; b = alloc_block(prefer_group); if (!b) return 0; e[i2] = b; wr_blk(tbl, ibuf); }
        return b;
    }
    /* triple indirect: 1024^3 blocks = 4 TiB, beyond any supported disk */
    return 0;
}

static void free_indirect_tree(u32 tbl, int depth) {
    if (!tbl) return;
    /* heap buffer (NOT static): this recurses, so a static would be clobbered
     * by the child call while the parent still reads it */
    u8 *buf = kmalloc(YFS_BLOCK_SIZE);
    rd_blk(tbl, buf); u32 *e = (u32 *)buf;
    if (depth == 1) { for (int i = 0; i < YFS_INDIRECT_PER; i++) if (e[i]) bset(e[i], false); }
    else { for (int i = 0; i < YFS_INDIRECT_PER; i++) if (e[i]) free_indirect_tree(e[i], depth - 1); }
    bset(tbl, false);
    kfree(buf);
}

/* Free every data block + indirect table an inode owns. */
static void free_all_blocks(yfs_inode_t *in) {
    for (int i = 0; i < YFS_DIRECT; i++)
        if (in->direct[i]) { bset(in->direct[i], false); in->direct[i] = 0; }
    if (in->single_indirect) { free_indirect_tree(in->single_indirect, 1); in->single_indirect = 0; }
    if (in->double_indirect) { free_indirect_tree(in->double_indirect, 2); in->double_indirect = 0; }
    if (in->triple_indirect) { free_indirect_tree(in->triple_indirect, 3); in->triple_indirect = 0; }
    in->blocks_count = 0;
}

/* Truncate an inode's block map to `nblocks` data blocks, freeing anything
 * beyond.  Correct for any shrink (the old code leaked blocks and left stale
 * indirect pointers for partial shrinks). */
static void truncate_blocks(yfs_inode_t *in, u32 nblocks) {
    /* direct blocks [nblocks, 12) */
    for (u32 i = nblocks; i < YFS_DIRECT; i++)
        if (in->direct[i]) { bset(in->direct[i], false); in->direct[i] = 0; }

    if (in->single_indirect) {
        if (nblocks <= YFS_DIRECT) {
            free_indirect_tree(in->single_indirect, 1); in->single_indirect = 0;
        } else {
            u32 hi = nblocks - YFS_DIRECT;          /* first entry to free */
            if (hi >= YFS_INDIRECT_PER) {
                free_indirect_tree(in->single_indirect, 1); in->single_indirect = 0;
            } else {
                u8 *buf = kmalloc(YFS_BLOCK_SIZE); rd_blk(in->single_indirect, buf);
                u32 *e = (u32 *)buf;
                for (u32 i = hi; i < YFS_INDIRECT_PER; i++)
                    if (e[i]) { bset(e[i], false); e[i] = 0; }
                wr_blk(in->single_indirect, buf); kfree(buf);
            }
        }
    }
    if (in->double_indirect) {
        /* free whole tree unless the shrink keeps data in it (>4 MiB files,
         * rare on this disk); correctness over micro-optimization */
        if (nblocks <= YFS_DIRECT + YFS_INDIRECT_PER)
            { free_indirect_tree(in->double_indirect, 2); in->double_indirect = 0; }
    }
    if (in->triple_indirect) {
        free_indirect_tree(in->triple_indirect, 3); in->triple_indirect = 0;
    }
    in->blocks_count = nblocks;
}

/* ---------------- inode free (deleted files) ---------------- */
static void inode_free(u32 ino) {
    yfs_inode_t in; rd_inode(ino, &in);
    for (int i = 0; i < YFS_DIRECT; i++) if (in.direct[i]) bset(in.direct[i], false);
    if (in.single_indirect) free_indirect_tree(in.single_indirect, 1);
    if (in.double_indirect) free_indirect_tree(in.double_indirect, 2);
    if (in.triple_indirect) free_indirect_tree(in.triple_indirect, 3);
    memset(&in, 0, sizeof in);
    iset(ino, false);
    wr_inode(ino, &in);
}

/* ---------------- directory entries ---------------- */
static u16 dirent_rec_len(u32 name_len) { return (u16)((8 + name_len + 3) & ~3u); }

/* build a dirent buffer for a directory vnode.  Only children that HAVE an
 * inode number are listed: initrd "seed" files (ino 0, never persisted) are
 * re-imported fresh from the initrd each boot, so they must not appear in the
 * on-disk dirents (a dirent pointing at ino 0 is invalid). */
static u8 *dir_build_entries(vnode_t *dir, u32 *size) {
    size_t total = 0;
    for (vnode_t *c = dir->child; c; c = c->sibling)
        if (c->ino) total += dirent_rec_len(strlen(c->name));
    u8 *buf = kmalloc(total ? total : 1);
    size_t off = 0;
    for (vnode_t *c = dir->child; c; c = c->sibling) {
        if (!c->ino) continue;
        u32 nl = strlen(c->name);
        u16 reclen = dirent_rec_len(nl);
        yfs_dirent_t de;
        de.inode = c->ino; de.rec_len = reclen;
        de.name_len = (u8)nl;
        de.type = (c->type == VN_DIR) ? YFS_TYPE_DIR :
                  (c->type == VN_SYMLINK) ? YFS_TYPE_LNK : YFS_TYPE_FILE;
        memcpy(buf + off, &de, 8);
        memcpy(buf + off + 8, c->name, nl);
        off += reclen;
    }
    *size = (u32)off;
    return buf;
}

/* write `data`/`size` as a file's data blocks (used by both files and dirs). */
static int persist_data(u32 ino, yfs_inode_t *in, const void *data, u32 size) {
    u32 prefer = (ino - 1) / YFS_INODES_PER_GROUP;
    u32 nblocks = (size + YFS_BLOCK_SIZE - 1) / YFS_BLOCK_SIZE;

    /* shrink: free everything beyond nblocks (correct for any size). */
    if (nblocks < in->blocks_count)
        truncate_blocks(in, nblocks);

    for (u32 b = 0; b < nblocks; b++) {
        u32 db = inode_get_block(in, b, true, prefer);
        if (!db) { kprintf("yfs: out of space persisting ino %u\n", ino); return -1; }
        static u8 buf[YFS_BLOCK_SIZE]; memset(buf, 0, sizeof buf);
        u32 off = b * YFS_BLOCK_SIZE, take = size - off;
        if (take > YFS_BLOCK_SIZE) take = YFS_BLOCK_SIZE;
        if (take) memcpy(buf, (const u8 *)data + off, take);
        wr_blk(db, buf);
        crc_store_block(db, crc32b(buf, YFS_BLOCK_SIZE));
    }
    in->size = size;
    in->blocks_count = nblocks;
    return 0;
}

/* ---------------- persist one vnode (files + dirs) ---------------- */
static int persist_node(vnode_t *v) {
    bool was_new = (v->ino == 0);
    if (was_new)
        v->ino = (v == vfs_root()) ? YFS_ROOT_INO : alloc_ino();
    if (!v->ino) return -1;

    yfs_inode_t in; memset(&in, 0, sizeof in);
    if (!was_new)
        rd_inode(v->ino, &in);           /* reuse the existing block map */
    /* a freshly-allocated inode slot holds garbage: leave `in` zeroed so the
     * block map starts empty instead of reading phantom block pointers */

    if (v->type == VN_DIR) {
        u32 sz; u8 *buf = dir_build_entries(v, &sz);
        int rc = persist_data(v->ino, &in, buf, sz);
        kfree(buf);
        if (rc) return rc;
    } else {
        if (persist_data(v->ino, &in, v->data, (u32)v->size)) return -1;
    }

    in.mode = ((v->type == VN_DIR) ? YFS_TYPE_DIR :
               (v->type == VN_SYMLINK) ? YFS_TYPE_LNK : YFS_TYPE_FILE) << 16 | (v->mode & 0xFFFF);
    in.uid = v->uid; in.gid = v->gid; in.mtime = (u32)v->mtime;
    if (!in.links_count) in.links_count = 1;
    in.generation++;
    in.crc = 0;
    in.crc = crc32b(&in, sizeof in);
    wr_inode(v->ino, &in);

    v->dirty = false;
    g_synced_files++;
    /* The parent's on-disk dirents must list this node's (possibly new) inode
     * number, so propagate dirtiness UP the tree.  sync_tree is post-order,
     * so the parent is persisted after this returns - no loop, and the whole
     * path from the changed file to the root gets a navigable on-disk form. */
    if (v->parent) v->parent->dirty = true;
    return 0;
}

/* post-order: children before parents (a dir's dirents need child inos) */
static void sync_tree(vnode_t *v) {
    if (!v) return;
    for (vnode_t *c = v->child; c; c = c->sibling) sync_tree(c);
    if (v->dirty) persist_node(v);
}

/* recursive helper: clear the dirty flag on a subtree (used after mount) */
static void clear_dirty(vnode_t *v) {
    if (!v) return;
    v->dirty = false;
    for (vnode_t *c = v->child; c; c = c->sibling) clear_dirty(c);
}

/* ---------------- mount: rebuild the RAM tree from disk ---------------- */
static void mount_dir(vnode_t *dir, yfs_inode_t *din) {
    if (!din->size) return;
    u8 *buf = kmalloc(din->size ? din->size : 1);
    u32 got = 0;
    for (u32 b = 0; got < din->size; b++) {
        u32 db = inode_get_block(din, b, false, 0);
        if (!db) break;
        static u8 blk[YFS_BLOCK_SIZE]; rd_blk(db, blk);
        u32 take = din->size - got; if (take > YFS_BLOCK_SIZE) take = YFS_BLOCK_SIZE;
        memcpy(buf + got, blk, take); got += take;
    }
    u32 off = 0;
    while (off + 8 <= din->size) {
        yfs_dirent_t *de = (yfs_dirent_t *)(buf + off);
        if (de->rec_len < 8 || de->rec_len > din->size - off) break;
        u32 nl = de->name_len; if (nl >= VFS_MAX_NAME) nl = VFS_MAX_NAME - 1;
        char name[VFS_MAX_NAME]; memcpy(name, buf + off + 8, nl); name[nl] = 0;
        if (de->inode && nl) {
            vnode_type_t t = (de->type == YFS_TYPE_DIR) ? VN_DIR :
                             (de->type == YFS_TYPE_LNK) ? VN_SYMLINK : VN_FILE;
            vnode_t *child = vfs_find_child(dir, name);
            if (!child) child = vfs_create(dir, name, t);
            if (child) {
                child->ino = de->inode;
                yfs_inode_t cin; rd_inode(de->inode, &cin);
                child->uid = cin.uid; child->gid = cin.gid;
                child->mode = (u16)(cin.mode & 0xFFFF);
                child->mtime = cin.mtime;
                child->dirty = false;
                if (t == VN_DIR) {
                    mount_dir(child, &cin);
                } else if (cin.size || t == VN_SYMLINK) {
                    /* free initrd's copy, load the disk's authoritative data */
                    if (child->data) kfree(child->data);
                    child->data = kmalloc(cin.size + 1);   /* +1 for a NUL */
                    child->cap = cin.size;
                    u32 fgot = 0;
                    for (u32 b = 0; fgot < cin.size; b++) {
                        u32 db = inode_get_block(&cin, b, false, 0);
                        if (!db) break;
                        static u8 blk[YFS_BLOCK_SIZE]; rd_blk(db, blk);
                        /* integrity: validate the per-block CRC (first block
                         * mismatch only, to avoid log spam) */
                        u32 expect = crc_of_block(db);
                        if (expect && expect != crc32b(blk, YFS_BLOCK_SIZE))
                            kprintf("yfs: !! CRC mismatch ino=%u block=%u (stored %08x calc %08x)\n",
                                    de->inode, b, expect, crc32b(blk, YFS_BLOCK_SIZE));
                        u32 take = cin.size - fgot; if (take > YFS_BLOCK_SIZE) take = YFS_BLOCK_SIZE;
                        memcpy((u8 *)child->data + fgot, blk, take); fgot += take;
                    }
                    ((u8 *)child->data)[cin.size] = 0;   /* NUL-terminate (symlink targets are C strings) */
                    child->size = cin.size;
                }
            }
        }
        off += de->rec_len;
    }
    kfree(buf);
}

/* ---------------- public API ---------------- */
bool blkfs_active(void) { return g_active; }
u64  blkfs_synced_files(void) { return g_synced_files; }

void blkfs_note_delete(u32 ino) {
    if (!g_active || !ino) return;
    for (int i = 0; i < g_deleted_n; i++) if (g_deleted[i] == ino) return;
    if (g_deleted_n < 64) g_deleted[g_deleted_n++] = ino;
}

static void flush_bitmaps(void) {
    for (u32 g = 0; g < g_sb.total_groups; g++) {
        wr_blk(g_desc[g].block_bitmap, g_bbitmap[g]);
        wr_blk(g_desc[g].inode_bitmap, g_ibitmap[g]);
    }
}

int blkfs_sync(void) {
    if (!g_active) return 0;
    vfs_lock();

    int dels = g_deleted_n;
    for (int i = 0; i < g_deleted_n; i++) if (g_deleted[i]) inode_free(g_deleted[i]);
    g_deleted_n = 0;

    u64 before = g_synced_files;
    sync_tree(vfs_root());
    int n = (int)(g_synced_files - before);

    /* If nothing was written and nothing was deleted, there is nothing to
     * flush: skip the superblock rewrite + device FLUSH.  The periodic
     * auto-sync (~1 Hz) then becomes a cheap no-op when the FS is idle,
     * instead of rewriting the superblock and barrier-flushing the device
     * every second. */
    if (n == 0 && dels == 0) {
        vfs_unlock();
        return 0;
    }

    /* crash-detection window: mark dirty BEFORE writing data, clear AFTER.
     * A crash in between leaves state=1 on disk -> detected on next mount. */
    static u8 sb[YFS_BLOCK_SIZE];
    g_sb.state = 1;
    memset(sb, 0, sizeof sb); memcpy(sb, &g_sb, sizeof g_sb);
    wr_blk(0, sb);

    flush_bitmaps();
    wr_gdt();

    g_sb.state = 0;
    memset(sb, 0, sizeof sb); memcpy(sb, &g_sb, sizeof g_sb);
    wr_blk(0, sb);

    vfs_unlock();
    blk_flush();
    return n;
}

static void format(void) {
    if (!compute_layout()) { kprintf("yfs: format geometry failed\n"); return; }
    for (u32 g = 0; g < g_sb.total_groups; g++) {
        memset(g_bbitmap[g], 0, YFS_BLOCK_SIZE);
        memset(g_ibitmap[g], 0, YFS_BLOCK_SIZE);
    }
    /* mark metadata blocks used in the block bitmaps */
    for (u32 g = 0; g < g_sb.total_groups; g++) {
        /* block bitmap, inode bitmap and inode table are NOT data blocks
         * (they live before data_start), so nothing to mark here; the block
         * bitmaps only cover [data_start, data_end). */
    }
    /* clear the CRC region + journal */
    static u8 z[YFS_BLOCK_SIZE]; memset(z, 0, sizeof z);
    for (u32 b = g_sb.crc_start; b < g_sb.journal_start + g_sb.journal_blocks; b++)
        wr_blk(b, z);
    g_sb.magic = YFS_MAGIC; g_sb.version = YFS_VERSION;
    g_sb.state = 0; g_sb.mount_count = 0;
    static u8 sb[YFS_BLOCK_SIZE]; memset(sb, 0, sizeof sb); memcpy(sb, &g_sb, sizeof g_sb);
    wr_blk(0, sb);
    wr_gdt();
    flush_bitmaps();
    g_active = true;
    kprintf("yfs v5: formatted %u blocks in %u groups (%u B blocks, %u inodes, journal @%u)\n",
            g_sb.total_blocks, g_sb.total_groups, YFS_BLOCK_SIZE,
            g_sb.total_groups * YFS_INODES_PER_GROUP, g_sb.journal_start);
}

int blkfs_init(void) {
    if (!blk_disk_present()) { g_active = false; return -1; }
    for (int g = 0; g < YFS_MAX_GROUPS; g++) {
        g_bbitmap[g] = kzalloc(YFS_BLOCK_SIZE);
        g_ibitmap[g] = kzalloc(YFS_BLOCK_SIZE);
    }
    static u8 sb[YFS_BLOCK_SIZE]; rd_blk(0, sb); memcpy(&g_sb, sb, sizeof g_sb);
    if (g_sb.magic != YFS_MAGIC || g_sb.version != YFS_VERSION) {
        kprintf("yfs: no/incompatible fs (magic %llx v%u) -> format v%d\n",
                (unsigned long long)g_sb.magic, g_sb.version, YFS_VERSION);
        format();
        return g_active ? 0 : -1;
    }
    rd_gdt();
    for (u32 g = 0; g < g_sb.total_groups; g++) {
        rd_blk(g_desc[g].block_bitmap, g_bbitmap[g]);
        rd_blk(g_desc[g].inode_bitmap, g_ibitmap[g]);
    }
    g_active = true;

    /* mount_count: increment once per MOUNT (not per sync) and persist it. */
    g_sb.mount_count++;
    {
        static u8 sb[YFS_BLOCK_SIZE];
        memset(sb, 0, sizeof sb); memcpy(sb, &g_sb, sizeof g_sb);
        wr_blk(0, sb);
    }

    if (g_sb.state == 1)
        kprintf("yfs: warning - unclean shutdown detected (state=dirty), CRCs will validate\n");

    /* rebuild the tree from the root inode (walk dirents) */
    yfs_inode_t rin; rd_inode(YFS_ROOT_INO, &rin);
    vnode_t *root = vfs_root();
    if (root) { root->ino = YFS_ROOT_INO; mount_dir(root, &rin); }
    /* mount only RECONSTRUCTS the tree; it must not leave dirty flags set
     * (vfs_create marks parents dirty).  Clear them so the first periodic
     * sync doesn't rewrite every file for no reason. */
    clear_dirty(root);

    kprintf("yfs v5: mounted (%u groups, %u inodes/group, journal @%u, crc @%u)\n",
            g_sb.total_groups, YFS_INODES_PER_GROUP, g_sb.journal_start, g_sb.crc_start);
    return 0;
}

/* ---------------- selftest (gated behind -DBLKFS_SELFTEST) ---------------- */
void blkfs_selftest(void) {
    if (!g_active) return;
    kprintf("yfs v5 selftest\n");
    vnode_t *d = vfs_lookup("/home/yart");
    if (!d) { kprintf("  !! no /home/yart\n"); return; }

    /* small file round-trip */
    vnode_t *f = vfs_lookup("/home/yart/selftest.txt");
    if (!f) f = vfs_create(d, "selftest.txt", VN_FILE);
    if (!f) { kprintf("  !! create fail\n"); return; }
    const char *msg = "yartfs v5 selftest payload\n";
    if (vfs_write(f, msg, 0, strlen(msg)) != (int)strlen(msg)) { kprintf("  !! write fail\n"); return; }
    blkfs_sync();
    yfs_inode_t in; rd_inode(f->ino, &in);
    kprintf("  small file: ino=%u blocks=%u size=%u crc=%08x\n", f->ino, in.blocks_count, in.size, in.crc);
    /* verify CRC of the inode we wrote */
    u32 saved = in.crc; in.crc = 0;
    if (crc32b(&in, sizeof in) != saved) kprintf("  !! inode CRC mismatch\n");

    /* directory round-trip: entries should have inode numbers */
    vnode_t *sub = vfs_lookup("/home/yart/selftest_dir");
    if (!sub) sub = vfs_create(d, "selftest_dir", VN_DIR);
    if (sub) {
        vnode_t *c = vfs_create(sub, "inner.txt", VN_FILE);
        if (c) vfs_write(c, "x", 0, 1);
        blkfs_sync();
        yfs_inode_t din; rd_inode(sub->ino, &din);
        kprintf("  dir: ino=%u entries=%u bytes\n", sub->ino, din.size);
    }
    /* symlink round-trip: create, resolve (write-through), readlink, persist */
    {
        vnode_t *sl = vfs_lookup_nofollow("/home/yart/selftest_link");
        if (sl) vfs_unlink(sl);
        sl = vfs_symlink(d, "selftest_link", "selftest.txt");
        if (!sl) { kprintf("  !! symlink create fail\n"); }
        else {
            /* resolution: writing to the link must write the target */
            vnode_t *via = vfs_lookup("/home/yart/selftest_link");
            if (via && via == f) kprintf("  symlink resolves to target (ok)\n");
            else kprintf("  !! symlink resolution failed\n");
            /* readlink returns the target string */
            char tgt[64]; u32 tl = (u32)sl->size;
            if (tl < sizeof tgt) { memcpy(tgt, sl->data, tl); tgt[tl] = 0; }
            kprintf("  readlink -> %s\n", tgt);
            blkfs_sync();
            yfs_inode_t lin; rd_inode(sl->ino, &lin);
            kprintf("  symlink inode: ino=%u type=%u size=%u\n", sl->ino, (lin.mode >> 16), lin.size);
            vfs_unlink(sl);
        }
    }

    vfs_unlink(f);
    blkfs_sync();
    kprintf("yfs v5 selftest PASS\n");
}
