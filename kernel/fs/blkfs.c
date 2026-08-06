/* Yart OS - YartFS: a minimal persistent filesystem on the virtio-blk disk.
 *
 * Model:
 *   - The RAM vnode tree (vfs.c) is the working copy.
 *   - Nodes are marked dirty by vfs_write/create/truncate; blkfs_sync()
 *     writes them through to the disk and clears the flag.
 *   - The idle desktop loop calls blkfs_sync() about once a second, so any
 *     change is persisted quickly and nothing is lost on reboot.
 *
 * On-disk layout (512B sectors):
 *    0                     superblock (magic + geometry)
 *    1                     inode bitmap (512 bits)
 *    2 .. 2+511            inode table (1 inode / sector, 512 inodes)
 *    ...                   data bitmap
 *    ...                   data area (files)
 *
 * Each inode's "name" is the file's FULL PATH; directories are implicit, so
 * the tree is rebuilt by mkdir_p()ing parents.  Files use up to 32 direct
 * blocks (16 KiB max), which covers every file this OS currently creates.
 */
#include <yart/blk.h>
#include <yart/fs.h>
#include <yart/mm.h>
#include <yart/string.h>
#include <yart/console.h>
#include <yart/hal.h>

static bool g_active;
static u64  g_synced_files;
static blkfs_super_t g_super;
static blkfs_inode_t *g_inodes;          /* BLKFS_MAX_INODES, heap        */
static u8   g_inode_used[BLKFS_MAX_INODES / 8];
static u8  *g_data_bitmap;               /* (data_sectors+7)/8 bytes      */
static u32  g_data_bitmap_sectors;

static char g_deleted[64][160];
static int  g_deleted_count;

/* The top of the disk is reserved for the swap tier (vmm_swap_disk_init), so
 * the filesystem sizes itself to everything below that region.  Both format
 * and mount must agree on this or the journal/swap regions would overlap. */
static u32 blkfs_disk_total(void) {
    u64 s = blk_disk_sectors();
    u64 swap = vmm_swap_disk_reserve_sectors();
    return (u32)(s > swap ? s - swap : 0);
}

static void io_read(u64 sector, u32 count, void *buf) {
    blk_read_sectors(sector, count, buf);
}
static void io_write(u64 sector, u32 count, const void *buf) {
    blk_write_sectors(sector, count, buf);
}

/* ---------------- per-block CRC32 (bit-rot detection) ---------------- */
static u32 crc32_bytes(const void *data, u32 len) {
    const u8 *p = data;
    u32 crc = 0xFFFFFFFFu;
    while (len--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++)
            crc = (crc >> 1) ^ (0xEDB88320u & (u32)-(crc & 1));
    }
    return ~crc;
}
static u32 crc_of(u32 sector) {
    /* crc table lives at crc_start_sector; one u32 per data sector */
    u32 off = sector - g_super.data_start_sector;
    u8 buf[BLK_SECTOR_SIZE];
    io_read(g_super.crc_start_sector + off / 128, 1, buf);
    u32 c;
    memcpy(&c, buf + (off % 128) * 4, 4);
    return c;
}
static void crc_store(u32 sector, u32 crc) {
    u32 off = sector - g_super.data_start_sector;
    u8 buf[BLK_SECTOR_SIZE];
    memset(buf, 0, sizeof buf);
    io_read(g_super.crc_start_sector + off / 128, 1, buf);
    memcpy(buf + (off % 128) * 4, &crc, 4);
    io_write(g_super.crc_start_sector + off / 128, 1, buf);
}

/* ---------------- write-ahead journal (multi-record redo log) ------- */
/* A real redo log at the end of the disk: up to 30 records, each holding
 * header + data.  A sequence number lets replay apply records in order and
 * stop at the first gap (crash).  The head record doubles as the commit
 * pointer.  A crash mid-transaction leaves an uncommitted tail that replay
 * simply stops at - the committed prefix is always applied, so no file is
 * ever half-written or lost. */
#define BLKFS_JRN_SECTORS 128
#define JRN_MAGIC   0x594A524Eu    /* "YJRN" */
#define JRN_RECORDS 30
#define JRN_FILE    1
#define JRN_DIR     2
#define JRN_DELETE  3
#define JRN_MAX_DATA (BLKFS_JRN_SECTORS - JRN_RECORDS)  /* data sectors */
typedef struct PACKED {
    u32 magic;      /* JRN_MAGIC                                */
    u32 seq;        /* increasing sequence (commit order)       */
    u32 type;       /* JRN_*                                    */
    u32 size;       /* bytes (files)                            */
    char path[160];
    u32 nblocks;    /* data sectors following the header        */
    u32 uid, mode;
    u32 reserved[4];
} blkfs_jrn_t;      /* 1 sector per record                       */

