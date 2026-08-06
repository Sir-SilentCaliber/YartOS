/* Yart OS - Intel e1000 (82540EM, QEMU default) NIC driver (row 16).
 *
 * Uses the legacy MMIO interface: PCI BAR0 is memory-mapped, TX/RX use the
 * classic 16-byte descriptor rings.  Frames are polled (the driver keeps an
 * internal RX FIFO; net_service drains it), which is robust under QEMU/TCG
 * and sidesteps IOAPIC IRQ routing for now.
 */
#include <yart/net.h>
#include <yart/mm.h>
#include <yart/io.h>
#include <yart/console.h>
#include <yart/string.h>
#include <yart/spinlock.h>
#include <yart/hal.h>   /* irq_register / apic_route_irq */

#define PCI_CFG_ADDR 0xCF8
#define PCI_CFG_DATA 0xCFC

static u32 pci_rd32(u8 bus, u8 dev, u8 fn, u8 off) {
    outl(PCI_CFG_ADDR, (1U << 31) | ((u32)bus << 16) | ((u32)dev << 11)
                       | ((u32)fn << 8) | (off & 0xFC));
    return inl(PCI_CFG_DATA);
}
static void pci_wr32(u8 bus, u8 dev, u8 fn, u8 off, u32 val) {
    outl(PCI_CFG_ADDR, (1U << 31) | ((u32)bus << 16) | ((u32)dev << 11)
                       | ((u32)fn << 8) | (off & 0xFC));
    outl(PCI_CFG_DATA, val);
}
static void pci_wr16(u8 bus, u8 dev, u8 fn, u8 off, u16 val) {
    u32 a = (1U << 31) | ((u32)bus << 16) | ((u32)dev << 11)
            | ((u32)fn << 8) | (off & 0xFC);
    outl(PCI_CFG_ADDR, a);
    outw(PCI_CFG_DATA, val);
}
static void pci_wr8(u8 bus, u8 dev, u8 fn, u8 off, u8 val) {
    u32 a = (1U << 31) | ((u32)bus << 16) | ((u32)dev << 11)
            | ((u32)fn << 8) | (off & 0xFC);
    outl(PCI_CFG_ADDR, a);
    outb(PCI_CFG_DATA, val);
}

/* ---- e1000 registers (offset >> 2) ---- */
#define REG_CTRL     (0x0000 >> 2)
#define REG_STATUS   (0x0008 >> 2)
#define REG_ICR      (0x00C0 >> 2)
#define REG_IMS      (0x00D0 >> 2)
#define REG_IMC      (0x00D8 >> 2)
#define REG_RCTL     (0x0100 >> 2)
#define REG_TCTL     (0x0400 >> 2)
#define REG_TIPG     (0x0410 >> 2)
#define REG_RDBAL    (0x2800 >> 2)
#define REG_RDBAH    (0x2804 >> 2)
#define REG_RDLEN    (0x2808 >> 2)
#define REG_RDH      (0x2810 >> 2)
#define REG_RDT      (0x2818 >> 2)
#define REG_TDBAL    (0x3800 >> 2)
#define REG_TDBAH    (0x3804 >> 2)
#define REG_TDLEN    (0x3808 >> 2)
#define REG_TDH      (0x3810 >> 2)
#define REG_TDT      (0x3818 >> 2)
#define REG_RAL      (0x5400 >> 2)
#define REG_RAH      (0x5404 >> 2)
#define REG_MTA      (0x5200 >> 2)

/* Note: in the e1000, E1000_RCTL_EN and E1000_TCTL_EN are BOTH bit 1 (0x2),
 * not bit 0.  SBP is bit 2.  Getting these wrong silently leaves the
 * transmitter/receiver disabled. */
#define CTRL_RST   (1u << 26)
#define CTRL_SLU   (1u << 6)
#define RCTL_EN    (1u << 1)
#define RCTL_SBP   (1u << 2)
#define RCTL_UPE   (1u << 3)   /* unicast promiscuous (off)   */
#define RCTL_MPE   (1u << 4)   /* multicast promiscuous: accept ALL mcast -
                                  needed for IPv6 NDP (33:33:.. frames); the
                                  MTA is zeroed at reset, so without MPE every
                                  ICMPv6/NDP frame is silently dropped by the
                                  NIC's multicast filter */
