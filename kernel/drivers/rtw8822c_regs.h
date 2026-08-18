#pragma once
/* RTL8822CE register map — transcribed from Linux rtw88 (reg.h), verified
 * against drivers/net/wireless/realtek/rtw88 @ mainline.  Offsets and bit
 * positions must match the silicon EXACTLY; do not renumber. */

/* ---- power / platform ---- */
#define RTW_REG_SYS_FUNC_EN    0x0002    /* 16-bit: analog/mac/dig enable */
#define RTW_REG_RSV_CTRL       0x001C
#define RTW_REG_EFUSE_CTRL     0x0030
#define RTW_REG_LDO_EFUSE_CTRL 0x0034
#define RTW_REG_CR             0x0100    /* command register */
#define RTW_REG_SYS_CFG1       0x00F0    /* chip version in bits 15:12 */
#define RTW_SHIFT_CHIP_VER     12
#define RTW_MASK_CHIP_VER      0xF

/* EFUSE control word bits (verified against reg.h) */
#define RTW_BIT_EF_FLAG        0x80000000u   /* BIT(31): read done */
#define RTW_SHIFT_EF_ADDR      8
#define RTW_MASK_EF_ADDR       0x3ff
#define RTW_MASK_EF_DATA       0xff

/* EFUSE geometry (rtw8822c.c hw spec) */
#define RTW_EFUSE_PHYS_SIZE    512
#define RTW_EFUSE_LOG_SIZE     768
#define RTW_EFUSE_PROTECT_SIZE 124
#define RTW_EFUSE_MAC_OFFSET   0x120         /* RTL8822CE: union at 0x120 */

/* ---- PCI DMA ring registers (pci.h, verified) ---- */
#define RTW_PCI_CTRL                0x300
#define RTW_PCI_TXBD_DESA_BCNQ      0x308
#define RTW_PCI_TXBD_DESA_MGMTQ     0x310
#define RTW_PCI_TXBD_DESA_VOQ       0x318
#define RTW_PCI_TXBD_DESA_VIQ       0x320
#define RTW_PCI_TXBD_DESA_BEQ       0x328
#define RTW_PCI_TXBD_DESA_BKQ       0x330
#define RTW_PCI_RXBD_DESA_MPDUQ     0x338
#define RTW_PCI_TXBD_DESA_HI0Q      0x340
#define RTW_PCI_TXBD_NUM_MGMTQ      0x380
#define RTW_PCI_RXBD_NUM_MPDUQ      0x382
#define RTW_PCI_TXBD_NUM_VOQ        0x384
#define RTW_PCI_TXBD_NUM_VIQ        0x386
#define RTW_PCI_TXBD_NUM_BEQ        0x388
#define RTW_PCI_TXBD_NUM_BKQ        0x38A
#define RTW_PCI_TXBD_NUM_HI0Q       0x38C
#define RTW_PCI_TXBD_IDX_BEQ        0x3A8    /* TX doorbell: host writes wp */
#define RTW_PCI_RXBD_IDX_MPDUQ      0x3B4    /* RX index: chip wp (27:16), host rp */
#define RTW_TRX_BD_HW_IDX_MASK      0x0FFF0000u  /* GENMASK(27,16) */

#define RTW_TRX_BD_IDX_MASK         0xFFF
#define RTW_MAX_RX_DESC_NUM         512
#define RTW_PCI_RX_BUF_SIZE         11478   /* 11454 + 24 */
#define RTW_TXBD_OWN_OFFSET         15

/* 8822c descriptor sizes (rtw8822c.c hw spec) */
#define RTW_TX_PKT_DESC_SZ          48
#define RTW_TX_BUF_DESC_SZ          16
#define RTW_RX_BUF_DESC_SZ          8

/* SYS_FUNC_EN byte 1 (offset 0x03) bits */
#define RTW_BIT_FEN_CPUEN      0x04      /* BIT(2) */
#define RTW_BIT_FEN_BB_GLB_RST 0x02      /* BIT(1) */
#define RTW_BIT_FEN_BB_RSTB    0x01      /* BIT(0) */

/* RSV_CTRL byte 1 (offset 0x1D) bits */
#define RTW_BIT_WLMCU_IOIF     0x01      /* BIT(0) */

/* ---- MCU firmware control (the firmware-download handshake) ---- */
#define RTW_REG_MCUFW_CTRL     0x0080
#define RTW_REG_MCU_TST_CFG    0x0084

#define RTW_BIT_MCUFWDL_EN     0x01      /* BIT(0): enable firmware download */
#define RTW_BIT_MCUFWDL_RDY    0x02      /* BIT(1): MCU ready for download  */
#define RTW_BIT_FWDL_CHK_RPT   0x04      /* BIT(2): firmware-download checksum report */
#define RTW_BIT_WINTINI_RDY    0x40      /* BIT(6): wlan init ready         */
#define RTW_BIT_RAM_DL_SEL     0x80      /* BIT(7): RAM download select     */
#define RTW_BIT_ROM_DLEN       (1u << 19)
#define RTW_BITS_ROM_PGE       (0x7u << 16)   /* GENMASK(18,16) page select */
#define RTW_SHIFT_ROM_PGE      16

/* The 8822C boots via the legacy 8051 path: after download, all four bits
 * must be set before the firmware is running. */
#define RTW_FW_READY_LEGACY \
    (RTW_BIT_MCUFWDL_RDY | RTW_BIT_FWDL_CHK_RPT | RTW_BIT_WINTINI_RDY | RTW_BIT_RAM_DL_SEL)

/* ---- firmware download geometry (legacy path) ---- */
#define RTW_FW_START_ADDR      0x1000    /* register window: firmware words */
#define RTW_DLFW_PAGE_SIZE     0x1000    /* 4096 bytes per page             */
#define RTW_DLFW_BLK_SIZE      4         /* 32-bit writes                   */
#define RTW_FW_HDR_SIZE        32        /* sizeof(struct rtw_fw_hdr)       */