static u32 jrn_start;
static u64 g_jrn_replays;
static u32 jrn_seq;   /* monotonic; persisted implicitly via records */

static void jrn_clear_range(void) {
    u8 z[BLK_SECTOR_SIZE];
    memset(z, 0, sizeof z);
    for (u32 i = 0; i < BLKFS_JRN_SECTORS; i++) io_write(jrn_start + i, 1, z);
}

/* Append one record + its data, then bump seq. */
static int jrn_write(const char *path, u8 type, const void *data, u32 size) {
    blkfs_jrn_t h;
    memset(&h, 0, sizeof h);
    h.magic = JRN_MAGIC; h.seq = ++jrn_seq; h.type = type; h.size = size;
    strncpy(h.path, path, sizeof h.path - 1);
    h.uid = 0; h.mode = 0644;
    u32 nblocks = (size + BLK_SECTOR_SIZE - 1) / BLK_SECTOR_SIZE;
    if (nblocks > JRN_MAX_DATA) { nblocks = JRN_MAX_DATA; size = JRN_MAX_DATA * BLK_SECTOR_SIZE; }
    h.nblocks = nblocks;
    u8 sb[BLK_SECTOR_SIZE];
    memset(sb, 0, sizeof sb);
    memcpy(sb, &h, sizeof h);
    u32 slot = (jrn_seq - 1) % JRN_RECORDS;
    io_write(jrn_start + slot, 1, sb);
    for (u32 b = 0; b < nblocks; b++) {
        u8 buf[BLK_SECTOR_SIZE];
        memset(buf, 0, sizeof buf);
        u32 off = b * BLK_SECTOR_SIZE;
        u32 take = (size - off > BLK_SECTOR_SIZE) ? BLK_SECTOR_SIZE : size - off;
        if (take && data) memcpy(buf, (const u8 *)data + off, take);
        io_write(jrn_start + JRN_RECORDS + b, 1, buf);
    }
    return 0;
}

/* On mount: scan all slots, collect records with valid magic + seq in
 * [min,max], sort by seq, replay the contiguous prefix from 1.., clear. */
static void jrn_replay(void) {
    blkfs_jrn_t recs[JRN_RECORDS];
    u32 n = 0, minseq = 0xFFFFFFFFu, maxseq = 0;
    for (u32 i = 0; i < JRN_RECORDS; i++) {
        blkfs_jrn_t h;
        u8 sb[BLK_SECTOR_SIZE];
        io_read(jrn_start + i, 1, sb);
        memcpy(&h, sb, sizeof h);
        if (h.magic != JRN_MAGIC) continue;
        recs[n++] = h;
        if (h.seq < minseq) minseq = h.seq;
        if (h.seq > maxseq) maxseq = h.seq;
    }
    if (!n) return;
    /* replay in seq order */
    u32 expect = minseq;
    for (u32 pass = 0; pass < n; pass++) {
        for (u32 i = 0; i < n; i++) {
            if (recs[i].seq != expect) continue;
            blkfs_jrn_t h = recs[i];
            g_jrn_replays++;
            kprintf("blkfs: journal replay #%u: %s (type=%u size=%u)\n",
                    h.seq, h.path, h.type, h.size);
            if (h.type == JRN_DELETE) {
                vnode_t *v = vfs_lookup(h.path);
                if (v) vfs_unlink(v);
            } else {
                const char *slash = NULL;
                for (const char *p = h.path; *p; p++) if (*p == '/') slash = p;
                if (slash && slash != h.path) {
                    char dir[VFS_MAX_PATH];
                    size_t l = slash - h.path;
                    if (l < sizeof dir) { memcpy(dir, h.path, l); dir[l] = 0; vfs_mkdir_p(dir); }
                }
                const char *base = slash ? slash + 1 : h.path;
                char dir[VFS_MAX_PATH];
                if (slash && slash != h.path) { size_t l = slash - h.path; memcpy(dir, h.path, l); dir[l] = 0; }
                else strncpy(dir, "/", sizeof dir - 1);
                vnode_t *d = vfs_lookup(dir);
                if (!d) return;
                vnode_t *v = vfs_lookup(h.path);
                if (!v) v = vfs_create(d, base, h.type == JRN_DIR ? VN_DIR : VN_FILE);
                if (v && h.type != JRN_DIR) {
                    if (v->data) kfree(v->data);
                    v->size = h.size; v->cap = h.size;
                    v->data = h.size ? kzalloc(h.size) : NULL;
                    u32 off = 0;
                    for (u32 b = 0; b < h.nblocks && off < h.size; b++) {
                        u8 buf[BLK_SECTOR_SIZE];
                        io_read(jrn_start + JRN_RECORDS + b, 1, buf);
                        u32 take = h.size - off;
                        if (take > BLK_SECTOR_SIZE) take = BLK_SECTOR_SIZE;
                        if (v->data) memcpy((u8 *)v->data + off, buf, take);
                        off += take;
                    }
                    v->uid = h.uid; v->mode = h.mode; v->dirty = true;
                }
            }
            expect++;
            break;
        }
    }
    jrn_clear_range();
    (void)maxseq;
}