#define RCTL_BAM   (1u << 15)
#define RCTL_SECRC (1u << 26)
#define RCTL_BSIZE_2048 (0u << 16)
#define TCTL_EN    (1u << 1)
#define TCTL_PSP   (1u << 3)
#define TCTL_CT    (0x0Fu << 4)
#define TCTL_COLD  (0x40u << 12)

/* legacy descriptors (16 bytes) */
typedef struct {
    u64 addr;
    u16 length;
    u16 checksum;
    u8  status;
    u8  errors;
    u16 special;
} e1000_rx_desc;
typedef struct {
    u64 addr;
    u16 length;
    u8  cso;
    u8  cmd;        /* MUST be u8 at offset 11; a u16 here shifts cmd/status */
    u8  status;
    u8  css;
    u16 special;
} e1000_tx_desc;

#define RX_DD (1u << 0)
#define RX_EOP (1u << 1)
#define TX_CMD_EOP (1u << 0)
#define TX_CMD_IFCS (1u << 1)
#define TX_CMD_RS (1u << 3)
#define TX_DD (1u << 0)

#define NUM_RX 32
#define NUM_TX 32

static volatile u32 *g_regs;     /* BAR0 MMIO */
static u8  g_mac[6];
static bool g_up;
static u8  g_irq_line;           /* PCI INTx line (0 = none / MSI-X) */

static inline u32 reg_rd(u32 off);
static inline void reg_wr(u32 off, u32 v);

/* Clear any pending device interrupt so a level-triggered line never storms.
 * We poll, so this just keeps the line deasserted. */
void e1000_irq_handler(cpu_regs_t *r) {
    (void)r;
    reg_rd(REG_ICR);             /* read-and-clear interrupts */
}
u8 e1000_irq_line(void) { return g_irq_line; }

static e1000_rx_desc *g_rxring;
static u8           *g_rxbuf_v[NUM_RX];
static paddr_t       g_rxbuf_p[NUM_RX];
static e1000_tx_desc *g_txring;
static u8           *g_txbuf_v[NUM_TX];
static paddr_t       g_txbuf_p[NUM_TX];
static u32 g_rx_sw;              /* software RX consumer index */
static u32 g_tx_sw;              /* software TX producer index */

/* driver-level RX FIFO of completed frames (drained by net_service) */
#define RXFIFO 64
#define RXFRAME 2048
static u8   g_rxfifo[RXFIFO][RXFRAME];
static u16  g_rxfifo_len[RXFIFO];
static u32  g_rxfifo_head, g_rxfifo_tail;
static u32  g_rx_drop;
static spinlock_t g_rxfifo_lock;

static inline u32 reg_rd(u32 off) { return g_regs[off]; }
static inline void reg_wr(u32 off, u32 v) { g_regs[off] = v; __asm__ volatile("" ::: "memory"); }

static void rx_fifo_push(const u8 *frame, u16 len) {
    u64 fl = irq_save();
    spin_lock(&g_rxfifo_lock);
    u32 next = (g_rxfifo_head + 1) % RXFIFO;
    if (next == g_rxfifo_tail) { g_rx_drop++; spin_unlock(&g_rxfifo_lock); irq_restore(fl); return; }
    memcpy(g_rxfifo[g_rxfifo_head], frame, len);
    g_rxfifo_len[g_rxfifo_head] = len;
    g_rxfifo_head = next;
    spin_unlock(&g_rxfifo_lock);
    irq_restore(fl);
}
static int rx_fifo_pop(u8 *out, u16 cap) {
    u64 fl = irq_save();
    spin_lock(&g_rxfifo_lock);
    if (g_rxfifo_tail == g_rxfifo_head) { spin_unlock(&g_rxfifo_lock); irq_restore(fl); return 0; }
    u16 len = g_rxfifo_len[g_rxfifo_tail];
    if (cap < len) len = cap;
    memcpy(out, g_rxfifo[g_rxfifo_tail], len);
    g_rxfifo_tail = (g_rxfifo_tail + 1) % RXFIFO;
    spin_unlock(&g_rxfifo_lock);
    irq_restore(fl);
    return len;
}

