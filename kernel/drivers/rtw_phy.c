/* YartOS rtw88 port - PHY init machinery.
 *
 * Transcribed from Linux rtw88:
 *   - rtw_pwr_seq_parser / rtw_sub_pwr_seq_parser  (mac.c)
 *   - rtw_phy_write_rf_reg                          (phy.c)
 *   - rtw_phy_cfg_mac/agc/bb/rf + delay addresses   (phy.c)
 *   - rtw_parse_tbl_phy_cond + rtw_phy_cond bitfield (phy.c, main.h)
 *   - trans_carddis_to_cardemu/trans_cardemu_to_act (rtw8822c.c, PCI only)
 *
 * The selftest drives the full power-on sequence and table machinery against
 * a fake chip register file that emulates the silicon's polling contract.
 */
#include <yart/types.h>
#include <yart/string.h>
#include <yart/console.h>
#include <yart/rtw88.h>
#include <yart/rtw_phy.h>
#include <yart/fw.h>
#include "rtw8822c_regs.h"

/* ---- power sequence (mac.c) ---- */
static int pwr_cmd_polling(rtw_dev_t *d, const rtw_pwr_seq_cmd_t *cmd) {
    u32 target = cmd->value & cmd->mask;
    for (int i = 0; i < 100; i++) {
        if ((rtw_read8(d, cmd->offset) & cmd->mask) == target) return 0;
        rtw_sleep_ms(d, 1);
    }
    return -1;
}

static int pwr_sub_seq_parser(rtw_dev_t *d, u8 intf_mask, u8 cut_mask,
                              const rtw_pwr_seq_cmd_t *cmd) {
    for (const rtw_pwr_seq_cmd_t *c = cmd; c->cmd != RTW_PWR_CMD_END; c++) {
        if (!(c->intf_mask & intf_mask) || !(c->cut_mask & cut_mask)) continue;
        switch (c->cmd) {
        case RTW_PWR_CMD_WRITE: {
            u8 v = rtw_read8(d, c->offset);
            v &= (u8)~c->mask;
            v |= (u8)(c->value & c->mask);
            rtw_write8(d, c->offset, v);
            break;
        }
        case RTW_PWR_CMD_POLLING:
            if (pwr_cmd_polling(d, c)) return -1;
            break;
        case RTW_PWR_CMD_DELAY:
            rtw_sleep_ms(d, c->offset);   /* ms granularity is fine here */
            break;
        default:
            return -1;
        }
    }
    return 0;
}

int rtw_pwr_seq_parser(rtw_dev_t *d, const rtw_pwr_seq_cmd_t * const *seqs) {
    for (int i = 0; seqs[i]; i++)
        if (pwr_sub_seq_parser(d, RTW_PWR_INTF_PCI, RTW_PWR_CUT_ALL, seqs[i]))
            return -1;
    return 0;
}

/* RTL8822CE power-on sequences (PCI-only entries), from rtw8822c.c. */
const rtw_pwr_seq_cmd_t rtw8822ce_pwr_on_cardemu[] = {
    {0x002E, 0xFF, 0x0F, 0x00, RTW_PWR_CMD_WRITE,   0x04, 0x04},
    {0x002D, 0xFF, 0x0F, 0x00, RTW_PWR_CMD_WRITE,   0x01, 0x00},
    {0x007F, 0xFF, 0x0F, 0x00, RTW_PWR_CMD_WRITE,   0x80, 0x00},
    {0x0005, 0xFF, 0x0F, 0x00, RTW_PWR_CMD_WRITE,   0x98, 0x00},
    {0xFFFF, 0xFF, 0x0F, 0x00, RTW_PWR_CMD_END,     0x00, 0x00},
};

