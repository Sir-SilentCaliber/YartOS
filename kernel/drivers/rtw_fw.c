/* YartOS rtw88 port - firmware download state machine (legacy 8051 path).
 *
 * Transcribed from Linux rtw88 (mac.c: download_firmware_legacy /
 * download_firmware_validate_legacy / wlan_cpu_enable / rtw_write_firmware_page).
 * The RTL8822CE boots its firmware from host RAM: we stop the on-chip CPU,
 * put the chip in download mode, stream the firmware 32-bit word at a time
 * through the register window at RTW_FW_START_ADDR (0x1000) in 4096-byte
 * pages, then boot the CPU and wait for RTW_FW_READY_LEGACY.
 *
 * This file carries a selftest that runs the FULL handshake against a fake
 * chip register file emulating the silicon's side of the contract, so the
 * state machine is proven before it ever touches real hardware.
 */
#include <yart/types.h>
#include <yart/string.h>
#include <yart/console.h>
#include <yart/rtw88.h>
#include "rtw8822c_regs.h"

/* ---- CPU enable (verified against mac.c wlan_cpu_enable) ---- */
static void wlan_cpu_enable(rtw_dev_t *d, bool enable) {
    if (enable) {
        rtw_write8_set(d, RTW_REG_RSV_CTRL + 1, RTW_BIT_WLMCU_IOIF);
        rtw_write8_set(d, RTW_REG_SYS_FUNC_EN + 1, RTW_BIT_FEN_CPUEN);
    } else {
        rtw_write8_clr(d, RTW_REG_SYS_FUNC_EN + 1, RTW_BIT_FEN_CPUEN);
        rtw_write8_clr(d, RTW_REG_RSV_CTRL + 1, RTW_BIT_WLMCU_IOIF);
    }
}

/* Enable download mode; wait up to 10x20ms for the chip to latch it. */
static int en_download(rtw_dev_t *d, bool enable) {
    if (enable) {
        wlan_cpu_enable(d, false);
        wlan_cpu_enable(d, true);
        rtw_write8_set(d, RTW_REG_MCUFW_CTRL, RTW_BIT_MCUFWDL_EN);
        for (int try = 0; try < 10; try++) {
            if (rtw_read8(d, RTW_REG_MCUFW_CTRL) & RTW_BIT_MCUFWDL_EN)
                goto ready;
            rtw_write8_set(d, RTW_REG_MCUFW_CTRL, RTW_BIT_MCUFWDL_EN);
            rtw_sleep_ms(d, 20);
        }
        return -1;
ready:
        rtw_write32_clr(d, RTW_REG_MCUFW_CTRL, RTW_BIT_ROM_DLEN);
    } else {
        rtw_write8_clr(d, RTW_REG_MCUFW_CTRL, RTW_BIT_MCUFWDL_EN);
    }
    return 0;
}

/* Stream one page through the register window at 0x1000. */
static void write_firmware_page(rtw_dev_t *d, u32 page, const u8 *data, u32 size) {
    u32 block_nr = size >> 2;                 /* DLFW_BLK_SIZE_SHIFT = 2 */
    u32 remain = size & 3;
    u32 val = rtw_read32(d, RTW_REG_MCUFW_CTRL);
    val &= ~RTW_BITS_ROM_PGE;
    val |= (page << RTW_SHIFT_ROM_PGE) & RTW_BITS_ROM_PGE;
    rtw_write32(d, RTW_REG_MCUFW_CTRL, val);

    u32 addr = RTW_FW_START_ADDR;
    const u32 *ptr = (const u32 *)data;
    for (u32 b = 0; b < block_nr; b++) {
        rtw_write32(d, addr, ptr[b]);
        addr += RTW_DLFW_BLK_SIZE;
    }
    if (remain) {                             /* pad the tail word with zeros */
        u32 word = 0;
        memcpy(&word, data + block_nr * 4, remain);
        rtw_write32(d, addr, word);
    }
}

/* Wait for a bit to reach a value (check_hw_ready). */
static int check_hw_ready(rtw_dev_t *d, u32 addr, u32 mask, u32 want) {
    for (int i = 0; i < 100; i++) {
        if ((rtw_read32(d, addr) & mask) == want) return 0;
        rtw_sleep_ms(d, 20);
    }
    return -1;
}

/* ---- header validation (legacy path) ----
 * The 32-byte legacy header is skipped before download (verified against
 * mac.c download_firmware_legacy).  The `size` field is informational in the
 * legacy format, so we only require a non-empty payload and a non-zero
 * signature; the version is surfaced for bring-up logs. */
static int fw_check_header(const u8 *blob, u32 size, u16 *version) {
    if (size < RTW_FW_HDR_SIZE + 4) return -1;
    const rtw_fw_hdr_t *h = (const rtw_fw_hdr_t *)blob;
    if (h->signature == 0) return -1;
    if (version) *version = h->version;   /* LE field, host is LE */
    return 0;
}