/* Drain the hardware RX ring into our FIFO, then hand buffers back. */
static void rx_drain(void) {
    if (!g_up) return;
    while (g_rxring[g_rx_sw].status & RX_DD) {
        u16 len = g_rxring[g_rx_sw].length;
        if (len > RXFRAME) len = RXFRAME;
        if (len <= 1518 && (g_rxring[g_rx_sw].status & RX_EOP))
            rx_fifo_push(g_rxbuf_v[g_rx_sw], len);
        /* return this buffer to the hardware */
        g_rxring[g_rx_sw].status = 0;
        g_rxring[g_rx_sw].addr = g_rxbuf_p[g_rx_sw];
        g_rx_sw = (g_rx_sw + 1) % NUM_RX;
    }
    __asm__ volatile("mfence" ::: "memory");   /* DMA visibility barrier */
    reg_wr(REG_RDT, (g_rx_sw + NUM_RX - 1) % NUM_RX);
}

int nic_rx(u8 *out, u16 cap) { return rx_fifo_pop(out, cap); }

int nic_send(const u8 *frame, u16 len) {
    if (!g_up || len > 1518) return -1;
    /* wait until the TX slot we're about to use has been transmitted */
    u32 tries = 0;
    while (!(g_txring[g_tx_sw].status & TX_DD)) {
        if (++tries > 2000000u) return -1;   /* ring full / stalled */
        __asm__ volatile("pause");
    }
    memcpy(g_txbuf_v[g_tx_sw], frame, len);
    g_txring[g_tx_sw].addr = g_txbuf_p[g_tx_sw];
    g_txring[g_tx_sw].length = len;
    g_txring[g_tx_sw].cmd = TX_CMD_EOP | TX_CMD_IFCS | TX_CMD_RS;
    g_txring[g_tx_sw].status = 0;
    __asm__ volatile("mfence" ::: "memory");   /* DMA visibility barrier */
    g_tx_sw = (g_tx_sw + 1) % NUM_TX;
    reg_wr(REG_TDT, g_tx_sw);
    return 0;
}

void nic_mac(u8 out[6]) { memcpy(out, g_mac, 6); }
bool nic_present(void) { return g_up; }

