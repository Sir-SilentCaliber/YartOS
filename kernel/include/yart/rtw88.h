#pragma once
#include <yart/types.h>

/* YartOS rtw88 port — driver-core structures for the RTL8822CE (10ec:c822).
 *
 * Register access is abstracted behind an ops table so the firmware-download
 * state machine can be proven in a selftest against a fake chip register
 * file, while the real PCI transport supplies MMIO-backed accessors on real
 * hardware.  This mirrors Linux rtw88's hci_ops layering. */

typedef struct rtw_dev rtw_dev_t;

typedef u8    (*rtw_read8_fn) (rtw_dev_t *d, u32 addr);
typedef u16   (*rtw_read16_fn)(rtw_dev_t *d, u32 addr);
typedef u32   (*rtw_read32_fn)(rtw_dev_t *d, u32 addr);
typedef void  (*rtw_write8_fn) (rtw_dev_t *d, u32 addr, u8 v);
typedef void  (*rtw_write16_fn)(rtw_dev_t *d, u32 addr, u16 v);
typedef void  (*rtw_write32_fn)(rtw_dev_t *d, u32 addr, u32 v);
typedef void  (*rtw_sleep_fn)  (rtw_dev_t *d, u32 ms);   /* download retry loops */

typedef struct {
    rtw_read8_fn  read8;
    rtw_read16_fn read16;
    rtw_read32_fn read32;
    rtw_write8_fn  write8;
    rtw_write16_fn write16;
    rtw_write32_fn write32;
    rtw_sleep_fn   sleep_ms;
} rtw_hci_ops_t;

struct rtw_dev {
    const rtw_hci_ops_t *ops;
    u16 vendor, device;      /* 0x10ec / 0xc822 */
    u8  bus, dev, fn;
    void *mmio;              /* BAR2 mapping (real hw) or fake regs (test) */
    u32  mmio_len;
    bool fw_loaded;
    bool fw_running;
    u16  fw_version;         /* from the firmware header, for bring-up logs */
    u8   mac[6];
    void *priv;              /* transport-private data */
};

/* ---- register accessors ---- */
static inline u32 rtw_read32(rtw_dev_t *d, u32 a) { return d->ops->read32(d, a); }
static inline u16 rtw_read16(rtw_dev_t *d, u32 a) { return d->ops->read16(d, a); }
static inline u8  rtw_read8 (rtw_dev_t *d, u32 a) { return d->ops->read8 (d, a); }
static inline void rtw_write32(rtw_dev_t *d, u32 a, u32 v) { d->ops->write32(d, a, v); }
static inline void rtw_write16(rtw_dev_t *d, u32 a, u16 v) { d->ops->write16(d, a, v); }
static inline void rtw_write8 (rtw_dev_t *d, u32 a, u8 v)  { d->ops->write8 (d, a, v); }

static inline void rtw_write8_set (rtw_dev_t *d, u32 a, u8 bits)  { rtw_write8 (d, a, (u8)(rtw_read8 (d, a) | bits)); }
static inline void rtw_write8_clr (rtw_dev_t *d, u32 a, u8 bits)  { rtw_write8 (d, a, (u8)(rtw_read8 (d, a) & ~bits)); }
static inline void rtw_write32_set(rtw_dev_t *d, u32 a, u32 bits) { rtw_write32(d, a, rtw_read32(d, a) | bits); }
static inline void rtw_write32_clr(rtw_dev_t *d, u32 a, u32 bits) { rtw_write32(d, a, rtw_read32(d, a) & ~bits); }
static inline u32 rtw_ffs(u32 x) {         /* find first set bit (__ffs) */
    u32 r = 0;
    if (!x) return 32;
    while (!(x & 1)) { x >>= 1; r++; }
    return r;
}
static inline void rtw_write32_mask(rtw_dev_t *d, u32 a, u32 mask, u32 val) {
    u32 shift = rtw_ffs(mask);
    u32 orig = rtw_read32(d, a);
    rtw_write32(d, a, (orig & ~mask) | ((val << shift) & mask));
}
static inline void rtw_sleep_ms(rtw_dev_t *d, u32 ms)            { d->ops->sleep_ms(d, ms); }

/* ---- firmware header (legacy 8051 path used by the 8822C) ---- */
typedef struct PACKED {
    u16 signature;     /* 0x00 */
    u8  category;      /* 0x02 */
    u8  function;      /* 0x03 */
    u16 version;       /* 0x04 */
    u8  subversion1;   /* 0x06 */
    u8  subversion2;   /* 0x07 */
    u8  month;         /* 0x08 */
    u8  day;           /* 0x09 */
    u8  hour;          /* 0x0A */
    u8  minute;        /* 0x0B */
    u16 size;          /* 0x0C  (payload bytes after the header) */
    u16 rsvd2;         /* 0x0E */
    u32 idx;           /* 0x10 */
    u32 rsvd3;         /* 0x14 */
    u32 rsvd4;         /* 0x18 */
    u32 rsvd5;         /* 0x1C */
} rtw_fw_hdr_t;         /* 32 bytes */

/* ---- entry points ---- */

/* PCI transport: detect + map BAR2 + power on.  Returns 0 on success. */
int  rtw_pci_probe(rtw_dev_t *d, u16 vendor, u16 device, u8 bus, u8 dev, u8 fn);

/* Firmware download: run the full legacy 8051 download handshake.
 * Returns 0 when the firmware is running. */
int  rtw_download_firmware(rtw_dev_t *d, const u8 *blob, u32 size);

/* True once RTW_FW_READY_LEGACY is observed. */
bool rtw_fw_running(const rtw_dev_t *d);

/* ---- EFUSE (MAC address) ----
 * Read one byte of the physical EFUSE (the register handshake). */
int  rtw_read8_physical_efuse(rtw_dev_t *d, u16 addr, u8 *data);
/* Reconstruct the logical EFUSE map (wear-leveled block decode). */
int  rtw_read_logical_efuse(rtw_dev_t *d, u8 *log_map, u32 log_size);
/* Read the station MAC (RTL8822CE: logical offset 0x120). */
int  rtw_read_mac(rtw_dev_t *d, u8 mac[6]);

int rtw_selftest(void);   /* 0 = ok (fake-chip download round-trip) */
int rtw_efuse_selftest(void);  /* 0 = ok (fake-chip EFUSE read + reconstruction) */