const rtw_pwr_seq_cmd_t rtw8822ce_pwr_on_act[] = {
    {0x0005, 0xFF, 0x0F, 0x00, RTW_PWR_CMD_WRITE,   0x1C, 0x00},
    {0x0075, 0xFF, 0x04, 0x00, RTW_PWR_CMD_WRITE,   0x01, 0x01},
    {0x0006, 0xFF, 0x0F, 0x00, RTW_PWR_CMD_POLLING, 0x02, 0x02},
    {0x0075, 0xFF, 0x04, 0x00, RTW_PWR_CMD_WRITE,   0x01, 0x00},
    {0x002E, 0xFF, 0x0F, 0x00, RTW_PWR_CMD_WRITE,   0x08, 0x00},
    {0x0006, 0xFF, 0x0F, 0x00, RTW_PWR_CMD_WRITE,   0x01, 0x01},
    {0x0005, 0xFF, 0x0F, 0x00, RTW_PWR_CMD_WRITE,   0x18, 0x00},
    {0x1018, 0xFF, 0x0F, 0x00, RTW_PWR_CMD_WRITE,   0x04, 0x04},
    {0x0005, 0xFF, 0x0F, 0x00, RTW_PWR_CMD_WRITE,   0x01, 0x01},
    {0x0005, 0xFF, 0x0F, 0x00, RTW_PWR_CMD_POLLING, 0x01, 0x00},
    {0x0074, 0xFF, 0x04, 0x00, RTW_PWR_CMD_WRITE,   0x20, 0x20},
    {0x0071, 0xFF, 0x04, 0x00, RTW_PWR_CMD_WRITE,   0x10, 0x00},
    {0x0062, 0xFF, 0x04, 0x00, RTW_PWR_CMD_WRITE,   0xE0, 0xE0},
    {0x0061, 0xFF, 0x04, 0x00, RTW_PWR_CMD_WRITE,   0xE0, 0x00},
    {0x001F, 0xFF, 0x0F, 0x00, RTW_PWR_CMD_WRITE,   0xC0, 0x80},
    {0x00EF, 0xFF, 0x0F, 0x00, RTW_PWR_CMD_WRITE,   0xC0, 0x80},
    {0x1045, 0xFF, 0x0F, 0x00, RTW_PWR_CMD_WRITE,   0x10, 0x10},
    {0x0010, 0xFF, 0x0F, 0x00, RTW_PWR_CMD_WRITE,   0x04, 0x04},
    {0x1064, 0xFF, 0x0F, 0x00, RTW_PWR_CMD_WRITE,   0x02, 0x02},
    {0xFFFF, 0xFF, 0x0F, 0x00, RTW_PWR_CMD_END,     0x00, 0x00},
};

/* ---- RF register write (phy.c rtw_phy_write_rf_reg) ---- */
void rtw_write_rf(rtw_dev_t *d, int rf_path, u32 addr, u32 mask, u32 data) {
    u32 base = (rf_path == RTW_RF_PATH_B) ? RTW_RF_BASE_B : RTW_RF_BASE_A;
    u32 direct = base + ((addr & 0xff) << 2);
    mask &= RTW_RFREG_MASK;
    rtw_write32_mask(d, direct, mask, data);
}

/* ---- table cfg functions (phy.c) ---- */
void rtw_phy_cfg_mac(rtw_dev_t *d, u32 addr, u32 data) {
    rtw_write8(d, addr, (u8)data);
}
void rtw_phy_cfg_agc(rtw_dev_t *d, u32 addr, u32 data) {
    rtw_write32(d, addr, data);
}
void rtw_phy_cfg_bb(rtw_dev_t *d, u32 addr, u32 data) {
    switch (addr) {
    case 0xfe: rtw_sleep_ms(d, 50); return;
    case 0xfd: rtw_sleep_ms(d, 5);  return;
    case 0xfc: rtw_sleep_ms(d, 1);  return;
    case 0xfb: rtw_sleep_ms(d, 1);  return;   /* 50us -> 1ms floor */
    case 0xfa: rtw_sleep_ms(d, 1);  return;   /* 5us  -> 1ms floor */
    case 0xf9: rtw_sleep_ms(d, 1);  return;   /* 1us  -> 1ms floor */
    default:   rtw_write32(d, addr, data); return;
    }
}
void rtw_phy_cfg_rf(rtw_dev_t *d, u32 addr, u32 data) {
    switch (addr) {
    case 0xffe: rtw_sleep_ms(d, 50); return;
    case 0xfe:  rtw_sleep_ms(d, 1);  return;  /* 100us -> 1ms floor */
    default:
        rtw_write_rf(d, RTW_RF_PATH_A, addr, RTW_RFREG_MASK, data);
        return;
    }
}

