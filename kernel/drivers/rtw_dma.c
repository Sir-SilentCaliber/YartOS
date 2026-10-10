/* YartOS rtw88 port - DMA descriptor rings (TX/RX).
 *
 * Transcribed from Linux rtw88 (tx.h, rx.h, pci.h, pci.c, tx.c).  The 8822C
 * exchanges frames with the host over descriptor rings in 32-bit DMA memory:
 *
 *  - TX ring entries are 16-byte buffer descriptors {buf_size, psb_len, dma};
 *    the DMA address points at [48-byte TX packet descriptor || 802.11 frame].
 *    The TX packet descriptor carries pkt size (w0), qsel/macid (w1), rate
 *    (w4) and a 16-bit XOR checksum over the descriptor (w7).
 *  - RX ring entries are 8-byte buffer descriptors {buf_size, total_pkt_size,
 *    dma}; the chip DMAs frames into 11478-byte buffers and writes a 24-byte
 *    RX descriptor at the front (pkt len in w0[13:0], macid in w1[6:0], rate
 *    in w3[6:0]).
 *  - Ring base addresses + lengths are handed to the chip by writing the
 *    TXBD_DESA_x / TXBD_NUM_x / RXBD_DESA_MPDUQ / RXBD_NUM_MPDUQ registers.
 *
 * The selftest runs ring allocation (real 32-bit DMA), register setup and
 * descriptor fill/parse against a fake chip register file, and validates the
 * checksum against an independent XOR recomputation.
 */
#include <yart/types.h>
#include <yart/string.h>
#include <yart/dma.h>
#include <yart/rtw88.h>
#include <yart/rtw_dma.h>
#include "rtw8822c_regs.h"

int rtw_ring_alloc(rtw_ring_t *ring, u8 desc_size, u32 len) {
    size_t sz = (size_t)desc_size * len;
    paddr_t phys = 0;
    void *v = dma_alloc32(sz, &phys);
    if (!v) return -1;
    ring->head = v;
    ring->dma = (u32)phys;
    ring->desc_size = desc_size;
    ring->len = len;
    ring->wp = ring->rp = 0;
    return 0;
}

void rtw_ring_setup(rtw_dev_t *d, const rtw_ring_t *tx_be, const rtw_ring_t *rx) {
    if (tx_be) {
        rtw_write32(d, RTW_PCI_TXBD_DESA_BEQ, tx_be->dma);
        rtw_write16(d, RTW_PCI_TXBD_NUM_BEQ, (u16)(tx_be->len & RTW_TRX_BD_IDX_MASK));
    }
    if (rx) {
        rtw_write32(d, RTW_PCI_RXBD_DESA_MPDUQ, rx->dma);
        rtw_write16(d, RTW_PCI_RXBD_NUM_MPDUQ, (u16)(rx->len & RTW_TRX_BD_IDX_MASK));
    }
}

void rtw_tx_desc_fill(rtw_tx_pkt_desc_t *desc, u32 pkt_size, u8 qsel,
                      u8 macid, u8 rate, u32 offset) {
    memset(desc, 0, sizeof *desc);
    desc->w0 = (pkt_size & 0xFFFF) | ((offset << 16) & RTW_TX_W0_OFFSET);
    desc->w1 = (macid & 0xFF)
             | rtw_enc(qsel, RTW_TX_W1_QSEL)
             | rtw_enc(0, RTW_TX_W1_RATE_ID)
             | rtw_enc(0, RTW_TX_W1_SEC_TYPE)
             | rtw_enc(0, RTW_TX_W1_PKT_OFFSET);
    desc->w4 = (rate & 0x7F);

    /* 16-bit XOR checksum over the whole 48-byte descriptor (w7 field
     * zeroed first), stored into w7[15:0] (verified against
     * rtw_tx_fill_txdesc_checksum). */
    desc->w7 &= ~RTW_TX_W7_CHECKSUM;
    u16 chksum = 0;
    u16 *p = (u16 *)desc;
    u32 words = (offset * 8 + RTW_TX_PKT_DESC_SZ) / 2;
    while (words--) chksum ^= *p++;
    desc->w7 |= (u32)chksum;
}

