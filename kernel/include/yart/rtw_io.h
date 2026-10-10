#pragma once
#include <yart/types.h>
#include <yart/rtw88.h>
#include <yart/rtw_dma.h>

/* YartOS rtw88 port - frame I/O over the TX/RX descriptor rings.
 *
 * This is the layer that actually moves 802.11 frames between the host and
 * the chip, transcribed from Linux rtw88 pci.c:
 *  - TX: build [48-byte packet descriptor || frame] in 32-bit DMA memory,
 *    write the 16-byte TX slot {desc, data} into the BEQ ring, bump the
 *    write pointer and ring the doorbell (RTK_PCI_TXBD_IDX_BEQ = 0x3A8).
 *  - RX: the chip DMAs frames into pre-posted buffers and writes its write
 *    pointer into RTK_PCI_RXBD_IDX_MPDUQ (0x3B4) bits 27:16; the host polls
 *    it, reads each frame (24-byte RX descriptor + payload), refills the
 *    buffer descriptor and writes its read pointer back.
 */

typedef struct {
    rtw_ring_t tx_ring;          /* BEQ */
    rtw_ring_t rx_ring;          /* MPDUQ */
    u32  tx_wp, tx_rp;
    u32  rx_rp;
    u8  *rx_buf[RTW_MAX_RX_DESC_NUM];   /* HHDM views of the RX buffers */
    u32  rx_buf_phys[RTW_MAX_RX_DESC_NUM];
} rtw_io_t;

/* Allocate the rings + RX buffers and hand everything to the chip. */
int  rtw_io_init(rtw_dev_t *d, rtw_io_t *io);

/* Queue one 802.11 frame for transmission (0 = ok). */
int  rtw_io_tx(rtw_dev_t *d, rtw_io_t *io, const u8 *frame, u32 len);

/* Poll the RX ring: copy one frame out if ready (1 = frame, 0 = none,
 * -1 = error).  The frame is the raw 802.11 payload (RX descriptor
 * stripped). */
int  rtw_io_rx_poll(rtw_dev_t *d, rtw_io_t *io, u8 *frame, u32 cap, u32 *out_len);

int rtw_io_selftest(void);   /* 0 = ok */
