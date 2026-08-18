#pragma once
#include <yart/types.h>
#include <yart/rtw88.h>

/* YartOS rtw88 port - DMA descriptor rings.
 *
 * Transcribed from Linux rtw88 (tx.h / rx.h / pci.h / pci.c).  The 8822C
 * talks to the host through descriptor rings in 32-bit DMA memory:
 *
 *  - TX: the host DMA-maps a buffer holding [48-byte TX packet descriptor ||
 *    802.11 frame], then writes a 16-byte "buffer descriptor" {buf_size,
 *    psb_len, dma} into the TX ring and bumps the write pointer.
 *  - RX: the host pre-fills the RX ring with 8-byte buffer descriptors
 *    {buf_size, total_pkt_size, dma} pointing at 11478-byte RX buffers; the
 *    chip DMAs received frames in and writes a 24-byte RX descriptor at the
 *    front of each buffer.
 *
 * Ring base addresses + lengths are handed to the chip by writing
 * TXBD_DESA_x / TXBD_NUM_x / RXBD_DESA_MPDUQ / RXBD_NUM_MPDUQ registers.
 *
 * TX/RX descriptor bitfields come from tx.h/rx.h (verified); the field
 * helpers mirror le32_encode_bits.
 */

/* ---- TX packet descriptor (48 bytes for the 8822C) ---- */
typedef struct PACKED {
    u32 w0, w1, w2, w3, w4, w5;
    u32 w6, w7, w8, w9, w10, w11;
} rtw_tx_pkt_desc_t;

#define RTW_TX_W0_TXPKTSIZE   0x0000FFFFu      /* GENMASK(15,0) */
#define RTW_TX_W0_OFFSET      0x00FF0000u      /* GENMASK(23,16) */
#define RTW_TX_W0_BMC         0x01000000u      /* BIT(24) */
#define RTW_TX_W0_LS          0x04000000u      /* BIT(26) */
#define RTW_TX_W0_DISQSELSEQ  0x80000000u      /* BIT(31) */
#define RTW_TX_W1_MACID       0x000000FFu      /* GENMASK(7,0) */
#define RTW_TX_W1_QSEL        0x00001F00u      /* GENMASK(12,8) */
#define RTW_TX_W1_RATE_ID     0x001F0000u      /* GENMASK(20,16) */
#define RTW_TX_W1_SEC_TYPE    0x00C00000u      /* GENMASK(23,22) */
#define RTW_TX_W1_PKT_OFFSET  0x1F000000u      /* GENMASK(28,24) */
#define RTW_TX_W1_MORE_DATA   0x20000000u      /* BIT(29) */
#define RTW_TX_W4_DATARATE    0x0000007Fu      /* GENMASK(6,0) */
#define RTW_TX_W7_CHECKSUM    0x0000FFFFu      /* GENMASK(15,0) */

/* ---- 8-byte buffer descriptor (shared TX/RX ring entry) ----
 * Verified against rtw_pci_tx_buffer_desc / rtw_pci_rx_buffer_desc:
 * {le16 buf_size, le16 psb_len|total_pkt_size, le32 dma}. */
typedef struct PACKED {
    u16 buf_size;       /* LE */
    u16 psb_len;        /* LE: TX = psb_len, RX = total_pkt_size */
    u32 dma;            /* LE */
} rtw_buf_desc_t;

/* ---- TX ring slot = TWO 8-byte descriptors (16 bytes) ----
 * Verified against rtw_pci_tx_write_data: buf_desc[0] describes the 48-byte
 * packet descriptor, buf_desc[1] describes the frame data. */
typedef struct PACKED {
    rtw_buf_desc_t d0;  /* packet descriptor: buf_size = tx_pkt_desc_sz */
    rtw_buf_desc_t d1;  /* data: buf_size = frame size */
} rtw_tx_slot_t;

/* ---- RX descriptor (24 bytes, chip-written, at the front of the buffer) ---- */
typedef struct PACKED {
    u32 w0, w1, w2, w3, w4, w5;
} rtw_rx_desc_t;

#define RTW_RX_W0_PKT_LEN      0x00003FFFu     /* GENMASK(13,0) */
#define RTW_RX_W0_CRC32        0x00004000u     /* BIT(14) */
#define RTW_RX_W0_ICV_ERR      0x00008000u     /* BIT(15) */
#define RTW_RX_W0_DRV_INFO_SZ  0x000F0000u     /* GENMASK(19,16) */
#define RTW_RX_W0_ENC_TYPE     0x00700000u     /* GENMASK(22,20) */
#define RTW_RX_W0_SHIFT        0x03000000u     /* GENMASK(25,24) */
#define RTW_RX_W0_PHYST        0x04000000u     /* BIT(26) */
#define RTW_RX_W0_SWDEC        0x08000000u     /* BIT(27) */
#define RTW_RX_W1_MACID        0x0000007Fu     /* GENMASK(6,0) */
#define RTW_RX_W3_RX_RATE      0x0000007Fu     /* GENMASK(6,0) */
#define RTW_RX_W4_BW           0x00000030u     /* GENMASK(5,4) */

/* ---- DMA geometry (shared by the dma/io layers) ---- */
#define RTW_MAX_RX_DESC_NUM     512
#define RTW_PCI_RX_BUF_SIZE     11478   /* 11454 + 24 */
#define RTW_TX_PKT_DESC_SZ      48
#define RTW_TX_BUF_DESC_SZ      16
#define RTW_RX_BUF_DESC_SZ      8

/* ---- generic ring ---- */
typedef struct {
    u8  *head;          /* HHDM mapping of the DMA memory */
    u32  dma;           /* physical address (32-bit DMA) */
    u8   desc_size;
    u32  len;
    u32  wp, rp;
} rtw_ring_t;

/* ---- field helpers (mirror le32_encode_bits: shift by __ffs(mask)) ---- */
static inline u32 rtw_enc(u32 val, u32 mask) {
    u32 shift = 0;
    while (!(mask & 1)) { mask >>= 1; shift++; }
    return (val << shift);
}

/* ---- entry points ---- */

/* Allocate a 32-bit DMA ring (contiguous, zeroed).  0 = ok. */
int  rtw_ring_alloc(rtw_ring_t *ring, u8 desc_size, u32 len);

/* Write the ring base addresses + lengths to the chip's registers.
 * rings[0..5] = BE/BK/VI/VO/MGMT/HI0 TX rings (in that order); rx = RX ring.
 * H2C/BCN queues are not used in the first bring-up. */
void rtw_ring_setup(rtw_dev_t *d, const rtw_ring_t *tx_be,
                    const rtw_ring_t *rx);

/* Fill a TX packet descriptor (w0/w1/w4) and its 16-bit XOR checksum (w7). */
void rtw_tx_desc_fill(rtw_tx_pkt_desc_t *desc, u32 pkt_size, u8 qsel,
                      u8 macid, u8 rate, u32 offset);

/* Fill a TX buffer-descriptor ring entry for a buffer at `dma`. */
void rtw_tx_slot_fill(rtw_tx_slot_t *slot, u32 dma, u16 frame_size, u16 psb_len);

/* Fill an RX buffer-descriptor ring entry (host pre-fill). */
void rtw_rx_buf_desc_fill(rtw_buf_desc_t *bd, u32 dma, u16 buf_size);

/* Parse the chip-written RX descriptor; returns the frame length. */
u16 rtw_rx_desc_parse(const rtw_rx_desc_t *desc, u8 *macid, u8 *rate,
                      bool *icv_err);

int rtw_dma_selftest(void);   /* 0 = ok */
