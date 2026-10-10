/* YartOS rtw88 port - EFUSE (MAC address) read.
 *
 * Transcribed from Linux rtw88 efuse.c.  The RTL8822CE stores its MAC in
 * one-time-programmable EFUSE cells.  Reads happen through a register
 * handshake on REG_EFUSE_CTRL:
 *   1. write the byte address into bits 8..17
 *   2. clear BIT_EF_FLAG (bit 31) -> the chip latches the address
 *   3. poll BIT_EF_FLAG until the chip sets it (read done)
 *   4. read the data from the low 8 bits
 *
 * The physical EFUSE is a sequence of wear-leveled blocks (write-once cells;
 * later writes supersede earlier ones).  rtw_read_logical_efuse() decodes the
 * 1-byte/2-byte block headers into a flat logical map, from which the MAC is
 * taken at RTW_EFUSE_MAC_OFFSET (0x120 for the 8822CE).
 *
 * The selftest emulates the chip's EFUSE contract with a fake register file
 * holding a hand-built physical map, and asserts the logical reconstruction
 * and the MAC extraction.
 */
#include <yart/types.h>
#include <yart/string.h>
#include <yart/console.h>
#include <yart/rtw88.h>
#include "rtw8822c_regs.h"

int rtw_read8_physical_efuse(rtw_dev_t *d, u16 addr, u8 *data) {
    rtw_write32_mask(d, RTW_REG_EFUSE_CTRL,
                     (u32)RTW_MASK_EF_ADDR << RTW_SHIFT_EF_ADDR, (u32)addr);
    rtw_write32_clr(d, RTW_REG_EFUSE_CTRL, RTW_BIT_EF_FLAG);
    for (int i = 0; i < 100000; i++) {
        if (rtw_read32(d, RTW_REG_EFUSE_CTRL) & RTW_BIT_EF_FLAG) {
            *data = (u8)(rtw_read8(d, RTW_REG_EFUSE_CTRL) & RTW_MASK_EF_DATA);
            return 0;
        }
        rtw_sleep_ms(d, 1);
    }
    *data = 0;
    return -1;
}

/* ---- block-header decode (verified against efuse.c) ---- */
#define EF_INVALID_HDR(h1, h2)  ((h1) == 0xff || (((h1) & 0x1f) == 0xf && (h2) == 0xff))
#define EF_BLK2(h1, h2)  ((((h2) & 0xf0) >> 1) | (((h1) >> 5) & 0x07))
#define EF_BLK1(h1)      (((h1) & 0xf0) >> 4)
#define EF_LOGIDX(blk, i) (((blk) << 3) + ((i) << 1))

static int rtw_dump_physical_efuse(rtw_dev_t *d, u8 *map, u32 size) {
    for (u32 a = 0; a < size; a++)
        if (rtw_read8_physical_efuse(d, (u16)a, &map[a])) return -1;
    return 0;
}

int rtw_read_logical_efuse(rtw_dev_t *d, u8 *log_map, u32 log_size) {
    u8 phys[RTW_EFUSE_PHYS_SIZE];
    if (rtw_dump_physical_efuse(d, phys, RTW_EFUSE_PHYS_SIZE - RTW_EFUSE_PROTECT_SIZE))
        return -1;
    memset(log_map, 0xff, log_size);

    u32 phy_idx = 0;
    u32 limit = RTW_EFUSE_PHYS_SIZE - RTW_EFUSE_PROTECT_SIZE;
    while (phy_idx < limit) {
        u8 h1 = phys[phy_idx];
        u8 h2 = (phy_idx + 1 < limit) ? phys[phy_idx + 1] : 0xff;
        if (EF_INVALID_HDR(h1, h2)) break;

        u32 blk; u8 word_en;
        if ((h1 & 0x1f) == 0xf) {       /* 2-byte header */
            blk = EF_BLK2(h1, h2);
            word_en = h2 & 0xf;
            phy_idx += 2;
        } else {                        /* 1-byte header */
            blk = EF_BLK1(h1);
            word_en = h1 & 0xf;
            phy_idx += 1;
        }
        for (int i = 0; i < 4; i++) {
            if (word_en & (1u << i)) continue;   /* word not written */
            u32 log_idx = EF_LOGIDX(blk, i);
            if (phy_idx + 1 >= limit || log_idx + 1 >= log_size) return -1;
            log_map[log_idx]     = phys[phy_idx];
            log_map[log_idx + 1] = phys[phy_idx + 1];
            phy_idx += 2;
        }
    }
    return 0;
}

