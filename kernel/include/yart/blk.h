#pragma once
#include <yart/types.h>
#include <yart/hal.h>   /* cpu_regs_t */

#define BLK_SECTOR_SIZE 512
#define BLK_MAX_SECTORS_PER_IO 8   /* 8 * 512 = 4 KiB, fits one bounce page */

/* ---------- virtio-blk block device (legacy PCI interface) ---------- */
void blk_init(void);                    /* probe PCI + bring the device up  */
bool blk_disk_present(void);            /* a virtio-blk disk was found       */
u8   blk_irq_line(void);                /* PCI interrupt line (0 = none)     */
void blk_irq_handler(cpu_regs_t *r);    /* ISR: sets the done flag           */
u32  blk_irq_count(void);               /* how many IRQs have fired          */
bool blk_uses_msix(void);               /* device uses MSI-X (skip INTx route)   */
u64  blk_disk_sectors(void);            /* total capacity in 512B sectors    */
int  blk_read_sectors(u64 sector, u32 count, void *dst);   /* 0 = OK        */
int  blk_write_sectors(u64 sector, u32 count, const void *src);

/* ---------- YartFS: a minimal persistent filesystem on the disk ---------- */
/* Design (honest & simple): the RAM vnode tree is the working copy; dirty
 * nodes are written through to the disk by blkfs_sync().  The on-disk
 * format stores one inode per file/directory whose "name" is the full path
 * (directories are implicit, so we never manage directory blocks).  Files
 * use 32 direct data blocks plus up to 32 indirect tables of 128 entries
 * each (4128 blocks ~= 2 MiB max) - no more silent truncation at 16 KiB.
 *
 * On-disk layout (512B sectors), all regions NON-OVERLAPPING:
 *    0                     superblock (magic + geometry + version)
 *    1                     inode bitmap (512 bits)
 *    2 .. 2+511            inode table (1 inode / sector, 512 inodes)
 *    ...                   data bitmap
 *    ...                   data area (files + indirect tables)
 *    ...                   per-data-sector CRC32 table (4 B per sector)
 *    [total-128, total)    write-ahead journal (30 records + data)
 * The CRC table is laid out AFTER the data area (never over the journal,
 * never over the swap region at the end of the disk). */
#define BLKFS_MAGIC  0x59525446533231ULL   /* "YRTFS21"                      */
#define BLKFS_VERSION 2                    /* v2: indirect blocks + fixed
                                              CRC/journal layout (v1 had a
                                              layout bug: the CRC table
                                              overlapped the journal and the
                                              swap area - old disks are
                                              reformatted on mount)         */
#define BLKFS_MAX_INODES 512
#define BLKFS_MAX_DIRECT 32
#define BLKFS_INDIRECT_PER 128             /* entries per indirect table    */
#define BLKFS_MAX_INDIRECT 32              /* indirect tables per file      */
#define BLKFS_MAX_BLOCKS (BLKFS_MAX_DIRECT + BLKFS_MAX_INDIRECT * BLKFS_INDIRECT_PER)
#define BLKFS_MAX_FILE   (BLKFS_MAX_BLOCKS * BLK_SECTOR_SIZE)  /* ~2 MiB    */

#define BLKFS_TYPE_FILE 1
#define BLKFS_TYPE_DIR  2

typedef struct PACKED {
    u64 magic;
    u32 version;
    u32 inode_count;            /* BLKFS_MAX_INODES                        */
    u32 inode_start_sector;     /* where the inode table begins            */
    u32 data_start_sector;      /* where file data begins                  */
    u32 data_sectors;           /* size of the data area                   */
    u32 crc_start_sector;       /* checksum table (1 u32 per data sector)  */
    u32 crc_sectors;            /* sectors used by the checksum table       */
    u32 journal_start_sector;   /* write-ahead redo log (at the disk end)  */
    u32 reserved[6];
} blkfs_super_t;                /* fits one 512B sector                    */

typedef struct PACKED {
    char path[160];
    u8   type;                  /* BLKFS_TYPE_*                            */
    u8   pad[3];
    u32  size;
    u32  mtime;
    u32  blocks;
    u32  direct[BLKFS_MAX_DIRECT];
    u32  indirect[BLKFS_MAX_INDIRECT];  /* sectors of indirect tables     */
    u32  uid;                   /* owner (persisted)                       */
    u32  mode;                  /* permission bits (persisted)             */
    u32  reserved[4];
} blkfs_inode_t;                /* 456 B - fits one 512B sector            */

/* lifecycle: called once after vfs_init(); formats the disk on first boot,
 * otherwise loads the on-disk tree into the RAM vfs.  Returns 0 if the
 * filesystem is active (disk-backed), -1 if there is no disk (RAM only). */
int  blkfs_init(void);
int  blkfs_sync(void);          /* persist dirty nodes; call periodically   */
void blkfs_note_delete(const char *path);  /* queue a delete for next sync  */
bool blkfs_active(void);
u64  blkfs_synced_files(void);