void rtw_tx_slot_fill(rtw_tx_slot_t *slot, u32 dma, u16 frame_size, u16 psb_len) {
    memset(slot, 0, sizeof *slot);
    slot->d0.buf_size = RTW_TX_PKT_DESC_SZ;      /* the packet descriptor */
    slot->d0.psb_len  = psb_len;                 /* LE (host is LE) */
    slot->d0.dma      = dma;
    slot->d1.buf_size = frame_size;              /* the frame data */
    slot->d1.psb_len  = 0;
    slot->d1.dma      = dma + RTW_TX_PKT_DESC_SZ;
}

void rtw_rx_buf_desc_fill(rtw_buf_desc_t *bd, u32 dma, u16 buf_size) {
    bd->buf_size = buf_size;
    bd->psb_len = 0;
    bd->dma = dma;
}

u16 rtw_rx_desc_parse(const rtw_rx_desc_t *desc, u8 *macid, u8 *rate, bool *icv_err) {
    if (macid)   *macid   = (u8)(desc->w1 & RTW_RX_W1_MACID);
    if (rate)    *rate    = (u8)(desc->w3 & RTW_RX_W3_RX_RATE);
    if (icv_err) *icv_err = (desc->w0 & RTW_RX_W0_ICV_ERR) != 0;
    return (u16)(desc->w0 & RTW_RX_W0_PKT_LEN);
}

/* ===================== selftest: fake chip register file ===================== */
#define FAKE_REGS 256
static u32 g_regs[FAKE_REGS];
static u32 fake_read32(rtw_dev_t *d, u32 a) {
    (void)d;
    if (a >= 0x300 && a < 0x400) return g_regs[(a - 0x300) >> 2];
    return 0;
}
static u16 fake_read16(rtw_dev_t *d, u32 a) { return (u16)fake_read32(d, a & ~3u); }
static u8  fake_read8 (rtw_dev_t *d, u32 a) { return (u8)fake_read16(d, a); }
static void fake_write32(rtw_dev_t *d, u32 a, u32 v) {
    (void)d;
    if (a >= 0x300 && a < 0x400) g_regs[(a - 0x300) >> 2] = v;
}
static void fake_write16(rtw_dev_t *d, u32 a, u16 v) {
    (void)d;
    u32 o = a & ~3u, shift = (a & 3u) * 8;
    u32 m = 0xFFFFu << shift;
    u32 cur = (a >= 0x300 && a < 0x400) ? g_regs[(o - 0x300) >> 2] : 0;
    if (a >= 0x300 && a < 0x400) g_regs[(o - 0x300) >> 2] = (cur & ~m) | (((u32)v << shift) & m);
}
static void fake_write8(rtw_dev_t *d, u32 a, u8 v) {
    (void)d;
    u32 o = a & ~3u, shift = (a & 3u) * 8;
    u32 m = 0xFFu << shift;
    u32 cur = (a >= 0x300 && a < 0x400) ? g_regs[(o - 0x300) >> 2] : 0;
    if (a >= 0x300 && a < 0x400) g_regs[(o - 0x300) >> 2] = (cur & ~m) | (((u32)v << shift) & m);
}
static void fake_sleep(rtw_dev_t *d, u32 ms) { (void)d; (void)ms; }

static const rtw_hci_ops_t dma_ops = {
    .read8 = fake_read8, .read16 = fake_read16, .read32 = fake_read32,
    .write8 = fake_write8, .write16 = fake_write16, .write32 = fake_write32,
    .sleep_ms = fake_sleep,
};

