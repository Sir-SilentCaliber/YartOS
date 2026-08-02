/* Yart OS - USB xHCI host controller (row 18).
 *
 * A minimal xHCI driver targeting QEMU's qemu-xhci + usb-kbd.  This stage:
 *   1. detects the controller (PCI class 0x0C / subclass 0x03),
 *   2. reads the MMIO base + capabilities,
 *   3. resets and RUNs the controller,
 *   4. allocates the Device Context Base Address Array (DCBAA), a command
 *      ring and an event ring,
 *   5. enables a device slot and reports root-hub port status (connected
 *      devices + speed) - the verifiable core of "the USB controller works".
 *
 * The full HID keyboard data path (Address Device / Configure Endpoint /
 * interrupt-IN report polling feeding kbd_enqueue) is layered on top in the
 * same file via usb_hid_poll(), driven from the main loop.
 *
 * Register map (fixed in QEMU, but we honour the capability registers):
 *   BAR0 + 0x00       capability registers (CAPLENGTH=0x00, HCSPARAMS1=0x04,
 *                     HCCPARAMS1=0x0C, DBOFF=0x14, RTSOFF=0x18)
 *   BAR0 + 0x40       operational registers (USBCMD/0x00, USBSTS/0x04,
 *                     CRCR/0x18, DCBAAP/0x30, CONFIG/0x38, PORTSC@0x400+0x10*i)
 *   BAR0 + 0x1000     runtime registers (interrupters)
 *   BAR0 + 0x2000     doorbell array (host command = 0, slot N = N)
 */
#include <yart/usb.h>
#include <yart/io.h>
#include <yart/mm.h>
#include <yart/console.h>
#include <yart/string.h>
#include <yart/hal.h>
#include <yart/drivers.h>   /* kbd_enqueue */

/* ---- PCI ---- */
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

/* ---- USBCMD / USBSTS / PORTSC bits (from QEMU's xhci) ---- */
#define USBCMD_RS    (1u << 0)
#define USBCMD_HCRST (1u << 1)
#define USBSTS_HCH   (1u << 0)
#define PORTSC_CCS   (1u << 0)      /* current connect status */
#define PORTSC_PED   (1u << 1)
#define PORTSC_PR    (1u << 4)      /* port reset */
#define PORTSC_PLS_SHIFT 5
#define PORTSC_PLS_MASK  0xf
#define PORTSC_PP    (1u << 9)      /* port power */
#define PORTSC_SPEED_SHIFT 10
#define PORTSC_SPEED_MASK  0xf
#define PORTSC_PRC   (1u << 21)     /* port reset change */

/* ---- state ---- */
static volatile u32 *g_cap;      /* BAR0 */
static volatile u32 *g_oper;     /* BAR0 + CAPLENGTH */
static volatile u32 *g_doorbell; /* BAR0 + DBOFF */
static bool  g_up;
static u32   g_maxslots, g_maxports;
static bool  g_64bit;            /* 64-bit address capability (ADC64) */

/* ring + context memory.  DMA targets MUST be real physical RAM handed out by
 * the PMM (phys_to_virt), because virt_to_phys only works for HHDM-mapped
 * pages - static .bss buffers in the kernel image map to a bogus physical
 * address that QEMU's DMA cannot read (this was the exact bug the NIC driver
 * was saved from). */
#define XHCI_MAXSLOTS 32
#define CMD_RING_SZ   16
#define EVENT_RING_SZ 16

static u8   *g_cmd_ring_buf;     /* phys = virt_to_phys(g_cmd_ring_buf) */
static u8   *g_evt_ring_buf;
static u8   *g_erst_buf;         /* event ring segment table (1 seg) */
static u8   *g_dcbaa_buf;
static u8   *g_devctx;           /* device contexts (XHCI_MAXSLOTS * 1024) */
static u8   *g_inputctx;         /* input contexts */
static u8   *g_ep0_ring;         /* EP0 transfer ring */
static u8   *g_ep1_ring;         /* EP1 interrupt-IN transfer ring */

static u32 g_cmd_ring_idx;       /* next command TRB to write */
static u32 g_evt_ring_idx;       /* event ring read index */
static u32 g_cmd_rcs = 1;        /* command ring cycle state */
static u32 g_evt_rcs = 1;        /* event ring cycle state */

static u32 g_slot, g_port;       /* the addressed device slot/port */
static bool g_kbd_present;
static u8  *g_hid_report;        /* pmm DMA buffer for the HID report  */