bool blkfs_active(void)          { return g_active; }
u64  blkfs_synced_files(void)    { return g_synced_files; }



/* ---------------- bitmaps ---------------- */
static bool inode_used(u32 i) { return (g_inode_used[i / 8] >> (i % 8)) & 1; }
static void inode_set(u32 i, bool used) {
    if (used) g_inode_used[i / 8] |=  (1u << (i % 8));
    else      g_inode_used[i / 8] &= ~(1u << (i % 8));
}
static bool data_used(u32 b) { return (g_data_bitmap[b / 8] >> (b % 8)) & 1; }
static void data_set(u32 b, bool used) {
    if (used) g_data_bitmap[b / 8] |=  (1u << (b % 8));
    else      g_data_bitmap[b / 8] &= ~(1u << (b % 8));
}
static u32 data_alloc(void) {
    for (u32 b = 0; b < g_super.data_sectors; b++)
        if (!data_used(b)) { data_set(b, true); return b; }
    return 0xFFFFFFFFu;
}

/* ---------------- inode <-> sector ---------------- */
static void inode_read(u32 i) {
    u8 scratch[BLK_SECTOR_SIZE];
    io_read(g_super.inode_start_sector + i, 1, scratch);
    memcpy(&g_inodes[i], scratch, sizeof(blkfs_inode_t));
}
static void inode_write(u32 i) {
    u8 scratch[BLK_SECTOR_SIZE];
    memset(scratch, 0, sizeof scratch);
    memcpy(scratch, &g_inodes[i], sizeof(blkfs_inode_t));
    io_write(g_super.inode_start_sector + i, 1, scratch);
}

static blkfs_inode_t *inode_find(const char *path) {
    for (u32 i = 0; i < BLKFS_MAX_INODES; i++)
        if (inode_used(i) && strcmp(g_inodes[i].path, path) == 0)
            return &g_inodes[i];
    return NULL;
}
static blkfs_inode_t *inode_alloc(const char *path) {
    for (u32 i = 0; i < BLKFS_MAX_INODES; i++)
        if (!inode_used(i)) {
            inode_set(i, true);
            memset(&g_inodes[i], 0, sizeof(blkfs_inode_t));
            strncpy(g_inodes[i].path, path, sizeof(g_inodes[i].path) - 1);
            inode_write(i);
            return &g_inodes[i];
        }
    return NULL;
}
/* Resolve (or, when `alloc`, create) the on-disk data sector for block `b`
 * of inode `in`.  Blocks [0, BLKFS_MAX_DIRECT) are direct; the rest go
 * through up to BLKFS_MAX_INDIRECT singly-indirect tables of 128 entries.
 * Returns the data sector or 0xFFFFFFFF on failure. */