int rtw_dma_selftest(void) {
    memset(g_regs, 0, sizeof g_regs);
    rtw_dev_t d;
    memset(&d, 0, sizeof d);
    d.ops = &dma_ops;

    /* 1. ring allocation (real 32-bit DMA) */
    rtw_ring_t tx, rx;
    if (rtw_ring_alloc(&tx, RTW_TX_BUF_DESC_SZ, 256)) return 1;
    if (tx.dma & 0xF) return 2;                  /* 16-byte aligned */
    if (rtw_ring_alloc(&rx, RTW_RX_BUF_DESC_SZ, RTW_MAX_RX_DESC_NUM)) return 4;

    /* 2. register setup: the chip must see the DMA addresses + lengths */
    rtw_ring_setup(&d, &tx, &rx);
    if (g_regs[(RTW_PCI_TXBD_DESA_BEQ - 0x300) >> 2] != tx.dma) return 5;
    if (g_regs[(RTW_PCI_RXBD_DESA_MPDUQ - 0x300) >> 2] != rx.dma) return 6;
    if ((g_regs[(RTW_PCI_TXBD_NUM_BEQ - 0x300) >> 2] & 0xFFFF) != 256) return 7;
    /* RXBD_NUM_MPDUQ = 0x382 -> high 16 bits of the 0x380 word */
    if (((g_regs[(RTW_PCI_RXBD_NUM_MPDUQ - 0x300) >> 2] >> 16) & 0xFFFF) != RTW_MAX_RX_DESC_NUM) return 8;

    /* 3. TX packet descriptor fill + checksum */
    rtw_tx_pkt_desc_t desc;
    rtw_tx_desc_fill(&desc, 1200, 2, 1, 0x0b, 0);
    if ((desc.w0 & 0xFFFF) != 1200) return 9;          /* pkt size */
    if (((desc.w1 >> 8) & 0x1F) != 2) return 10;       /* qsel */
    if ((desc.w1 & 0xFF) != 1) return 11;              /* macid */
    if ((desc.w4 & 0x7F) != 0x0b) return 12;           /* datarate */
    {   /* independent XOR recomputation */
        u32 saved = desc.w7;
        desc.w7 &= ~RTW_TX_W7_CHECKSUM;
        u16 chk = 0, *p = (u16 *)&desc;
        for (u32 i = 0; i < RTW_TX_PKT_DESC_SZ / 2; i++) chk ^= p[i];
        if ((saved & 0xFFFF) != chk) return 13;
    }

    /* 4. TX buffer descriptor fill */
    rtw_tx_slot_t tbd;
    rtw_tx_slot_fill(&tbd, 0x12345000, 1200, 10);
    if (tbd.d0.dma != 0x12345000 || tbd.d0.buf_size != RTW_TX_PKT_DESC_SZ || tbd.d0.psb_len != 10) return 14;
    if (tbd.d1.dma != 0x12345000 + RTW_TX_PKT_DESC_SZ || tbd.d1.buf_size != 1200 || tbd.d1.psb_len != 0) return 15;

    /* 5. RX buffer descriptor fill */
    rtw_buf_desc_t rbd;
    rtw_rx_buf_desc_fill(&rbd, 0x56780000, RTW_PCI_RX_BUF_SIZE);
    if (rbd.dma != 0x56780000 || rbd.buf_size != RTW_PCI_RX_BUF_SIZE) return 16;

    /* 6. RX descriptor parse */
    rtw_rx_desc_t rd;
    memset(&rd, 0, sizeof rd);
    rd.w0 = 700 | RTW_RX_W0_ICV_ERR;
    rd.w1 = 3;                          /* macid */
    rd.w3 = 0x0b;                       /* rate 5.5 Mb/s */
    rd.w4 = 0x20;                       /* BW 40 MHz */
    u8 macid = 0, rate = 0; bool icv = false;
    u16 plen = rtw_rx_desc_parse(&rd, &macid, &rate, &icv);
    if (plen != 700 || macid != 3 || rate != 0x0b || !icv) return 17;

    /* 7. zero-length pkt + no flags */
    memset(&rd, 0, sizeof rd);
    rd.w0 = 0;
    if (rtw_rx_desc_parse(&rd, &macid, &rate, &icv) != 0 || icv) return 18;

    return 0;
}