void nic_init(void) {
    /* find the e1000 (class 0x02, vendor 0x8086) */
    u8 bus = 0, dev = 0, fn = 0; bool found = false;
    for (int b = 0; b < 4 && !found; b++)
      for (int d = 0; d < 32 && !found; d++)
        for (int f = 0; f < 8 && !found; f++) {
            u32 w0 = pci_rd32(b, d, f, 0x00);
            if ((w0 & 0xFFFF) == 0xFFFF) continue;
            u32 w2 = pci_rd32(b, d, f, 0x08);
            u8 cls = (w2 >> 24) & 0xFF;
            if (cls == 0x02 && (w0 & 0xFFFF) == 0x8086) {
                bus = b; dev = d; fn = f; found = true;
            }
        }
    if (!found) { kprintf("net: no e1000 NIC found\n"); return; }

    u32 bar0 = pci_rd32(bus, dev, fn, 0x10);
    u32 cmd = pci_rd32(bus, dev, fn, 0x04);
    cmd |= 0x07;                     /* I/O + mem + bus master */
    pci_wr32(bus, dev, fn, 0x04, cmd);
    g_irq_line = (u8)(pci_rd32(bus, dev, fn, 0x3C) & 0xFF);

    u32 base = bar0 & ~0xFULL;
    g_regs = (volatile u32 *)phys_to_virt((paddr_t)base);
    kprintf("net: e1000 at %x:%x.%x bar0=0x%x\n", bus, dev, fn, base);

    /* Route the NIC's INTx line to a handler that clears the device interrupt
     * (ICR) so a level-triggered line can never assert forever and flood the
     * CPU.  We poll, but this keeps any stray device interrupt handled. */
    if (g_irq_line) {
        u8 vec = (u8)(49 + ((g_irq_line + 1) & 0xF));
        if (vec >= 64) vec = 62;
        if (vec < 64) {
            apic_route_irq(g_irq_line, vec, false);
            irq_register(vec, e1000_irq_handler);
            kprintf("net: e1000 irq %u -> vec %u (polled, irq cleared)\n",
                    g_irq_line, vec);
        }
    }

    /* Bring the link up and mask ALL device interrupts up front (we poll).
     * We deliberately avoid the full CTRL_RST software reset: resetting the
     * NIC during bring-up (and its 100k-pause wait) was found to intermittently
     * destabilize AP scheduling, and it isn't needed under QEMU where the
     * device comes up ready.  We also don't touch the PCI interrupt line. */
    reg_wr(REG_CTRL, CTRL_SLU);      /* set link up */
    reg_wr(REG_IMC, 0xFFFFFFFF);     /* disable all interrupts (we poll) */
    reg_rd(REG_ICR);                 /* read + clear any pending interrupt */

    /* read MAC from the RAL/RAH the device loaded */
    u32 ral = reg_rd(REG_RAL), rah = reg_rd(REG_RAH);
    g_mac[0] = ral & 0xFF; g_mac[1] = (ral >> 8) & 0xFF;
    g_mac[2] = (ral >> 16) & 0xFF; g_mac[3] = (ral >> 24) & 0xFF;
    g_mac[4] = rah & 0xFF; g_mac[5] = (rah >> 8) & 0xFF;

    /* RX ring + buffers */
    paddr_t rrp = pmm_alloc_pages(sizeof(e1000_rx_desc) * NUM_RX / PAGE_SIZE + 1);
    g_rxring = phys_to_virt(rrp);
    memset(g_rxring, 0, sizeof(e1000_rx_desc) * NUM_RX);
    for (int i = 0; i < NUM_RX; i++) {
        paddr_t bp = pmm_alloc_page();
        g_rxbuf_p[i] = bp;
        g_rxbuf_v[i] = phys_to_virt(bp);
        g_rxring[i].addr = bp;
        g_rxring[i].status = 0;
    }
    g_rx_sw = 0;
    reg_wr(REG_RDBAL, (u32)(rrp & 0xFFFFFFFF));
    reg_wr(REG_RDBAH, (u32)((u64)rrp >> 32));
    reg_wr(REG_RDLEN, sizeof(e1000_rx_desc) * NUM_RX);
    reg_wr(REG_RDH, 0);
    reg_wr(REG_RDT, NUM_RX - 1);
    /* accept broadcast + ALL multicast (MPE + all-ones MTA): IPv6 NDP/RA
     * frames go to 33:33:.. multicast groups; without this every RA/NS/NA
     * is silently dropped by the NIC's multicast filter. */
    for (int i = 0; i < 128; i++)
        reg_wr(REG_MTA + (u32)i, 0xFFFFFFFFu);
    reg_wr(REG_RCTL, RCTL_EN | RCTL_SBP | RCTL_MPE | RCTL_BAM | RCTL_SECRC |
                     RCTL_BSIZE_2048);
    /* program our MAC as the receive address */
    reg_wr(REG_RAL, ral);
    reg_wr(REG_RAH, (rah & 0xFFFF) | 0x80000000u);   /* AV bit */
    /* accept all multicast (set MTA bits 31..0) */
    for (int i = 0; i < 4; i++) reg_wr(REG_MTA + i, 0xFFFFFFFF);

    /* TX ring + buffers */
    paddr_t trp = pmm_alloc_pages(sizeof(e1000_tx_desc) * NUM_TX / PAGE_SIZE + 1);
    g_txring = phys_to_virt(trp);
    memset(g_txring, 0, sizeof(e1000_tx_desc) * NUM_TX);
    for (int i = 0; i < NUM_TX; i++) {
        paddr_t bp = pmm_alloc_page();
        g_txbuf_p[i] = bp;
        g_txbuf_v[i] = phys_to_virt(bp);
        g_txring[i].status = TX_DD;    /* all slots initially free */
    }
    g_tx_sw = 0;
    reg_wr(REG_TDBAL, (u32)(trp & 0xFFFFFFFF));
    reg_wr(REG_TDBAH, (u32)((u64)trp >> 32));
    reg_wr(REG_TDLEN, sizeof(e1000_tx_desc) * NUM_TX);
    reg_wr(REG_TDH, 0);
    reg_wr(REG_TDT, 0);
    reg_wr(REG_TIPG, 0x0060200A);
    reg_wr(REG_TCTL, TCTL_EN | TCTL_PSP | TCTL_CT | TCTL_COLD);

    g_rxfifo_head = g_rxfifo_tail = 0;
    g_up = true;
    kprintf("net: e1000 up, MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
            g_mac[0], g_mac[1], g_mac[2], g_mac[3], g_mac[4], g_mac[5]);
}

/* Called by net_service: pull hardware frames into the FIFO. */
void nic_poll(void) { if (g_up) rx_drain(); }