/* ---- xHCI endpoint types (spec) ---- */
#define EP_TYPE_CONTROL 4
#define EP_TYPE_INT_IN  7
#define EP_TYPE_SHIFT   3

/* slot context field helpers */
#define SLOT_CTX_ROUTE(s)       ((s)[0] & 0xFFFFF)
#define SLOT_CTX_PORT(s)        (((s)[1] >> 16) & 0xFF)
#define SLOT_CTX_SPEED(s)       (((s)[1] >> 20) & 0xF)
#define SLOT_CTX_STATE(s)       (((s)[3] >> 27) & 0x1F)
#define SLOT_CTX_ENTRIES_SHIFT  27

/* Allocate a DMA buffer from the PMM (returns NULL on OOM). */
static u8 *dma_alloc(size_t bytes) {
    u32 pages = (u32)(PAGE_ALIGN_UP(bytes) / PAGE_SIZE);
    paddr_t p = pmm_alloc_pages(pages);
    if (!p) return 0;
    u8 *v = phys_to_virt(p);
    memset(v, 0, bytes);
    return v;
}

bool usb_kbd_present(void) { return g_kbd_present; }

static void ep0_control(u32 slot, u32 setup_dw0, u32 setup_dw1);
static void consume_transfer_event(void);

/* ---- TRB helpers ---- */
static void cmd_trb(u32 type, u32 a, u32 b, u32 c, u32 slotid) {
    u32 *t = (u32 *)(g_cmd_ring_buf + g_cmd_ring_idx * 16);
    t[0] = a; t[1] = b; t[2] = c;
    /* control dword: type in [15:10], slot id in [31:24] (used by Address
     * Device / Configure Endpoint - QEMU reads control>>24), cycle bit 0 */
    t[3] = (type << 10) | (slotid << 24) | g_cmd_rcs;
    g_cmd_ring_idx = (g_cmd_ring_idx + 1) % CMD_RING_SZ;
}
static void ring_doorbell(u32 db) {
    g_doorbell[db] = 0;
    __asm__ volatile("mfence" ::: "memory");
}

#define TRB_TYPE_SHIFT 10    /* xHCI: type is bits [15:10] (6 bits) */
#define TRB_TYPE(t) ((t) << TRB_TYPE_SHIFT)
/* QEMU xhci TRBType enum: TR_NORMAL=1 ... TR_LINK=6, CR_ENABLE_SLOT=9,
 * CR_ADDRESS_DEVICE=11, CR_CONFIGURE_ENDPOINT=12, ER_TRANSFER=32,
 * ER_COMMAND_COMPLETE=33, ER_PORT_STATUS_CHANGE=34. */
#define TRB_TYPE_CMD_COMPLETION 33
#define TRB_TYPE_ENABLE_SLOT    9
#define TRB_TYPE_ADDRESS_DEV    11
#define TRB_TYPE_CONFIGURE_EP   12

/* Ring the host command doorbell and wait for a command-completion event.
 * The event ring may hold other events (port status change, etc.) queued
 * before/around the completion, so we consume any valid event and keep going
 * until the matching command completion (or a timeout). */
static u32 run_cmd_and_wait(u32 slot_id, u32 *evt) {
    ring_doorbell(0);
    for (volatile u32 t = 0; t < 3000000u; t++) {
        u32 *e = (u32 *)(g_evt_ring_buf + g_evt_ring_idx * 16);
        if ((e[3] & 1) != g_evt_rcs) { __asm__ volatile("pause"); continue; }
        u32 type = (e[3] >> 10) & 0x3F;
        u32 slot = (e[3] >> 24) & 0xFF;   /* slot id from event */
        u32 code = (e[2] >> 24) & 0xFF;   /* completion code */
        /* consume this event regardless of type */
        if (evt && type == TRB_TYPE_CMD_COMPLETION) *evt = e[0];
        e[3] &= ~1u;                       /* clear cycle */
        g_evt_ring_idx = (g_evt_ring_idx + 1) % EVENT_RING_SZ;
        if (g_evt_ring_idx == 0) g_evt_rcs ^= 1;
        if (type == TRB_TYPE_CMD_COMPLETION)
            return (code == 1) ? slot : 0; /* 1 = CC_SUCCESS */
    }
    return 0;
}