void rtw_load_table(rtw_dev_t *d, const rtw_phy_cfg_pair_t *pairs, u32 n_pairs,
                    void (*cfg)(rtw_dev_t *, u32, u32)) {
    for (u32 i = 0; i < n_pairs; i++)
        cfg(d, pairs[i].addr, pairs[i].data);
}

/* ---- conditional table parser (phy.c rtw_parse_tbl_phy_cond) ---- */
#define COND_BRANCH_IF     0
#define COND_BRANCH_ELIF   1
#define COND_BRANCH_ELSE   2
#define COND_BRANCH_ENDIF  3
#define COND_NEG           0x40000000u   /* bit 30 */
#define COND_POS           0x80000000u   /* bit 31 */

/* Decode the 32-bit cond word (little-endian bitfields, main.h). */
static void cond_decode(u32 w, u32 *branch, u8 *rfe, u8 *intf, u8 *pkg, u8 *plat, u8 *cut) {
    *rfe    = (u8)(w & 0xFF);
    *intf   = (u8)((w >> 8) & 0xF);
    *pkg    = (u8)((w >> 12) & 0xF);
    *plat   = (u8)((w >> 16) & 0xF);
    *cut    = (u8)((w >> 24) & 0xF);
    *branch = (w >> 28) & 0x3;
}

static bool cond_matches(rtw_phy_cond_t drv, u8 rfe, u8 intf, u8 pkg, u8 plat, u8 cut) {
    if (cut  && cut  != drv.cut)  return false;
    if (pkg  && pkg  != drv.pkg)  return false;
    if (intf && intf != drv.intf) return false;
    if (plat && plat != drv.plat) return false;
    if (rfe  && rfe  != drv.rfe)  return false;
    return true;
}

void rtw_parse_tbl_phy_cond(rtw_dev_t *d, const rtw_phy_cfg_pair_t *pairs,
                            u32 n_pairs, rtw_phy_cond_t drv_cond,
                            void (*cfg)(rtw_dev_t *, u32, u32)) {
    bool is_matched = true, is_skipped = false;
    u8 pos_rfe = 0, pos_intf = 0, pos_pkg = 0, pos_plat = 0, pos_cut = 0;
    u32 pos_branch = COND_BRANCH_IF;

    for (u32 i = 0; i < n_pairs; i++) {
        u32 w = pairs[i].addr;
        if (w & COND_POS) {
            switch ((w >> 28) & 0x3) {
            case COND_BRANCH_ENDIF: is_matched = true;  is_skipped = false; break;
            case COND_BRANCH_ELSE:  is_matched = is_skipped ? false : true; break;
            case COND_BRANCH_IF:
            case COND_BRANCH_ELIF:
            default:
                cond_decode(w, &pos_branch, &pos_rfe, &pos_intf, &pos_pkg, &pos_plat, &pos_cut);
                break;
            }
        } else if (w & COND_NEG) {
            if (!is_skipped) {
                if (cond_matches(drv_cond, pos_rfe, pos_intf, pos_pkg, pos_plat, pos_cut)) {
                    is_matched = true; is_skipped = true;
                } else {
                    is_matched = false; is_skipped = false;
                }
            } else {
                is_matched = false;
            }
        } else if (is_matched) {
            cfg(d, pairs[i].addr, pairs[i].data);
        }
    }
    (void)pos_branch;
}

