#pragma once
#include <yart/types.h>
#include <yart/rtw88.h>

/* YartOS rtw88 port - PHY init machinery (power sequence + register tables).
 *
 * Transcribed from Linux rtw88 (mac.c rtw_pwr_seq_parser, phy.c table loaders,
 * rtw8822c.c power sequences).  This is the code that brings the chip from
 * "firmware running" to "radio on":
 *   1. rtw_pwr_seq_parser() runs the card-enable power sequence (carddis ->
 *      cardemu -> act), a list of WRITE / POLLING / DELAY commands.
 *   2. rtw_phy_load_tables() applies the MAC / AGC / BB / RF register tables.
 *   3. The MAC/BB/RF config functions know the Realtek delay-addresses
 *      (0xf9..0xfe for BB, 0xfe/0xffe for RF).
 *
 * The table DATA (1.1 MB of {addr, data} pairs) is generated from the Linux
 * source by scripts/gen_rtw_tables.py - the machinery here is what must be
 * transcribed by hand and verified.
 */

/* ---- power sequence commands (mac.c) ---- */
#define RTW_PWR_CMD_WRITE    0x01
#define RTW_PWR_CMD_POLLING  0x02
#define RTW_PWR_CMD_DELAY    0x03
#define RTW_PWR_CMD_END      0x04
#define RTW_PWR_ADDR_MAC     0x00
#define RTW_PWR_INTF_PCI     0x04      /* BIT(2) */
#define RTW_PWR_INTF_ALL     0x0F
#define RTW_PWR_CUT_ALL      0xFF

typedef struct {
    u16 offset;
    u8  cut_mask;
    u8  intf_mask;
    u8  base;
    u8  cmd;
    u8  mask;
    u8  value;
} rtw_pwr_seq_cmd_t;

/* Run a NULL-terminated array of command sequences (0 = ok). */
int rtw_pwr_seq_parser(rtw_dev_t *d, const rtw_pwr_seq_cmd_t * const *seqs);

/* The RTL8822CE PCI power-on sequence (carddis->cardemu->act), transcribed
 * from rtw8822c.c with the USB/SDIO-only entries removed. */
extern const rtw_pwr_seq_cmd_t rtw8822ce_pwr_on_cardemu[];
extern const rtw_pwr_seq_cmd_t rtw8822ce_pwr_on_act[];

/* ---- RF register write (phy.c rtw_phy_write_rf_reg) ---- */
void rtw_write_rf(rtw_dev_t *d, int rf_path, u32 addr, u32 mask, u32 data);
#define RTW_RFREG_MASK    0xfffff
#define RTW_RF_PATH_A     0
#define RTW_RF_PATH_B     1
#define RTW_RF_BASE_A     0x3c00    /* rtw8822c rf_base_addr */
#define RTW_RF_BASE_B     0x4c00

/* ---- register tables (phy.c cfg functions) ---- */
typedef struct {
    u32 addr;
    u32 data;
} rtw_phy_cfg_pair_t;

/* Apply one {addr, data} pair the way the named cfg function would.
 * mac=write8, agc=write32, bb=write32+delays, rf=write_rf+delays. */
void rtw_phy_cfg_mac(rtw_dev_t *d, u32 addr, u32 data);
void rtw_phy_cfg_agc(rtw_dev_t *d, u32 addr, u32 data);
void rtw_phy_cfg_bb (rtw_dev_t *d, u32 addr, u32 data);
void rtw_phy_cfg_rf (rtw_dev_t *d, u32 addr, u32 data);

/* Apply a whole table of pairs with the given cfg function. */
void rtw_load_table(rtw_dev_t *d, const rtw_phy_cfg_pair_t *pairs, u32 n_pairs,
                    void (*cfg)(rtw_dev_t *, u32, u32));

/* Conditional table parser (phy.c rtw_parse_tbl_phy_cond).  `cond` holds the
 * chip's cut/pkg/plat/intf/rfe; entries whose addr word has bit31 (pos) are
 * IF/ELIF/ELSE/ENDIF directives, bit30 (neg) a negated condition. */
typedef struct {
    u8  rfe;   /* RFE option (0 = any) */
    u8  intf;  /* INTF_PCIE bit (0 = any) */
    u8  pkg;   /* package (0 = any) */
    u8  plat;  /* platform (0 = any) */
    u8  cut;   /* cut version (0 = any) */
} rtw_phy_cond_t;

void rtw_parse_tbl_phy_cond(rtw_dev_t *d, const rtw_phy_cfg_pair_t *pairs,
                            u32 n_pairs, rtw_phy_cond_t drv_cond,
                            void (*cfg)(rtw_dev_t *, u32, u32));

/* Kernel-side loader for the generated table blob (gen_rtw_tables.py). */
int  rtw_phy_load_tables(rtw_dev_t *d, const char *path);
int  rtw_phy_apply_blob(rtw_dev_t *d, const u8 *blob, u32 len);

int rtw_phy_selftest(void);   /* 0 = ok */