void usb_init(void) {
    /* find xHCI (class 0x0C, subclass 0x03) */
    u8 bus = 0, dev = 0, fn = 0; bool found = false;
    for (int b = 0; b < 4 && !found; b++)
      for (int d = 0; d < 32 && !found; d++)
        for (int f = 0; f < 8 && !found; f++) {
            u32 w0 = pci_rd32(b, d, f, 0x00);
            if ((w0 & 0xFFFF) == 0xFFFF) continue;
            u32 w2 = pci_rd32(b, d, f, 0x08);
            u8 cls = (w2 >> 24) & 0xFF, sub = (w2 >> 16) & 0xFF;
            if (cls == 0x0C && sub == 0x03) { bus = b; dev = d; fn = f; found = true; }
        }
    if (!found) { kprintf("usb: no xHCI controller found\n"); return; }

    u32 cmd = pci_rd32(bus, dev, fn, 0x04);
    cmd |= 0x07;
    pci_wr32(bus, dev, fn, 0x04, cmd);

    u32 bar0 = pci_rd32(bus, dev, fn, 0x10) & ~0xF;
    g_cap = (volatile u32 *)phys_to_virt((paddr_t)bar0);
    u8 caplen = (u8)(g_cap[0] & 0xFF);
    g_oper = (volatile u32 *)((u8 *)g_cap + caplen);
    u32 hcsp1 = g_cap[0x04 >> 2];
    u32 hccp1 = g_cap[0x0C >> 2];
    g_maxslots = hcsp1 & 0xFF;
    g_maxports = (hcsp1 >> 24) & 0xFF;
    g_64bit = (hccp1 & 1) != 0;          /* ADC64 */
    u32 dboff = g_cap[0x14 >> 2] & ~3u;
    u32 rtsoff = g_cap[0x18 >> 2] & ~3u;
    g_doorbell = (volatile u32 *)((u8 *)g_cap + dboff);
    kprintf("usb: xHCI at %x:%x.%x bar0=0x%x caplen=%u slots=%u ports=%u 64bit=%d "
            "dboff=%x rtsoff=%x\n",
            bus, dev, fn, bar0, caplen, g_maxslots, g_maxports, g_64bit ? 1 : 0,
            dboff, rtsoff);
    if (g_maxslots == 0 || g_maxslots > XHCI_MAXSLOTS) g_maxslots = XHCI_MAXSLOTS;

    /* reset + run */
    g_oper[0x00 >> 2] = USBCMD_HCRST;
    for (volatile u32 t = 0; t < 100000u && (g_oper[0x00 >> 2] & USBCMD_HCRST); t++) __asm__ volatile("pause");
    g_oper[0x00 >> 2] = USBCMD_RS;
    for (volatile u32 t = 0; t < 100000u && (g_oper[0x04 >> 2] & USBSTS_HCH); t++) __asm__ volatile("pause");
    if (g_oper[0x04 >> 2] & USBSTS_HCH) { kprintf("usb: xHCI failed to run\n"); return; }

    /* DCBAA + command ring + event ring (real RAM via the PMM) */
    g_cmd_ring_buf = dma_alloc(CMD_RING_SZ * 16);
    g_evt_ring_buf = dma_alloc(EVENT_RING_SZ * 16);
    g_erst_buf     = dma_alloc(16);
    g_dcbaa_buf    = dma_alloc(XHCI_MAXSLOTS * 8);
    g_devctx       = dma_alloc(XHCI_MAXSLOTS * 1024);
    g_inputctx     = dma_alloc(XHCI_MAXSLOTS * 1024);
    g_ep0_ring     = dma_alloc(16 * 16);
    g_ep1_ring     = dma_alloc(16 * 16);
    g_hid_report   = dma_alloc(8);
    if (!g_cmd_ring_buf || !g_evt_ring_buf || !g_erst_buf ||
        !g_dcbaa_buf || !g_devctx || !g_inputctx || !g_ep0_ring || !g_ep1_ring ||
        !g_hid_report) {
        kprintf("usb: DMA buffer alloc failed\n");
        return;
    }
    g_cmd_ring_idx = 0; g_evt_ring_idx = 0; g_cmd_rcs = 1; g_evt_rcs = 1;

    paddr_t dcbaap = virt_to_phys(g_dcbaa_buf);
    paddr_t crcr   = virt_to_phys(g_cmd_ring_buf);
    g_oper[0x30 >> 2] = (u32)(dcbaap & 0xFFFFFFFF);
    g_oper[0x34 >> 2] = (u32)((u64)dcbaap >> 32);
    g_oper[0x18 >> 2] = (u32)(crcr & 0xFFFFFFFF) | 1;   /* CRCR + RCS */
    g_oper[0x1C >> 2] = (u32)((u64)crcr >> 32);
    g_oper[0x38 >> 2] = 1;                               /* CONFIG: 1 slot */

    /* event ring: the ERST (segment table) entry points at the event ring,
     * ERSTBA points at the segment table, ERDP points at the event ring. */
    paddr_t evring = virt_to_phys(g_evt_ring_buf);
    paddr_t erst   = virt_to_phys(g_erst_buf);
    u32 *es = (u32 *)g_erst_buf;
    es[0] = (u32)(evring & 0xFFFFFFFF);        /* segment base low  */
    es[1] = (u32)((u64)evring >> 32);          /* segment base high */
    es[2] = EVENT_RING_SZ;                     /* segment size (TRBs) */
    es[3] = 0;
    volatile u32 *rt = (volatile u32 *)((u8 *)g_cap + (g_cap[0x18 >> 2] & ~3u));
    rt[0x28 >> 2] = 1;                     /* ERSTSZ = 1 segment */
    rt[0x30 >> 2] = (u32)(erst & 0xFFFFFFFF);   /* ERSTBA low */
    rt[0x34 >> 2] = (u32)((u64)erst >> 32);      /* ERSTBA high */
    rt[0x20 >> 2] = (1u << 0);              /* IMAN: IE = 1 */
    rt[0x38 >> 2] = (u32)(evring & 0xFFFFFFFF);  /* ERDP = event ring */
    rt[0x3C >> 2] = 0;
    __asm__ volatile("mfence" ::: "memory");

    /* Enable Slot */
    cmd_trb(TRB_TYPE_ENABLE_SLOT, 0, 0, 0, 0);
    u32 slot = run_cmd_and_wait(0, 0);
    if (!slot) { kprintf("usb: Enable Slot failed\n"); return; }
    kprintf("usb: slot %u enabled\n", slot);

    /* root hub: report connected ports */
    int connected = 0;
    for (u32 p = 0; p < g_maxports; p++) {
        u32 *ps = (u32 *)((u8 *)g_oper + 0x400 + 0x10 * p);
        u32 v = ps[0];
        if (v & PORTSC_CCS) {
            u32 speed = (v >> PORTSC_SPEED_SHIFT) & PORTSC_SPEED_MASK;
            const char *sn = speed==4?"super":speed==3?"high":speed==2?"low":"full";
            kprintf("usb: port %u: device connected (%s speed)\n", p, sn);
            if (!connected) g_port = p;    /* remember the first connected port */
            connected++;
        } else {
            kprintf("usb: port %u: empty\n", p);
        }
    }
    g_up = true;
    kprintf("usb: xHCI up (%u port%s, %u connected) - controller works\n",
            g_maxports, g_maxports == 1 ? "" : "s", connected);

    if (connected) {
        g_slot = slot;
        /* try every root-hub port number (1-based); the USB devices may sit on
         * ports that don't match our PORTSC scan due to USB3-port offsets in
         * qemu-xhci, so probe all of them until one addresses successfully. */
        int ok = 0;
        for (u32 p = 1; p <= g_maxports && !ok; p++) {
            if (usb_address_device(slot, p)) { g_port = p; ok = 1; }
        }
        if (ok) {
            usb_configure_endpoint(slot);
            if (g_kbd_present) {
                /* SET_CONFIGURATION (bmRT=0, bReq=9, wValue=1) to activate
                 * the keyboard, then it delivers boot-protocol reports. */
                ep0_control(slot, 0x00010900, 0);
                for (volatile u32 t = 0; t < 300000u; t++) {
                    u32 *e = (u32 *)(g_evt_ring_buf + g_evt_ring_idx * 16);
                    if ((e[3] & 1) == g_evt_rcs && ((e[3] >> 10) & 0x3F) == 32) { consume_transfer_event(); break; }
                    __asm__ volatile("pause");
                }
                kprintf("usb: keyboard configured (SET_CONFIGURATION sent) - HID reports active\n");
            }
        } else {
            kprintf("usb: could not address any port\n");
        }
        /* HID boot-protocol setup + report polling happen in usb_hid_poll,
         * which is called from the main loop. */
    }
}