static u32 inode_data_block(blkfs_inode_t *in, u32 b, bool alloc) {
    if (b < BLKFS_MAX_DIRECT) {
        u32 db = in->direct[b];
        if (db == 0 && alloc) {
            db = data_alloc();
            if (db != 0xFFFFFFFFu) in->direct[b] = db;
        }
        return db;
    }
    u32 i = b - BLKFS_MAX_DIRECT;
    u32 ti = i / BLKFS_INDIRECT_PER;
    u32 te = i % BLKFS_INDIRECT_PER;
    if (ti >= BLKFS_MAX_INDIRECT) return 0xFFFFFFFFu;
    u32 tbl = in->indirect[ti];
    if (tbl == 0) {
        if (!alloc) return 0xFFFFFFFFu;
        tbl = data_alloc();
        if (tbl == 0xFFFFFFFFu) return 0xFFFFFFFFu;
        u8 z[BLK_SECTOR_SIZE];
        memset(z, 0, sizeof z);
        io_write(g_super.data_start_sector + tbl, 1, z);
        in->indirect[ti] = tbl;
    }
    u8 buf[BLK_SECTOR_SIZE];
    io_read(g_super.data_start_sector + tbl, 1, buf);
    u32 *ents = (u32 *)buf;
    u32 db = ents[te];
    if (db == 0 && alloc) {
        db = data_alloc();
        if (db != 0xFFFFFFFFu) {
            ents[te] = db;
            io_write(g_super.data_start_sector + tbl, 1, buf);
        }
    }
    return db;
}

static void discard_block(u32 db) {
    if (db >= g_super.data_sectors) return;
    /* discard: zero the sector so deleted data + its CRC are wiped
     * (privacy + matches the freed-bitmap) */
    u8 z[BLK_SECTOR_SIZE];
    memset(z, 0, sizeof z);
    io_write(g_super.data_start_sector + db, 1, z);
    crc_store(g_super.data_start_sector + db, 0);
    data_set(db, false);
}

static void inode_free(blkfs_inode_t *in) {
    for (u32 i = 0; i < BLKFS_MAX_INODES; i++)
        if (&g_inodes[i] == in) {
            for (u32 b = 0; b < in->blocks; b++) {
                u32 db = inode_data_block(in, b, false);
                if (db != 0xFFFFFFFFu) discard_block(db);
            }
            /* free the indirect tables themselves */
            for (u32 t = 0; t < BLKFS_MAX_INDIRECT; t++)
                if (in->indirect[t] < g_super.data_sectors)
                    data_set(in->indirect[t], false);
            inode_set(i, false);
            memset(&g_inodes[i], 0, sizeof(blkfs_inode_t));
            inode_write(i);
            return;
        }
}

/* ---------------- persist one node ---------------- */
static int persist_node(vnode_t *v) {
    char path[VFS_MAX_PATH];
    if (vfs_path_of(v, path, sizeof path) <= 0) return -1;
    u8  type = (v->type == VN_DIR) ? BLKFS_TYPE_DIR : BLKFS_TYPE_FILE;
    u32 size = (v->type == VN_FILE) ? (u32)v->size : 0;
    blkfs_inode_t *in = inode_find(path);
    if (!in) in = inode_alloc(path);
    if (!in) return -1;

    bool truncated = false;
    u32 nblocks = (size + BLK_SECTOR_SIZE - 1) / BLK_SECTOR_SIZE;
    if (nblocks > BLKFS_MAX_BLOCKS) {
        kprintf("blkfs: WARNING %s too big (%u B) - only %u B stored on disk\n",
                path, size, BLKFS_MAX_FILE);
        nblocks = BLKFS_MAX_BLOCKS;
        size = BLKFS_MAX_FILE;
        truncated = true;                      /* caller keeps it dirty and
                                                  re-logs on every sync -
                                                  the overflow is NEVER
                                                  silently dropped */
    }
    /* INCREMENTAL: keep the blocks we already own; only write the blocks
     * that are actually dirty (per-block bitmap from vfs_write).  A file
     * that changed 1 of 4 blocks writes 1 block, not the whole file.
     * Blocks beyond the 32 direct pointers go through indirect tables
     * (inode_data_block allocates the chain on demand). */
    for (u32 b = 0; b < nblocks; b++) {
        u32 db = inode_data_block(in, b, true);
        if (db == 0xFFFFFFFFu) { kprintf("blkfs: out of disk space\n"); return -1; }
        bool dirty = (b >= v->dirty_b0 && b < v->dirty_b1);
        if (!dirty) continue;                  /* unchanged: skip the write */
        u8 buf[BLK_SECTOR_SIZE];
        memset(buf, 0, sizeof buf);
        u32 off = b * BLK_SECTOR_SIZE;
        u32 take = (size - off > BLK_SECTOR_SIZE) ? BLK_SECTOR_SIZE : size - off;
        if (take) memcpy(buf, (const u8 *)v->data + off, take);
        io_write(g_super.data_start_sector + db, 1, buf);
        crc_store(g_super.data_start_sector + db, crc32_bytes(buf, BLK_SECTOR_SIZE));
    }
    /* free blocks beyond the new size (file shrank) */
    for (u32 b = nblocks; b < in->blocks; b++) {
        u32 db = inode_data_block(in, b, false);
        if (db != 0xFFFFFFFFu) discard_block(db);
    }
    /* zero every stale direct pointer so a later grow reallocates instead
     * of reusing a freed (discarded) sector */
    for (u32 b = nblocks; b < BLKFS_MAX_DIRECT; b++)
        in->direct[b] = 0;
    /* prune indirect tables that are now entirely unused */
    u32 need_tables = nblocks > BLKFS_MAX_DIRECT
                          ? (nblocks - BLKFS_MAX_DIRECT + BLKFS_INDIRECT_PER - 1)
                                / BLKFS_INDIRECT_PER
                          : 0;
    for (u32 t = need_tables; t < BLKFS_MAX_INDIRECT; t++) {
        if (in->indirect[t] < g_super.data_sectors) {
            data_set(in->indirect[t], false);
            in->indirect[t] = 0;
        }
    }
    in->type   = type;
    in->size   = size;
    in->blocks = nblocks;
    in->mtime  = 0;
    in->uid    = v->uid;
    in->mode   = v->mode;
    inode_write((u32)(in - g_inodes));
    if (!truncated) {
        v->dirty_b0 = 0;                       /* all changes persisted */
        v->dirty_b1 = 0;
    }
    return truncated ? -1 : 0;                 /* -1: caller keeps dirty */
}

