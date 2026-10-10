/* YartOS rtw88 port - frame I/O over the TX/RX descriptor rings.
 *
 * Transcribed from Linux rtw88 pci.c (rtw_pci_tx_write_data, rtw_pci_rx_napi,
 * rtw_pci_get_hw_rx_ring_nr).  Register offsets verified against pci.h:
 *   TX doorbell  RTW_PCI_TXBD_IDX_BEQ   = 0x3A8 (host writes its wp)
 *   RX index     RTW_PCI_RXBD_IDX_MPDUQ = 0x3B4 (chip wp in bits 27:16,
 *                host rp in bits 11:0)
 *
 * The selftest proves a full DMA round-trip with a fake chip that emulates
 * the silicon's side: it reads the host's TX slot from the ring, and injects
 * a reply by DMA-ing a frame into a posted RX buffer and advancing its wp.
 */
#include <yart/types.h>
#include <yart/string.h>
#include <yart/mm.h>
#include <yart/dma.h>
#include <yart/rtw88.h>
#include <yart/rtw_dma.h>
#include <yart/rtw_io.h>
#include "rtw8822c_regs.h"

int rtw_io_init(rtw_dev_t *d, rtw_io_t *io) {
    memset(io, 0, sizeof *io);
    if (rtw_ring_alloc(&io->tx_ring, RTW_TX_BUF_DESC_SZ, 256)) return -1;
    if (rtw_ring_alloc(&io->rx_ring, RTW_RX_BUF_DESC_SZ, RTW_MAX_RX_DESC_NUM)) return -1;

    /* pre-post the RX buffers */
    for (u32 i = 0; i < io->rx_ring.len; i++) {
        paddr_t phys = 0;
        void *v = dma_alloc32(RTW_PCI_RX_BUF_SIZE, &phys);
        if (!v) return -1;
        io->rx_buf[i] = v;
        io->rx_buf_phys[i] = (u32)phys;
        rtw_buf_desc_t *bd = (rtw_buf_desc_t *)(io->rx_ring.head + i * io->rx_ring.desc_size);
        rtw_rx_buf_desc_fill(bd, (u32)phys, RTW_PCI_RX_BUF_SIZE);
    }

    rtw_ring_setup(d, &io->tx_ring, &io->rx_ring);
    rtw_write16(d, RTW_PCI_RXBD_IDX_MPDUQ, 0);    /* host rp = 0 */
    return 0;
}

int rtw_io_tx(rtw_dev_t *d, rtw_io_t *io, const u8 *frame, u32 len) {
    if (!len || len > 8000) return -1;
    u32 idx = io->tx_wp % io->tx_ring.len;

    /* build [48-byte packet descriptor || frame] in DMA memory */
    paddr_t phys = 0;
    u8 *buf = dma_alloc32(RTW_TX_PKT_DESC_SZ + len, &phys);
    if (!buf) return -1;
    rtw_tx_pkt_desc_t desc;
    rtw_tx_desc_fill(&desc, len, 1 /* BE qsel */, 0 /* macid */, 0x0b /* 5.5M */, 0);
    memcpy(buf, &desc, RTW_TX_PKT_DESC_SZ);
    memcpy(buf + RTW_TX_PKT_DESC_SZ, frame, len);

    rtw_tx_slot_t *slot = (rtw_tx_slot_t *)(io->tx_ring.head + idx * io->tx_ring.desc_size);
    u16 psb_len = (u16)((len - 1) / 128 + 1);
    rtw_tx_slot_fill(slot, (u32)phys, (u16)len, psb_len);

    io->tx_wp++;
    rtw_write16(d, RTW_PCI_TXBD_IDX_BEQ, (u16)(io->tx_wp & RTW_TRX_BD_IDX_MASK));
    return 0;
}

int rtw_io_rx_poll(rtw_dev_t *d, rtw_io_t *io, u8 *frame, u32 cap, u32 *out_len) {
    /* chip write pointer lives in bits 27:16 of the RX index register */
    u32 reg = rtw_read32(d, RTW_PCI_RXBD_IDX_MPDUQ);
    u32 chip_wp = (reg & RTW_TRX_BD_HW_IDX_MASK) >> 16;
    if (chip_wp == io->rx_rp) return 0;

    u32 idx = io->rx_rp % io->rx_ring.len;
    u8 *buf = io->rx_buf[idx];
    const rtw_rx_desc_t *rd = (const rtw_rx_desc_t *)buf;
    u16 plen = rtw_rx_desc_parse(rd, NULL, NULL, NULL);
    if (plen > cap) return -1;

    /* payload = rx_desc (24) + drv_info (0) + shift (0) */
    memcpy(frame, buf + 24, plen);
    if (out_len) *out_len = plen;

    /* refill the descriptor for reuse and advance the host read pointer */
    rtw_buf_desc_t *bd = (rtw_buf_desc_t *)(io->rx_ring.head + idx * io->rx_ring.desc_size);
    rtw_rx_buf_desc_fill(bd, io->rx_buf_phys[idx], RTW_PCI_RX_BUF_SIZE);
    io->rx_rp = (io->rx_rp + 1) % io->rx_ring.len;
    rtw_write16(d, RTW_PCI_RXBD_IDX_MPDUQ, (u16)(io->rx_rp & RTW_TRX_BD_IDX_MASK));
    return 1;
}