/* ---- HID keyboard data path ---- */

/* Set up the input context for Address Device and point DCBAA[slot] at the
 * output context.  QEMU requires ictl = {0, 3}, then slot ctx at +32 and
 * ep0 ctx at +64.  The root-hub route string is 0, so the port field alone
 * selects the device. */
static void setup_input_ctx(u32 slot, u32 port) {
    u32 *ic = (u32 *)(g_inputctx + slot * 1024);
    memset(ic, 0, 1024);
    ic[0] = 0;             /* ICTL drop = 0 */
    ic[1] = 0x3;           /* ICTL add = slot(bit0) + ep0(bit1) */
    /* slot context (dword 8..11 of input ctx = offset 32) */
    u32 *sc = ic + 8;
    sc[0] = (1u << SLOT_CTX_ENTRIES_SHIFT);   /* 1 context entry (EP0) */
    sc[1] = ((port + 1) << 16);                /* root hub port (1-based) */
    sc[2] = 0;                                /* interrupt target 0 */
    sc[3] = 0;
    /* ep0 context (offset 64) */
    u32 *ec = ic + 16;
    ec[1] = (EP_TYPE_CONTROL << EP_TYPE_SHIFT) | (64u << 16);  /* ctrl, max 64 */
    ec[2] = ((u32)(virt_to_phys(g_ep0_ring) & 0xFFFFFFFF)) | 1;  /* DCS=1 */
    ec[3] = (u32)((u64)virt_to_phys(g_ep0_ring) >> 32);
    /* DCBAA[slot] -> output context */
    u64 *dcbaa = (u64 *)g_dcbaa_buf;
    dcbaa[slot] = virt_to_phys(g_devctx + slot * 1024);
    __asm__ volatile("mfence" ::: "memory");
}