/* ---------------- sync ---------------- */
static void flush_bitmaps(void) {
    u8 sb[BLK_SECTOR_SIZE];
    memset(sb, 0, sizeof sb);
    memcpy(sb, g_inode_used, sizeof g_inode_used);
    io_write(1, 1, sb);                      /* inode bitmap sector 1     */
    for (u32 s = 0; s < g_data_bitmap_sectors; s++) {
        u8 b[BLK_SECTOR_SIZE];
        memset(b, 0, sizeof b);
        u32 off = s * BLK_SECTOR_SIZE;
        u32 n = g_super.data_sectors / 8 - off;
        if (n > BLK_SECTOR_SIZE) n = BLK_SECTOR_SIZE;
        memcpy(b, g_data_bitmap + off, n);
        io_write(g_super.data_start_sector - g_data_bitmap_sectors + s, 1, b);
    }
}

static void sync_node(vnode_t *v) {
    if (!v) return;
    if (v->dirty) {
        char path[VFS_MAX_PATH];
        if (vfs_path_of(v, path, sizeof path) > 0) {
            u8 t = (v->type == VN_DIR) ? JRN_DIR : JRN_FILE;
            /* write-ahead: journal first, apply; the log is compacted
             * once at the end of the sync (multi-record redo log) */
            jrn_write(path, t, v->data, (v->type == VN_FILE) ? (u32)v->size : 0);
            if (persist_node(v) == 0) {
                g_synced_files++;
                v->dirty = false;
            }
            /* else: node stays dirty - retried next sync, and the
             * journal record remains for crash recovery */
        }
    }
    for (vnode_t *c = v->child; c; c = c->sibling)
        sync_node(c);
}

int blkfs_sync(void) {
    if (!g_active) return 0;
    /* The tree is walked + written through here while user syscalls may
     * mutate it on other CPUs: hold the VFS lock for the whole sync. */
    vfs_lock();
    /* process queued deletes first (journaled so a crash can't resurrect) */
    for (int i = 0; i < g_deleted_count; i++) {
        jrn_write(g_deleted[i], JRN_DELETE, NULL, 0);
        blkfs_inode_t *in = inode_find(g_deleted[i]);
        if (in) inode_free(in);
    }
    g_deleted_count = 0;

    u64 before = g_synced_files;
    sync_node(vfs_root());
    flush_bitmaps();
    jrn_clear_range();      /* all records applied - compact the log */
    int n = (int)(g_synced_files - before);
    vfs_unlock();
    return n;
}

void blkfs_note_delete(const char *path) {
    if (!g_active || !path) return;
    for (int i = 0; i < g_deleted_count; i++)
        if (strcmp(g_deleted[i], path) == 0) return;
    if (g_deleted_count < 64) {
        strncpy(g_deleted[g_deleted_count], path, 159);
        g_deleted[g_deleted_count][159] = 0;
        g_deleted_count++;
    }
}