int rtw_read_mac(rtw_dev_t *d, u8 mac[6]) {
    u8 log[RTW_EFUSE_LOG_SIZE];
    if (rtw_read_logical_efuse(d, log, sizeof log)) return -1;
    memcpy(mac, log + RTW_EFUSE_MAC_OFFSET, 6);
    /* sanity: an all-0xff MAC means the cells were never programmed */
    for (int i = 0; i < 6; i++) if (mac[i] != 0xff) return 0;
    return -1;
}

/* ===================== selftest: fake EFUSE chip ===================== */
static struct {
    u32  ef_ctrl;
    u16  ef_addr;
    u8   phy_map[RTW_EFUSE_PHYS_SIZE];
} g_ef;

static u8  ef_read8(rtw_dev_t *d, u32 a) {
    (void)d;
    if (a == RTW_REG_EFUSE_CTRL)
        return g_ef.phy_map[g_ef.ef_addr];      /* data byte */
    return 0;
}
static u16 ef_read16(rtw_dev_t *d, u32 a) { return ef_read8(d, a); }
static u32 ef_read32(rtw_dev_t *d, u32 a) {
    (void)d;
    if (a == RTW_REG_EFUSE_CTRL) return g_ef.ef_ctrl;
    return 0;
}
static void ef_write8(rtw_dev_t *d, u32 a, u8 v)  { (void)d; (void)a; (void)v; }
static void ef_write16(rtw_dev_t *d, u32 a, u16 v) { (void)d; (void)a; (void)v; }
static void ef_write32(rtw_dev_t *d, u32 a, u32 v) {
    (void)d;
    if (a == RTW_REG_EFUSE_CTRL) {
        if (!(v & RTW_BIT_EF_FLAG)) {            /* new read request */
            g_ef.ef_addr = (u16)((v >> RTW_SHIFT_EF_ADDR) & RTW_MASK_EF_ADDR);
            g_ef.ef_ctrl = v | RTW_BIT_EF_FLAG;  /* instantly done */
        } else {
            g_ef.ef_ctrl = v;
        }
    }
}
static void ef_sleep(rtw_dev_t *d, u32 ms) { (void)d; (void)ms; }

static const rtw_hci_ops_t ef_ops = {
    .read8 = ef_read8, .read16 = ef_read16, .read32 = ef_read32,
    .write8 = ef_write8, .write16 = ef_write16, .write32 = ef_write32,
    .sleep_ms = ef_sleep,
};

int rtw_efuse_selftest(void) {
    static const u8 want_mac[6] = {0x20,0x0a,0x5c,0x33,0x22,0x11};

    memset(&g_ef, 0, sizeof g_ef);
    memset(g_ef.phy_map, 0xff, sizeof g_ef.phy_map);   /* unprogrammed cells */
    /* hand-built physical map: one 2-byte-header block for logical block 36
     * (logical addr 0x120), word_en = 0x8 (word 3 not written -> 3 words =
     * 6 bytes of MAC), then a 0xff terminator. */
    g_ef.phy_map[0] = 0x8f;      /* hdr1: 0xf | ((36 & 7) << 5) = 0xf | 0x80 */
    g_ef.phy_map[1] = 0x48;      /* hdr2: ((36 >> 3) << 4) | 0x8 = 0x40 | 0x8 */
    memcpy(&g_ef.phy_map[2], want_mac, 6);

    rtw_dev_t d;
    memset(&d, 0, sizeof d);
    d.ops = &ef_ops;

    /* 1. physical read round-trip */
    u8 byte = 0;
    if (rtw_read8_physical_efuse(&d, 2, &byte) || byte != want_mac[0]) return 1;
    if (rtw_read8_physical_efuse(&d, 7, &byte) || byte != want_mac[5]) return 2;

    /* 2. logical reconstruction */
    u8 log[RTW_EFUSE_LOG_SIZE];
    if (rtw_read_logical_efuse(&d, log, sizeof log)) return 3;
    if (memcmp(log + RTW_EFUSE_MAC_OFFSET, want_mac, 6)) return 4;

    /* 3. MAC extraction */
    u8 mac[6];
    if (rtw_read_mac(&d, mac) || memcmp(mac, want_mac, 6)) return 5;

    /* 4. blank EFUSE -> MAC read must fail (all 0xff) */
    memset(&g_ef, 0, sizeof g_ef);
    memset(g_ef.phy_map, 0xff, sizeof g_ef.phy_map);
    if (rtw_read_mac(&d, mac) == 0) return 6;

    return 0;
}
