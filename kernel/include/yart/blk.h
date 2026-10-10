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
/* Flush the device's write cache to stable storage (VIRTIO_BLK_T_FLUSH).
 * fsync() durability REQUIRES this: QEMU's writeback cache only reaches the
 * backing file on a flush (or a graceful exit). */
int  blk_flush(void);

/* ---------- YartFS v5: ext-architected filesystem (see kernel/fs/blkfs.c) ----
 * 4 KiB blocks, block groups (bitmaps + inode tables), inode-number keying,
 * on-disk directory entries, indirect-block file mapping, per-inode + per-block
 * CRC.  The on-disk structures are private to blkfs.c. */
int  blkfs_init(void);
int  blkfs_sync(void);
void blkfs_note_delete(u32 ino);   /* queue an inode for deletion at sync */
bool blkfs_active(void);
u64  blkfs_synced_files(void);
void blkfs_selftest(void);