/* ===================== selftest: fake chip =====================
 * The fake chip shares the host's DMA memory (dma_alloc32) so it can read
 * the TX slot and write into RX buffers exactly like the silicon would. */
typedef struct {
    u8  regs[0x400];
    u32 tx_wp_seen;      /* the doorbell value the chip last observed */
    u32 chip_rp;         /* chip's TX read pointer */
    u32 chip_wp;         /* chip's RX write pointer */
} io_chip_t;

static io_chip_t g_iochip;
static rtw_io_t   g_iohost;

/* fake MMIO register file: 0x300-0x3ff window */
static u32 io_reg_read32(rtw_dev_t *d, u32 a) {
    (void)d;
    if (a >= 0x300 && a < 0x400) {
        u32 o = (a - 0x300) >> 2;
        return ((u32 *)g_iochip.regs)[o];
    }
    return 0;
}
static u16 io_reg_read16(rtw_dev_t *d, u32 a) {
    u32 v = io_reg_read32(d, a & ~3u);
    return (u16)((v >> ((a & 3u) * 8)) & 0xFFFF);
}
static u8  io_reg_read8(rtw_dev_t *d, u32 a) { return (u8)io_reg_read16(d, a); }
static void io_reg_write32(rtw_dev_t *d, u32 a, u32 v) {
    (void)d;
    if (a >= 0x300 && a < 0x400) ((u32 *)g_iochip.regs)[(a - 0x300) >> 2] = v;
}
static void io_reg_write16(rtw_dev_t *d, u32 a, u16 v) { (void)d;
    u32 o = (a - 0x300) >> 2, sh = (a & 3u) * 8;
    u32 m = 0xFFFFu << sh;
    ((u32 *)g_iochip.regs)[o] = (((u32 *)g_iochip.regs)[o] & ~m) | (((u32)v << sh) & m);
    if (a == RTW_PCI_TXBD_IDX_BEQ) g_iochip.tx_wp_seen = v;   /* doorbell */
}
static void io_reg_write8(rtw_dev_t *d, u32 a, u8 v) { io_reg_write16(d, a & ~1u, v); }
static void io_sleep(rtw_dev_t *d, u32 ms) { (void)d; (void)ms; }

static const rtw_hci_ops_t io_ops = {
    .read8 = io_reg_read8, .read16 = io_reg_read16, .read32 = io_reg_read32,
    .write8 = io_reg_write8, .write16 = io_reg_write16, .write32 = io_reg_write32,
    .sleep_ms = io_sleep,
};

/* chip side: pull the next TX frame from the ring */
static int chip_read_tx(rtw_dev_t *d, u8 *frame, u32 cap, u32 *out_len) {
    (void)d;
    if (g_iochip.chip_rp >= g_iochip.tx_wp_seen) return 0;
    u32 idx = g_iochip.chip_rp % g_iohost.tx_ring.len;
    rtw_tx_slot_t *slot = (rtw_tx_slot_t *)(g_iohost.tx_ring.head + idx * g_iohost.tx_ring.desc_size);
    u16 frame_len = slot->d1.buf_size;
    u32 dma = slot->d1.dma;
    if (frame_len > cap) return -1;
    /* the chip DMAs [desc||frame] from `dma` */
    memcpy(frame, (u8 *)phys_to_virt((paddr_t)dma), frame_len);
    g_iochip.chip_rp++;
    if (out_len) *out_len = frame_len;
    return 1;
}