int rtw_download_firmware(rtw_dev_t *d, const u8 *blob, u32 size) {
    u16 ver = 0;
    if (fw_check_header(blob, size, &ver)) {
        kprintf("rtw: firmware header invalid (size=%u)\n", size);
        return -1;
    }

    wlan_cpu_enable(d, false);

    if (en_download(d, true)) {
        kprintf("rtw: failed to enter firmware-download mode\n");
        return -1;
    }

    const u8 *data = blob + RTW_FW_HDR_SIZE;
    u32 remain = size - RTW_FW_HDR_SIZE;
    u32 total_page = remain >> 12;            /* DLFW_PAGE_SIZE_SHIFT = 12 */
    u32 last_page = remain & (RTW_DLFW_PAGE_SIZE - 1);

    rtw_write8_set(d, RTW_REG_MCUFW_CTRL, RTW_BIT_FWDL_CHK_RPT);

    u32 page;
    for (page = 0; page < total_page; page++) {
        write_firmware_page(d, page, data, RTW_DLFW_PAGE_SIZE);
        data += RTW_DLFW_PAGE_SIZE;
    }
    if (last_page)
        write_firmware_page(d, page, data, last_page);

    if (check_hw_ready(d, RTW_REG_MCUFW_CTRL, RTW_BIT_FWDL_CHK_RPT, RTW_BIT_FWDL_CHK_RPT)) {
        kprintf("rtw: firmware download checksum report timeout\n");
        en_download(d, false);
        return -1;
    }

    en_download(d, false);

    /* validate: boot the CPU and wait for FW_READY_LEGACY */
    u32 val = rtw_read32(d, RTW_REG_MCUFW_CTRL);
    val |= RTW_BIT_MCUFWDL_RDY;
    val &= ~RTW_BIT_WINTINI_RDY;
    rtw_write32(d, RTW_REG_MCUFW_CTRL, val);

    wlan_cpu_enable(d, false);
    wlan_cpu_enable(d, true);

    for (int try = 0; try < 10; try++) {
        u32 s = rtw_read32(d, RTW_REG_MCUFW_CTRL);
        if ((s & RTW_FW_READY_LEGACY) == RTW_FW_READY_LEGACY) {
            d->fw_loaded = true;
            d->fw_running = true;
            d->fw_version = ver;
            kprintf("rtw: firmware v%u.%u.%u running (chip answered)\n",
                    (ver >> 8) & 0xff, ver & 0xff, 0);
            return 0;
        }
        rtw_sleep_ms(d, 20);
    }
    kprintf("rtw: firmware validation timeout (MCUFW_CTRL=0x%x)\n",
            rtw_read32(d, RTW_REG_MCUFW_CTRL));
    return -1;
}

bool rtw_fw_running(const rtw_dev_t *d) { return d && d->fw_running; }

/* ===================== selftest: fake chip =====================
 * Emulates the silicon's side of the download contract:
 *   - MCUFWDL_EN latches when written
 *   - each fully-written page sets FWDL_CHK_RPT (on page switch / poll)
 *   - booting the CPU with MCUFWDL_RDY set -> FW_READY_LEGACY
 */
typedef struct {
    u32  mcu_ctrl;                          /* REG_MCUFW_CTRL (32-bit) */
    u8   sys_func_hi;                       /* SYS_FUNC_EN byte 1      */
    u8   rsv_hi;                            /* RSV_CTRL byte 1         */
    u32  fw_ram[RTW_DLFW_PAGE_SIZE / 4];
    u32  fw_words;
    u8   fw_image[3 * RTW_DLFW_PAGE_SIZE];
    u32  fw_image_len;
    bool page_dirty;
} fake_chip_t;

static fake_chip_t g_fake;

static u8 fake_read8(rtw_dev_t *d, u32 a) {
    (void)d;
    if (a == RTW_REG_MCUFW_CTRL)  return (u8)g_fake.mcu_ctrl;
    if (a == RTW_REG_SYS_FUNC_EN + 1) return g_fake.sys_func_hi;
    if (a == RTW_REG_RSV_CTRL + 1)    return g_fake.rsv_hi;
    return 0;
}
static u16 fake_read16(rtw_dev_t *d, u32 a) { return (u16)fake_read8(d, a); }
static u32 fake_read32(rtw_dev_t *d, u32 a) {
    (void)d;
    if (a == RTW_REG_MCUFW_CTRL) return g_fake.mcu_ctrl;
    return 0;
}

/* The chip has processed the current page: append it to the image. */
static void fake_commit_page(void) {
    if (!g_fake.page_dirty) return;
    for (u32 i = 0; i < g_fake.fw_words; i++) {
        u32 off = g_fake.fw_image_len;
        if (off + 4 <= sizeof g_fake.fw_image) {
            g_fake.fw_image[off + 0] = (u8)g_fake.fw_ram[i];
            g_fake.fw_image[off + 1] = (u8)(g_fake.fw_ram[i] >> 8);
            g_fake.fw_image[off + 2] = (u8)(g_fake.fw_ram[i] >> 16);
            g_fake.fw_image[off + 3] = (u8)(g_fake.fw_ram[i] >> 24);
            g_fake.fw_image_len += 4;
        }
    }
    g_fake.fw_words = 0;
    g_fake.page_dirty = false;
}