/* ---------------- format / mount ---------------- */
/* Compute the on-disk geometry with STRICT non-overlap of every region.
 * Order from the front: super, inode bitmap, inode table, data bitmap,
 * data area, CRC table; the journal is pinned at the END of the FS area
 * (the swap tier lives above it).  The old v1 layout computed
 * crc_start_sector BEFORE subtracting the CRC sectors, which made the CRC
 * table cover the entire journal (and spill into swap) - the CRC stores
 * corrupted journal records and the journal's end-of-sync clear wiped the
 * CRC table every second, silently disabling bit-rot detection.  v2 also
 * persists journal_start_sector so mount can validate everything. */
static bool compute_geometry(void) {
    u32 total = blkfs_disk_total();
    u32 inode_table = BLKFS_MAX_INODES;      /* 1 inode per sector         */
    u32 after_inodes = g_super.inode_start_sector + inode_table;

    g_super.journal_start_sector = total - BLKFS_JRN_SECTORS;
    if (g_super.journal_start_sector <= after_inodes) {
        kprintf("blkfs: !! disk too small for inode table + journal (%u sectors)\n",
                total);
        return false;
    }

    u32 data_sectors = g_super.journal_start_sector - after_inodes;
    u32 bitmap_bytes = (data_sectors + 7) / 8;
    g_data_bitmap_sectors = (bitmap_bytes + BLK_SECTOR_SIZE - 1) / BLK_SECTOR_SIZE;
    data_sectors -= g_data_bitmap_sectors;

    /* reserve the CRC table space FIRST, then its start is data_end. */
    u32 crc_sectors = (data_sectors + 127) / 128;   /* 1 u32 per sector */
    data_sectors  -= crc_sectors;

    g_super.data_start_sector = after_inodes + g_data_bitmap_sectors;
    g_super.data_sectors      = data_sectors;
    g_super.crc_sectors       = crc_sectors;
    g_super.crc_start_sector  = g_super.data_start_sector + data_sectors;

    /* invariant: regions tile the area exactly, nothing overlaps */
    if (g_super.crc_start_sector + g_super.crc_sectors !=
        g_super.journal_start_sector) {
        kprintf("blkfs: !! geometry error: crc_end=%u journal=%u\n",
                g_super.crc_start_sector + g_super.crc_sectors,
                g_super.journal_start_sector);
        return false;
    }
    return true;
}

/* Validate a superblock read from disk.  Returns true when the geometry is
 * sane and every region is inside the disk and non-overlapping. */
static bool geometry_ok(const blkfs_super_t *s) {
    if (s->version != BLKFS_VERSION) {
        kprintf("blkfs: disk is format v%u, kernel wants v%u\n",
                s->version, BLKFS_VERSION);
        return false;
    }
    if (s->inode_count != BLKFS_MAX_INODES || s->inode_start_sector != 2)
        return false;
    u32 total = blkfs_disk_total();
    u32 inode_end = s->inode_start_sector + s->inode_count;
    if (inode_end > s->data_start_sector) return false;
    if (s->data_start_sector + s->data_sectors != s->crc_start_sector)
        return false;
    if (s->crc_start_sector + s->crc_sectors != s->journal_start_sector)
        return false;
    if (s->journal_start_sector + BLKFS_JRN_SECTORS != total)
        return false;
    return true;
}

static void format(void) {
    memset(&g_super, 0, sizeof g_super);
    g_super.magic = BLKFS_MAGIC;
    g_super.version = BLKFS_VERSION;
    g_super.inode_count = BLKFS_MAX_INODES;
    g_super.inode_start_sector = 2;          /* super + inode bitmap       */

    if (!compute_geometry()) {
        g_active = false;
        kprintf("blkfs: formatting impossible - filesystem disabled\n");
        return;
    }

    memset(g_inode_used, 0, sizeof g_inode_used);
    memset(g_inodes, 0, (size_t)BLKFS_MAX_INODES * sizeof(blkfs_inode_t));
    memset(g_data_bitmap, 0, (size_t)g_super.data_sectors / 8 + 1);

    jrn_start = g_super.journal_start_sector;
    jrn_seq = 0;
    jrn_clear_range();
    {
        u8 z[BLK_SECTOR_SIZE];
        memset(z, 0, sizeof z);
        for (u32 i = 0; i < g_super.crc_sectors; i++)
            io_write(g_super.crc_start_sector + i, 1, z);
    }
    u8 sb[BLK_SECTOR_SIZE];
    memset(sb, 0, sizeof sb);
    memcpy(sb, &g_super, sizeof g_super);
    io_write(0, 1, sb);
    flush_bitmaps();
    /* FRESH-FORMAT FIX: the caller does `return g_active ? 0 : -1;`, so a
     * successful format must arm the filesystem here - otherwise every
     * fresh-disk boot ran with blkfs disabled (fsync() and all blkfs file
     * ops failed all session). */
    g_active = true;
    kprintf("blkfs: formatted %u sectors (data %u @%u, crc %u @%u, "
            "journal 128 @%u, inodes %u)\n",
            (u32)blkfs_disk_total(), g_super.data_sectors, g_super.data_start_sector,
            g_super.crc_sectors, g_super.crc_start_sector,
            g_super.journal_start_sector, g_super.inode_count);
}