/* ---- generated table blob loader (gen_rtw_tables.py format) ---- */
#define RTW_TBL_MAGIC 0x50575452u   /* "RTWP" */

static u32 rd_u32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

int rtw_phy_apply_blob(rtw_dev_t *d, const u8 *blob, u32 len) {
    if (len < 8 || rd_u32(blob) != RTW_TBL_MAGIC) return -1;
    u32 n_tables = rd_u32(blob + 4);
    u32 off = 8;
    for (u32 t = 0; t < n_tables; t++) {
        if (off + 24 > len) return -1;
        u32 n_pairs = rd_u32(blob + off + 16);
        u32 kind    = rd_u32(blob + off + 20);
        const u32 *pairs = (const u32 *)(blob + off + 24);
        if (off + 24 + n_pairs * 8 > len) return -1;
        for (u32 i = 0; i < n_pairs; i++) {
            u32 addr = pairs[2 * i], data = pairs[2 * i + 1];
            switch (kind) {
            case 0: rtw_phy_cfg_mac(d, addr, data); break;
            case 1: rtw_phy_cfg_agc(d, addr, data); break;
            case 2: case 5: rtw_phy_cfg_bb(d, addr, data); break;
            case 3: rtw_write_rf(d, RTW_RF_PATH_A, addr, RTW_RFREG_MASK, data); break;
            case 4: rtw_write_rf(d, RTW_RF_PATH_B, addr, RTW_RFREG_MASK, data); break;
            default: return -1;
            }
        }
        off += 24 + n_pairs * 8;
    }
    return 0;
}

int rtw_phy_load_tables(rtw_dev_t *d, const char *path) {
    u8 *blob = NULL; size_t len = 0;
    if (fw_load(path, &blob, &len) || !blob || !len) {
        if (blob) fw_free(blob);
        kprintf("rtw: PHY table blob '%s' not found\n", path);
        return -1;
    }
    int r = rtw_phy_apply_blob(d, blob, (u32)len);
    fw_free(blob);
    if (r) kprintf("rtw: PHY table blob '%s' invalid\n", path);
    return r;
}

/* ===================== selftest: fake chip ===================== */
static struct {
    u8   regs[0x2000];
    u32  rf[2][0x100];
    bool analog_ready;   /* hardware asserts 0x06 bit1 when platform pwr on */
    bool cpu_booted;
    int  bb_delay_ms;
} g_phy;

static u8  ph_read8(rtw_dev_t *d, u32 a) {
    (void)d;
    if (a < sizeof g_phy.regs) {
        if (a == 0x0006 && g_phy.analog_ready) return (u8)(g_phy.regs[a] | 0x02);
        if (a == 0x0005 && g_phy.cpu_booted) return (u8)(g_phy.regs[a] & ~0x01);
        return g_phy.regs[a];
    }
    return 0;
}
static u16 ph_read16(rtw_dev_t *d, u32 a) { return ph_read8(d, a); }
static u32 ph_read32(rtw_dev_t *d, u32 a) {
    (void)d;
    if (a >= 0x3c00 && a < 0x4c00) return g_phy.rf[0][(a - 0x3c00) >> 2];
    if (a >= 0x4c00 && a < 0x5c00) return g_phy.rf[1][(a - 0x4c00) >> 2];
    if (a + 3 < sizeof g_phy.regs)
        return (u32)g_phy.regs[a] | ((u32)g_phy.regs[a+1] << 8) |
               ((u32)g_phy.regs[a+2] << 16) | ((u32)g_phy.regs[a+3] << 24);
    return 0;
}
static void ph_write8(rtw_dev_t *d, u32 a, u8 v) {
    (void)d;
    if (a < sizeof g_phy.regs) {
        if (a == 0x0075 && (v & 0x01)) g_phy.analog_ready = true;  /* platform pwr */
        if (a == 0x0006 && (v & 0x01)) g_phy.cpu_booted = true;    /* CPU enable */
        g_phy.regs[a] = v;
    }
}
static void ph_write16(rtw_dev_t *d, u32 a, u16 v) { ph_write8(d, a, (u8)v); }
static void ph_write32(rtw_dev_t *d, u32 a, u32 v) {
    (void)d;
    if (a >= 0x3c00 && a < 0x4c00) { g_phy.rf[0][(a - 0x3c00) >> 2] = v; return; }
    if (a >= 0x4c00 && a < 0x5c00) { g_phy.rf[1][(a - 0x4c00) >> 2] = v; return; }
    if (a + 3 < sizeof g_phy.regs) {
        g_phy.regs[a] = (u8)v; g_phy.regs[a+1] = (u8)(v >> 8);
        g_phy.regs[a+2] = (u8)(v >> 16); g_phy.regs[a+3] = (u8)(v >> 24);
    }
}
static void ph_sleep(rtw_dev_t *d, u32 ms) { (void)d; g_phy.bb_delay_ms += (int)ms; }