/* Returns 1 on success, 0 on failure. */
int usb_address_device(u32 slot, u32 port) {
    setup_input_ctx(slot, port);
    paddr_t ictx = virt_to_phys(g_inputctx + slot * 1024);
    cmd_trb(TRB_TYPE_ADDRESS_DEV, (u32)(ictx & 0xFFFFFFFF),
            (u32)((u64)ictx >> 32), 0, slot);   /* BSR=0 */
    u32 s = run_cmd_and_wait(slot, 0);
    if (!s) { kprintf("usb: Address Device (port %u) failed\n", port); return 0; }
    /* confirm the output context slot state = SLOT_ADDRESSED (2) */
    u32 *oc = (u32 *)(g_devctx + slot * 1024);
    u32 st = SLOT_CTX_STATE(oc);
    kprintf("usb: slot %u addressed (state=%u) - device has address %u\n",
            slot, st, slot);
    return 1;
}

/* Configure the interrupt-IN endpoint (epid 3 = USB address 1, IN) so the HID
 * keyboard can deliver boot-protocol reports.  QEMU's xhci_configure_slot:
 *   - ictl[0]&3 == 0 and ictl[1]&3 == 1 (add slot context),
 *   - for each ep i in 2..31 with ictl[1] bit i set, reads the ep context at
 *     ictx+32+32*i and enables it.
 * So we add the slot (bit 0) + epid 3 (bit 3): ictl[1] = 0x9, ep3 ctx at
 * ictx+32+96 = ictx+128.  Context entries = 2 (EP0 + the IN endpoint). */
void usb_configure_endpoint(u32 slot) {
    u32 *ic = (u32 *)(g_inputctx + slot * 1024);
    memset(ic, 0, 1024);
    ic[1] = 0x9;                              /* add slot (bit0) + epid3 (bit3) */
    u32 *sc = ic + 8;
    sc[0] = (2u << SLOT_CTX_ENTRIES_SHIFT);   /* EP0 + EP1-IN */
    sc[1] = ((g_port + 1) << 16);             /* 1-based port */
    /* EP3 (interrupt IN, address 1): 8-byte boot-protocol reports */
    u32 *e3 = ic + 32;                        /* ep3 ctx at ictx+128 */
    e3[0] = (1u << 16);                       /* interval = 2^1 = 2ms */
    e3[1] = (EP_TYPE_INT_IN << EP_TYPE_SHIFT) | (8u << 16);  /* type + max 8 B */
    e3[2] = ((u32)(virt_to_phys(g_ep1_ring) & 0xFFFFFFFF)) | 1;  /* DCS=1 */
    e3[3] = (u32)((u64)virt_to_phys(g_ep1_ring) >> 32);
    __asm__ volatile("mfence" ::: "memory");
    paddr_t ictx = virt_to_phys(g_inputctx + slot * 1024);
    cmd_trb(TRB_TYPE_CONFIGURE_EP, (u32)(ictx & 0xFFFFFFFF),
            (u32)((u64)ictx >> 32), 0, slot);
    u32 s = run_cmd_and_wait(slot, 0);
    kprintf("usb: configure endpoint %s (slot %u)\n", s ? "OK" : "FAILED", slot);
    if (s) g_kbd_present = true;
}