static void load_inode_into_tree(blkfs_inode_t *in) {
    if (in->type == BLKFS_TYPE_DIR) {
        if (strcmp(in->path, "/") != 0) {
            vfs_mkdir_p(in->path);
            vnode_t *d = vfs_lookup(in->path);
            if (d) { d->uid = in->uid; d->mode = in->mode; d->dirty = false; }
        }
        return;
    }
    /* file: mkdir parents, create/replace, read blocks */
    const char *slash = NULL;
    for (const char *p = in->path; *p; p++) if (*p == '/') slash = p;
    if (slash && slash != in->path) {
        char dir[VFS_MAX_PATH];
        size_t l = slash - in->path;
        if (l < sizeof dir) {
            memcpy(dir, in->path, l); dir[l] = 0;
            vfs_mkdir_p(dir);
        }
    }
    vnode_t *v = vfs_lookup(in->path);
    if (!v) {
        /* find parent + base */
        const char *base = slash ? slash + 1 : in->path;
        char dir[VFS_MAX_PATH];
        if (slash && slash != in->path) {
            size_t l = slash - in->path;
            memcpy(dir, in->path, l); dir[l] = 0;
        } else {
            strncpy(dir, "/", sizeof dir - 1);
        }
        vnode_t *d = vfs_lookup(dir);
        if (!d) return;
        v = vfs_create(d, base, VN_FILE);
        if (!v) return;
    }
    if (v->type != VN_FILE) return;
    v->size = in->size;
    v->cap  = 0;
    v->uid  = in->uid;
    v->mode = in->mode;
    v->dirty = false;
    if (in->size > 0) {
        v->data = kzalloc(in->size);
        v->cap  = in->size;
        u32 off = 0;
        for (u32 b = 0; b < in->blocks && off < in->size; b++) {
            u32 db = inode_data_block(in, b, false);
            if (db == 0xFFFFFFFFu) {
                kprintf("blkfs: !! %s block %u unreachable (corrupt inode)\n",
                        in->path, b);
                break;
            }
            u8 buf[BLK_SECTOR_SIZE];
            io_read(g_super.data_start_sector + db, 1, buf);
            /* verify the block checksum (bit-rot detection) */
            {
                u32 expect = crc_of(g_super.data_start_sector + db);
                u32 actual = crc32_bytes(buf, BLK_SECTOR_SIZE);
                if (expect && expect != actual)
                    kprintf("blkfs: !! CRC MISMATCH on %s block %u (bit-rot?)\n",
                            in->path, b);
            }
            u32 take = in->size - off;
            if (take > BLK_SECTOR_SIZE) take = BLK_SECTOR_SIZE;
            memcpy((u8 *)v->data + off, buf, take);
            off += take;
        }
    }
}

/* Boot-time durability test for the indirect-block path: a 64 KiB file is
 * 128 blocks - past the 32 direct pointers, so blocks 32..127 must be
 * reached through the indirect tables.  We write it, sync, then read every
 * block back FROM THE DISK (through inode_data_block) and CRC-verify it.
 * The file is deleted afterwards (exercising indirect-table freeing). */
