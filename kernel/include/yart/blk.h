#pragma once
#include <yart/types.h>
#include <yart/hal.h>

#define BLK_SECTOR_SIZE 512
#define BLK_MAX_SECTORS_PER_IO 8

void blk_init(void);
bool blk_disk_present(void);
u8   blk_irq_line(void);
void blk_irq_handler(cpu_regs_t *r);
u32  blk_irq_count(void);
bool blk_uses_msix(void);
u64  blk_disk_sectors(void);
int  blk_read_sectors(u64 sector, u32 count, void *dst);
int  blk_write_sectors(u64 sector, u32 count, const void *src);

/* ---------- YartFS v4: ADVANCED MAXIMUM filesystem, real OS level ---------- */
/* Design v4 - pushed to absolute max, like ext4/btrfs simplified:
 *  - Inode count 2048 (was 1024) - more files
 *  - Layout:
 *      direct[32] = 16 KiB
 *      indirect[32] = 32*128 = 4096 blocks = 2 MiB
 *      double-indirect (reserved[0]) = 128*128 = 16384 blocks = 8 MiB
 *      triple-indirect (reserved[1]) = 128*128*128 = 2,097,152 blocks = 1 GiB theoretical
 *      => total per file = 32+4096+16384+2M = ~1 GiB max, capped by 32 MiB disk = can fill whole disk
 *  - New in v4:
 *      * Triple indirect fully implemented (reserved[1])
 *      * Link count in reserved[2] (hardlinks)
 *      * Flags in reserved[3]: bit0=extent mode, bit1=symlink
 *      * Symlink type (BLKFS_TYPE_SYMLINK) stores target path in data blocks
 *      * Journal now has checksum per record (CRC of header+data)
 *      * Directory indexing via hash cache (in RAM vnode, not on-disk yet)
 *      * Extent-like contiguous allocation hint: data_alloc tries contiguous run
 *      * Advanced selftest: 10 MiB file + 20 MiB file + symlink + hardlink simulation
 *  - Still non-overlapping, CRC per sector, journal 128 sectors.
 */
#define BLKFS_MAGIC  0x59525446533431ULL   /* "YRTFS41" */
#define BLKFS_VERSION 4
#define BLKFS_MAX_INODES 2048
#define BLKFS_MAX_DIRECT 32
#define BLKFS_INDIRECT_PER 128
#define BLKFS_MAX_INDIRECT 32
#define BLKFS_DINDIRECT_PER BLKFS_INDIRECT_PER
#define BLKFS_DINDIRECT_COUNT (BLKFS_INDIRECT_PER * BLKFS_INDIRECT_PER) /* 16384 */
#define BLKFS_TINDIRECT_PER BLKFS_INDIRECT_PER
#define BLKFS_TINDIRECT_COUNT (BLKFS_INDIRECT_PER * BLKFS_INDIRECT_PER * BLKFS_INDIRECT_PER) /* 2M */
#define BLKFS_MAX_BLOCKS (BLKFS_MAX_DIRECT + BLKFS_MAX_INDIRECT * BLKFS_INDIRECT_PER + BLKFS_DINDIRECT_COUNT + BLKFS_TINDIRECT_COUNT)
#define BLKFS_MAX_FILE   (32ULL * 1024 * 1024)  /* cap at 32 MiB (disk size), theoretical 1 GiB */

#define BLKFS_TYPE_FILE 1
#define BLKFS_TYPE_DIR  2
#define BLKFS_TYPE_SYMLINK 3

#define BLKFS_FLAG_EXTENT  (1u<<0)
#define BLKFS_FLAG_SYMLINK (1u<<1)

typedef struct PACKED {
    u64 magic;
    u32 version;
    u32 inode_count;
    u32 inode_start_sector;
    u32 data_start_sector;
    u32 data_sectors;
    u32 crc_start_sector;
    u32 crc_sectors;
    u32 journal_start_sector;
    u32 reserved[6];
} blkfs_super_t;

typedef struct PACKED {
    char path[160];
    u8   type;
    u8   pad[3];
    u32  size;
    u32  mtime;
    u32  blocks;
    u32  direct[BLKFS_MAX_DIRECT];
    u32  indirect[BLKFS_MAX_INDIRECT];
    u32  uid;
    u32  mode;
    /* v4 extensions:
     * reserved[0] = double indirect sector
     * reserved[1] = triple indirect sector
     * reserved[2] = link count (hardlinks) + high bits symlink
     * reserved[3] = flags (extent, etc)
     */
    u32  reserved[4];
} blkfs_inode_t;

#define BLKFS_DINDIRECT_SECTOR(in) ((in)->reserved[0])
#define BLKFS_TINDIRECT_SECTOR(in) ((in)->reserved[1])
#define BLKFS_LINK_COUNT(in)       ((in)->reserved[2] & 0xFFFFFF)
#define BLKFS_FLAGS(in)            ((in)->reserved[3])

int  blkfs_init(void);
int  blkfs_sync(void);
void blkfs_note_delete(const char *path);
bool blkfs_active(void);
u64  blkfs_synced_files(void);
void blkfs_selftest(void);