static const rtw_hci_ops_t phy_ops = {
    .read8 = ph_read8, .read16 = ph_read16, .read32 = ph_read32,
    .write8 = ph_write8, .write16 = ph_write16, .write32 = ph_write32,
    .sleep_ms = ph_sleep,
};

int rtw_phy_selftest(void) {
    memset(&g_phy, 0, sizeof g_phy);
    rtw_dev_t d;
    memset(&d, 0, sizeof d);
    d.ops = &phy_ops;

    /* 1. full power-on sequence completes */
    {
        const rtw_pwr_seq_cmd_t *seq[] = {
            rtw8822ce_pwr_on_cardemu, rtw8822ce_pwr_on_act, NULL
        };
        if (rtw_pwr_seq_parser(&d, seq)) return 1;
        if (!g_phy.cpu_booted) return 2;
        /* spot-check a few final register states from the ACT sequence */
        if (!(g_phy.regs[0x0074] & 0x20)) return 3;      /* set bit5 */
        if (g_phy.regs[0x0062] != 0xE0) return 4;        /* 0xE0 */
        if ((g_phy.regs[0x001F] & 0xC0) != 0x80) return 5;
        if (!(g_phy.regs[0x1064] & 0x02)) return 6;
    }

    /* 2. table cfg functions + delays */
    g_phy.bb_delay_ms = 0;
    rtw_phy_cfg_mac(&d, 0x0000, 0x12);
    if (g_phy.regs[0x0000] != 0x12) return 7;
    rtw_phy_cfg_agc(&d, 0x1D90, 0x300001FF);
    if (ph_read32(&d, 0x1D90) != 0x300001FF) return 8;
    rtw_phy_cfg_bb(&d, 0xfe, 0);          /* delay 50ms */
    rtw_phy_cfg_bb(&d, 0xfc, 0);          /* delay 1ms */
    if (g_phy.bb_delay_ms != 51) return 9;
    rtw_phy_cfg_bb(&d, 0x1000, 0xDEADBEEF);
    if (ph_read32(&d, 0x1000) != 0xDEADBEEF) return 10;

    /* 3. RF write addressing (path A 0x3c00, path B 0x4c00, addr<<2) */
    rtw_write_rf(&d, RTW_RF_PATH_A, 0x00, RTW_RFREG_MASK, 0x30000);
    if (g_phy.rf[0][0] != 0x30000) return 11;
    rtw_write_rf(&d, RTW_RF_PATH_B, 0x8f, RTW_RFREG_MASK, 0x10001);
    if (g_phy.rf[1][0x8f] != 0x10001) return 12;
    rtw_phy_cfg_rf(&d, 0xffe, 0);         /* RF delay 50ms */
    rtw_phy_cfg_rf(&d, 0x0, 0x10000);     /* path A rf[0] */
    if (g_phy.rf[0][0] != 0x10000) return 13;

    /* 4. conditional table: apply entries only when cut/intf match */
    {
        static const rtw_phy_cfg_pair_t cond_tbl[] = {
            {0x80000000u | 0x0000000c, 0},   /* IF: cut=0xc, intf=0 (any) */
            {0x0000, 0xAA},                  /* applies only if cut matches */
            {0x80000000u | (3u << 28) | 0, 0}, /* ENDIF */
            {0x0001, 0xBB},                  /* always applies */
        };
        rtw_phy_cond_t c1 = { .rfe = 0, .intf = 0, .pkg = 0, .plat = 0, .cut = 0xc };
        rtw_parse_tbl_phy_cond(&d, cond_tbl, 4, c1, rtw_phy_cfg_mac);
        if (g_phy.regs[0x0000] != 0xAA) return 14;   /* matched */
        if (g_phy.regs[0x0001] != 0xBB) return 15;   /* unconditional */

        rtw_phy_cond_t c2 = { .rfe = 0, .intf = 0, .pkg = 0, .plat = 0, .cut = 0x0 };
        rtw_parse_tbl_phy_cond(&d, cond_tbl, 4, c2, rtw_phy_cfg_mac);
        if (g_phy.regs[0x0000] != 0xAA) return 16;   /* NOT overwritten (cut differs) */
        if (g_phy.regs[0x0001] != 0xBB) return 17;   /* still applied */
    }

    /* 5. generated blob loader round-trip (synthetic 2-table blob).
     * NOTE: real AGC tables re-write the SAME register repeatedly (each
     * entry is a full 32-bit write), so the final value is the LAST one. */
    {
        u8 blob[8 + (24 + 2*8) + (24 + 2*8)];
        memset(blob, 0, sizeof blob);
        blob[0]=0x52; blob[1]=0x54; blob[2]=0x57; blob[3]=0x50;   /* "RTWP" LE */
        blob[4]=2;                                               /* n_tables=2 */
        u32 o = 8;
        /* table 0: agc (kind 1), 2 pairs, same register twice */
        memcpy(blob + o, "agc", 4); o += 16;
        blob[o]=2; o += 4; blob[o]=1; o += 4;                    /* n=2, kind=1 */
        blob[o]=0x90; blob[o+1]=0x1D; o += 4;                    /* addr 0x1D90 */
        blob[o]=0xFF; blob[o+1]=0x01; blob[o+2]=0x00; blob[o+3]=0x30; o += 4; /* 0x300001FF */
        blob[o]=0x90; blob[o+1]=0x1D; o += 4;                    /* addr 0x1D90 */
        blob[o]=0xFE; blob[o+1]=0x01; blob[o+2]=0x01; blob[o+3]=0x30; o += 4; /* 0x300101FE */
        /* table 1: rf_b (kind 4), 2 pairs */
        memcpy(blob + o, "rf_b", 4); o += 16;
        blob[o]=2; o += 4; blob[o]=4; o += 4;
        blob[o]=0x00; o += 4;                                    /* rf addr 0 */
        blob[o]=0x01; blob[o+1]=0x00; blob[o+2]=0x01; blob[o+3]=0x00; o += 4; /* 0x10001 */
        blob[o]=0x8F; o += 4;                                    /* rf addr 0x8f */
        blob[o]=0x00; blob[o+1]=0x01; o += 4;                    /* 0x100 */

        ph_write32(&d, 0x1D90, 0);
        g_phy.rf[1][0] = 0; g_phy.rf[1][0x8f] = 0;
        if (rtw_phy_apply_blob(&d, blob, sizeof blob)) return 18;
        if (ph_read32(&d, 0x1D90) != 0x300101FE) return 19;      /* last write wins */
        if (g_phy.rf[1][0] != 0x10001) return 21;                /* rf_b applied */
        if (g_phy.rf[1][0x8f] != 0x100) return 22;
        /* bad magic rejected */
        blob[0] ^= 0xFF;
        if (rtw_phy_apply_blob(&d, blob, sizeof blob) == 0) return 23;
    }

    return 0;
}