static void fake_write8(rtw_dev_t *d, u32 a, u8 v) {
    (void)d;
    if (a == RTW_REG_MCUFW_CTRL) {
        g_fake.mcu_ctrl = (g_fake.mcu_ctrl & ~0xFFu) | v;
        /* the driver sets CHK_RPT as a REQUEST; the chip clears it while
         * it processes the download and re-asserts it when done */
        if (v & RTW_BIT_FWDL_CHK_RPT)
            g_fake.mcu_ctrl &= ~(u32)RTW_BIT_FWDL_CHK_RPT;
        return;
    }
    if (a == RTW_REG_SYS_FUNC_EN + 1) {
        bool was_off = !(g_fake.sys_func_hi & RTW_BIT_FEN_CPUEN);
        g_fake.sys_func_hi = v;
        if (was_off && (v & RTW_BIT_FEN_CPUEN) &&
            (g_fake.mcu_ctrl & RTW_BIT_MCUFWDL_RDY))
            g_fake.mcu_ctrl |= RTW_FW_READY_LEGACY;   /* firmware booted */
        return;
    }
    if (a == RTW_REG_RSV_CTRL + 1) { g_fake.rsv_hi = v; return; }
}
static void fake_write16(rtw_dev_t *d, u32 a, u16 v) { fake_write8(d, a, (u8)v); }
static void fake_write32(rtw_dev_t *d, u32 a, u32 v) {
    (void)d;
    if (a == RTW_REG_MCUFW_CTRL) {
        u32 old_pge = g_fake.mcu_ctrl & RTW_BITS_ROM_PGE;
        u32 new_pge = v & RTW_BITS_ROM_PGE;
        if (old_pge != new_pge) fake_commit_page();   /* page switch = done */
        g_fake.mcu_ctrl = v;                          /* full 32-bit write */
        return;
    }
    if (a >= RTW_FW_START_ADDR && a < RTW_FW_START_ADDR + RTW_DLFW_PAGE_SIZE) {
        u32 idx = (a - RTW_FW_START_ADDR) / 4;
        if (idx < RTW_DLFW_PAGE_SIZE / 4) {
            g_fake.fw_ram[idx] = v;
            if (idx + 1 > g_fake.fw_words) g_fake.fw_words = idx + 1;
            g_fake.page_dirty = true;
        }
        return;
    }
}
static void fake_sleep(rtw_dev_t *d, u32 ms) {
    (void)d; (void)ms;
    /* polling tick: the chip finishes the pending page and reports its
     * download checksum via FWDL_CHK_RPT */
    if (g_fake.mcu_ctrl & RTW_BIT_MCUFWDL_EN) {
        fake_commit_page();
        if (g_fake.fw_image_len) g_fake.mcu_ctrl |= RTW_BIT_FWDL_CHK_RPT;
    }
}

static const rtw_hci_ops_t fake_ops = {
    .read8 = fake_read8, .read16 = fake_read16, .read32 = fake_read32,
    .write8 = fake_write8, .write16 = fake_write16, .write32 = fake_write32,
    .sleep_ms = fake_sleep,
};

int rtw_selftest(void) {
    /* synthetic firmware: 32-byte header + 2 full pages + 100-byte tail */
    u32 payload = 2 * RTW_DLFW_PAGE_SIZE + 100;
    u8 blob[RTW_FW_HDR_SIZE + 2 * RTW_DLFW_PAGE_SIZE + 100];
    memset(blob, 0, sizeof blob);
    rtw_fw_hdr_t *h = (rtw_fw_hdr_t *)blob;
    h->signature = 0x1188;                    /* LE: bytes 0x88 0x11 */
    h->category = 0x01;
    h->function = 0x00;
    h->version = 0x0009;                      /* LE */
    h->subversion1 = 0x02;
    h->subversion2 = 0x00;
    h->month = 1; h->day = 1; h->hour = 0; h->minute = 0;
    h->size = (u16)payload;                   /* LE */
    for (u32 i = 0; i < payload; i++)
        blob[RTW_FW_HDR_SIZE + i] = (u8)(i * 13 + 5);

    memset(&g_fake, 0, sizeof g_fake);
    rtw_dev_t d;
    memset(&d, 0, sizeof d);
    d.ops = &fake_ops;

    if (rtw_download_firmware(&d, blob, sizeof blob)) return 1;
    if (!d.fw_running || !d.fw_loaded) return 2;
    if (d.fw_version != 0x0009) return 3;

    /* the fake chip must have reconstructed the exact payload */
    if (g_fake.fw_image_len != payload) return 4;
    if (memcmp(g_fake.fw_image, blob + RTW_FW_HDR_SIZE, payload)) return 5;

    /* a corrupt header (zeroed signature) must be rejected */
    memset(&g_fake, 0, sizeof g_fake);
    rtw_dev_t d2;
    memset(&d2, 0, sizeof d2);
    d2.ops = &fake_ops;
    blob[0] = 0; blob[1] = 0;                /* clobber the signature */
    if (rtw_download_firmware(&d2, blob, sizeof blob) == 0) return 6;

    return 0;
}