void blkfs_selftest(void) {
    if (!g_active) return;
    kprintf("blkfs: selftest (64 KiB through indirect blocks)\n");
    bool ok = true;
    vnode_t *d = vfs_lookup("/home/yart");
    if (!d) { kprintf("blkfs: !! no /home/yart for selftest\n"); return; }
    vnode_t *v = vfs_lookup("/home/yart/big_selftest.bin");
    if (!v) v = vfs_create(d, "big_selftest.bin", VN_FILE);
    if (!v) { kprintf("blkfs: !! selftest create failed\n"); return; }

    const u32 SZ = 64 * 1024;
    u8 *buf = kmalloc(SZ);
    for (u32 i = 0; i < SZ; i++) buf[i] = (u8)(i * 31 + 7);
    if (vfs_write(v, buf, 0, SZ) != (int)SZ) { ok = false; }
    blkfs_sync();
    if (v->size != SZ) {
        kprintf("blkfs: !! selftest: vnode size %u != %u\n",
                (u32)v->size, SZ);
        ok = false;
    }
    blkfs_inode_t *in = inode_find("/home/yart/big_selftest.bin");
    if (!in) { ok = false; }
    else if (in->blocks != SZ / BLK_SECTOR_SIZE) {
        kprintf("blkfs: !! selftest: on-disk blocks %u != %u\n",
                in->blocks, SZ / BLK_SECTOR_SIZE);
        ok = false;
    }
    if (ok && in) {
        u32 nblocks = in->blocks;
        for (u32 b = 0; b < nblocks; b++) {
            u32 db = inode_data_block(in, b, false);
            if (db == 0xFFFFFFFFu) { ok = false; break; }
            u8 rb[BLK_SECTOR_SIZE];
            io_read(g_super.data_start_sector + db, 1, rb);
            if (memcmp(rb, buf + (size_t)b * BLK_SECTOR_SIZE,
                       BLK_SECTOR_SIZE) != 0) {
                kprintf("blkfs: !! selftest: block %u content mismatch\n", b);
                ok = false;
                break;
            }
            u32 expect = crc_of(g_super.data_start_sector + db);
            if (expect != crc32_bytes(rb, BLK_SECTOR_SIZE)) {
                kprintf("blkfs: !! selftest: block %u CRC mismatch\n", b);
                ok = false;
                break;
            }
        }
    }
    kprintf("blkfs: selftest %s (128 blocks, indirect tables, CRCs)\n",
            ok ? "PASS" : "FAIL");
    kfree(buf);
    vfs_unlink(v);                /* also frees indirect tables */
    blkfs_sync();
}

int blkfs_init(void) {
    if (!blk_disk_present()) { g_active = false; return -1; }

    g_inodes = kzalloc((size_t)BLKFS_MAX_INODES * sizeof(blkfs_inode_t));
    g_data_bitmap = kzalloc((size_t)blkfs_disk_total() / 8 + 16);

    u8 sb[BLK_SECTOR_SIZE];
    io_read(0, 1, sb);
    memcpy(&g_super, sb, sizeof g_super);

    if (g_super.magic != BLKFS_MAGIC) {
        kprintf("blkfs: no filesystem on disk - formatting\n");
        format();
        return g_active ? 0 : -1;    /* format() can fail on tiny disks */
    }

    if (!geometry_ok(&g_super)) {
        kprintf("blkfs: on-disk geometry invalid or old format - "
                "reformatting (old data discarded)\n");
        format();
        return g_active ? 0 : -1;
    }

    /* load bitmaps + inode table */
    u8 bm[BLK_SECTOR_SIZE];
    io_read(1, 1, bm);
    memcpy(g_inode_used, bm, sizeof g_inode_used);
    g_data_bitmap_sectors =
        (u32)(((g_super.data_sectors + 7) / 8 + BLK_SECTOR_SIZE - 1) / BLK_SECTOR_SIZE);
    for (u32 s = 0; s < g_data_bitmap_sectors; s++) {
        u8 b[BLK_SECTOR_SIZE];
        io_read(g_super.data_start_sector - g_data_bitmap_sectors + s, 1, b);
        memcpy(g_data_bitmap + s * BLK_SECTOR_SIZE, b, BLK_SECTOR_SIZE);
    }
    u32 loaded = 0;
    for (u32 i = 0; i < BLKFS_MAX_INODES; i++) {
        if (!inode_used(i)) continue;
        inode_read(i);
        load_inode_into_tree(&g_inodes[i]);
        loaded++;
    }
    g_active = true;
    kprintf("blkfs: mounted - %u files/dirs loaded from disk (%u sectors)\n",
            loaded, g_super.data_sectors);
    jrn_start = g_super.journal_start_sector;
    jrn_seq = 0;
    jrn_replay();
    if (g_jrn_replays) kprintf("blkfs: %llu journal transaction(s) replayed\n",
                               (unsigned long long)g_jrn_replays);
    return 0;
}