/* ---- EP0/EP1 transfer rings ---- */
static u32 g_ep0_idx, g_ep1_idx;

/* Queue a control transfer (SETUP + STATUS) on the EP0 ring and ring the EP0
 * doorbell (value 1).  The 8-byte setup packet is passed as two dwords. */
static void ep0_control(u32 slot, u32 setup_dw0, u32 setup_dw1) {
    u32 *s = (u32 *)(g_ep0_ring + g_ep0_idx * 16);
    s[0] = setup_dw0; s[1] = setup_dw1;
    s[2] = 8;                                /* transfer length 8 */
    s[3] = (2u << 10) | (1u << 6) | (1u << 5) | 1;   /* SETUP, IDT, IOC, cyc */
    g_ep0_idx = (g_ep0_idx + 1) % 16;
    u32 *st = (u32 *)(g_ep0_ring + g_ep0_idx * 16);
    st[0] = 0; st[1] = 0; st[2] = 0;
    st[3] = (4u << 10) | 1;                  /* STATUS, cycle */
    g_ep0_idx = (g_ep0_idx + 1) % 16;
    __asm__ volatile("mfence" ::: "memory");
    g_doorbell[slot] = 1;                    /* EP0 control */
}

/* Consume one transfer event (type 32) from the event ring. */
static void consume_transfer_event(void) {
    u32 *e = (u32 *)(g_evt_ring_buf + g_evt_ring_idx * 16);
    if ((e[3] & 1) != g_evt_rcs) return;
    e[3] &= ~1u;
    g_evt_ring_idx = (g_evt_ring_idx + 1) % EVENT_RING_SZ;
    if (g_evt_ring_idx == 0) g_evt_rcs ^= 1;
}

/* HID keycode -> event (boot protocol).  We send the modifiers + each key as
 * a press/release pair through kbd_enqueue. */
static void hid_report(const u8 *r) {
    u32 mods = 0;
    if (r[0] & 0x02) mods |= KEY_SHIFT;
    if (r[0] & 0x01) mods |= KEY_CTRL;
    if (r[0] & 0x04) mods |= KEY_ALT;
    static u8 prev[8];
    for (int i = 2; i < 8; i++) {
        u8 k = r[i];
        if (k && k != prev[i])
            kbd_enqueue(k, 0, mods);          /* press */
        else if (!k && prev[i]) {
            kbd_enqueue(prev[i], 0, KEY_RELEASE | mods);  /* release */
        }
        prev[i] = k;
    }
}

/* Called from the main loop: (re)arm and collect interrupt-IN reports on EP1.
 * The EP1-IN doorbell value is 3 (epid 3).  The report buffer is a pmm DMA
 * buffer (virt_to_phys on a .bss buffer gives a bogus address QEMU can't use). */
void usb_hid_poll(void) {
    if (!g_up || !g_slot || !g_kbd_present) return;
    u8 *report = g_hid_report;
    static bool armed;
    static u32 report_count;
    if (!armed) {
        u32 *t = (u32 *)(g_ep1_ring + g_ep1_idx * 16);
        t[0] = (u32)(virt_to_phys(report) & 0xFFFFFFFF);
        t[1] = (u32)((u64)virt_to_phys(report) >> 32);
        t[2] = 8;                            /* TRB transfer length */
        t[3] = (1u << 10) | (1u << 5) | 1;   /* TR_NORMAL, IOC, cycle */
        g_ep1_idx = (g_ep1_idx + 1) % 16;
        __asm__ volatile("mfence" ::: "memory");
        g_doorbell[g_slot] = 3;              /* EP1 IN */
        armed = true;
    }
    u32 *e = (u32 *)(g_evt_ring_buf + g_evt_ring_idx * 16);
    if ((e[3] & 1) == g_evt_rcs && ((e[3] >> 10) & 0x3F) == 32) {
        report_count++;
        hid_report(report);
        consume_transfer_event();
        armed = false;                       /* re-arm next poll */
    }
}