/* chip side: inject a frame into the host's RX ring */
static void chip_inject_rx(rtw_dev_t *d, const u8 *frame, u32 len) {
    u32 idx = g_iochip.chip_wp % g_iohost.rx_ring.len;
    u8 *buf = g_iohost.rx_buf[idx];
    rtw_rx_desc_t *rd = (rtw_rx_desc_t *)buf;
    memset(rd, 0, sizeof *rd);
    rd->w0 = len & RTW_RX_W0_PKT_LEN;
    memcpy(buf + 24, frame, len);
    rtw_buf_desc_t *bd = (rtw_buf_desc_t *)(g_iohost.rx_ring.head + idx * g_iohost.rx_ring.desc_size);
    rtw_rx_buf_desc_fill(bd, g_iohost.rx_buf_phys[idx], RTW_PCI_RX_BUF_SIZE);
    bd->psb_len = (u16)len;                     /* total_pkt_size */
    g_iochip.chip_wp = (g_iochip.chip_wp + 1) % g_iohost.rx_ring.len;
    /* chip writes its wp into bits 27:16 of the RX index register */
    u32 reg = io_reg_read32(d, RTW_PCI_RXBD_IDX_MPDUQ) & 0xFFFF;
    reg |= (g_iochip.chip_wp << 16) & RTW_TRX_BD_HW_IDX_MASK;
    io_reg_write32(d, RTW_PCI_RXBD_IDX_MPDUQ, reg);
}

int rtw_io_selftest(void) {
    memset(&g_iochip, 0, sizeof g_iochip);
    memset(&g_iohost, 0, sizeof g_iohost);
    rtw_dev_t d;
    memset(&d, 0, sizeof d);
    d.ops = &io_ops;

    if (rtw_io_init(&d, &g_iohost)) return 1;

    /* 1. host sends a frame; the chip must receive it byte-exact */
    static const u8 tx_frame[64] = {
        0x40,0x00, 0x00,0x00,
        0x02,0x11,0x22,0x33,0x44,0x55, 0x02,0x66,0x77,0x88,0x99,0xaa,
        0x02,0x11,0x22,0x33,0x44,0x55, 0x00,0x00,
        0xde,0xad,0xbe,0xef, 0xde,0xad,0xbe,0xef, 0xde,0xad,0xbe,0xef,
        0xde,0xad,0xbe,0xef, 0xde,0xad,0xbe,0xef, 0xde,0xad,0xbe,0xef,
        0xde,0xad,0xbe,0xef, 0xde,0xad,0xbe,0xef, 0xde,0xad,0xbe,0xef,
        0xde,0xad,0xbe,0xef };
    if (rtw_io_tx(&d, &g_iohost, tx_frame, sizeof tx_frame)) return 2;

    u8 got[256]; u32 got_len = 0;
    if (chip_read_tx(&d, got, sizeof got, &got_len) != 1) return 3;
    if (got_len != sizeof tx_frame || memcmp(got, tx_frame, sizeof tx_frame)) return 4;

    /* 2. chip injects a reply; the host must receive it byte-exact */
    static const u8 rx_frame[48] = {
        0x50,0x00, 0x00,0x00,
        0x02,0x66,0x77,0x88,0x99,0xaa, 0x02,0x11,0x22,0x33,0x44,0x55,
        0x02,0x11,0x22,0x33,0x44,0x55, 0x00,0x00,
        0xca,0xfe,0xca,0xfe, 0xca,0xfe,0xca,0xfe, 0xca,0xfe,0xca,0xfe,
        0xca,0xfe,0xca,0xfe, 0xca,0xfe,0xca,0xfe, 0xca,0xfe,0xca,0xfe };
    chip_inject_rx(&d, rx_frame, sizeof rx_frame);

    u8 rx[256]; u32 rx_len = 0;
    int r = rtw_io_rx_poll(&d, &g_iohost, rx, sizeof rx, &rx_len);
    if (r != 1) return 5;
    if (rx_len != sizeof rx_frame || memcmp(rx, rx_frame, sizeof rx_frame)) return 6;

    /* 3. nothing pending -> 0 */
    if (rtw_io_rx_poll(&d, &g_iohost, rx, sizeof rx, &rx_len) != 0) return 7;

    /* 4. multi-frame RX: 3 frames in order */
    for (int i = 0; i < 3; i++) {
        u8 f[32];
        memset(f, 0xa0 + i, sizeof f);
        chip_inject_rx(&d, f, sizeof f);
    }
    for (int i = 0; i < 3; i++) {
        u8 f[32];
        if (rtw_io_rx_poll(&d, &g_iohost, f, sizeof f, &rx_len) != 1) return 8;
        if (rx_len != 32 || f[0] != 0xa0 + i) return 9;
    }
    if (rtw_io_rx_poll(&d, &g_iohost, rx, sizeof rx, &rx_len) != 0) return 10;

    return 0;
}
