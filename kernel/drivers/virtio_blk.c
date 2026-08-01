/* Yart OS - virtio-blk driver: BOTH legacy (virtio 0.9) and modern
 * (virtio 1.0) PCI interfaces.
 *
 *   - modern : PCI vendor capability (0x09) with common/notify/isr/device
 *              regions, 64-bit queue addresses, real feature negotiation
 *              (VERSION_1 mandatory bit), INTx interrupt delivery
 *              (QEMU: -device virtio-blk-pci,disable-legacy=on,msix=off)
 *   - legacy : fixed BAR0 registers, queue PFN, page-sized queues
 *              (QEMU: -device virtio-blk-pci,disable-modern=on)
 *   - transitional : the capability list decides which path is used.
 *
 * Both modes are interrupt-driven (INTx routed through the IOAPIC by
 * apic.c) with a polling fallback.  I/O is scatter-gather at the virtqueue
 * level: each request is a 3-descriptor chain (header, data, status) and
 * data bounces through a DMA page so the device only touches prepared
 * physical memory.  Multi-queue is intentionally one queue: this kernel is
 * single-CPU, so one queue is the correct (and spec-legal) configuration.
 */
#include <yart/blk.h>
#include <yart/io.h>
#include <yart/mm.h>
#include <yart/console.h>
#include <yart/string.h>
#include <yart/hal.h>   /* apic_local_id, irq_register */

/* virtio legacy PCI register offsets (Linux/QEMU legacy layout) */
#define VQ_HOST_FEAT    0x00
#define VQ_GUEST_FEAT   0x04
#define VQ_QUEUE_PFN    0x08
#define VQ_QUEUE_NUM    0x0C
#define VQ_QUEUE_SEL    0x0E
#define VQ_QUEUE_NOTIFY 0x10
#define VQ_STATUS       0x12
#define VQ_ISR          0x13
#define VQ_CONFIG       0x14   /* device config: 0x14 when MSI-X disabled  */

/* modern PCI capability cfg_type values */
#define VP_CAP_COMMON 1
#define VP_CAP_NOTIFY 2
#define VP_CAP_ISR    3
#define VP_CAP_DEVICE 4

#define VQ_SIZE 128                       /* power of two                   */

typedef struct PACKED { u64 addr; u32 len; u16 flags; u16 next; } vq_desc_t;
typedef struct PACKED { u16 flags; u16 idx; u16 ring[VQ_SIZE]; } vq_avail_t;
typedef struct PACKED { u16 flags; u16 idx; struct PACKED { u32 id; u32 len; } ring[VQ_SIZE]; } vq_used_t;

#define VIRTQ_DESC_F_NEXT   1
#define VIRTQ_DESC_F_WRITE  2

typedef struct PACKED { u32 type; u32 ioprio; u64 sector; } blk_req_hdr_t;
#define BLK_T_IN  0
#define BLK_T_OUT 1

/* modern common-config register offsets (within the common region) */
#define C_DEV_FEAT_SEL   0x00
#define C_DEV_FEAT       0x04
#define C_DRV_FEAT_SEL   0x08
#define C_DRV_FEAT       0x0C
#define C_MSIX_VECTOR    0x10
#define C_NUM_QUEUES     0x12
#define C_QUEUE_MSIX_VEC 0x1A
#define C_QUEUE_SEL      0x16
#define C_QUEUE_SIZE     0x18
#define C_QUEUE_ENABLE   0x1C
#define C_QUEUE_NOTIFY_OFF 0x1E
#define C_QUEUE_DESC     0x20
#define C_QUEUE_DRIVER   0x28
#define C_QUEUE_DEVICE   0x30
#define C_DEVICE_STATUS  0x14   /* Linux/QEMU modern common cfg layout */

#define ST_ACK 1
#define ST_DRIVER 2
#define ST_FEATURES_OK 8
#define ST_DRIVER_OK 4

#define FEAT_VERSION_1 (1ULL << 32)

/* PCI config access */
static u32 pci_cfg_read32(u8 bus, u8 dev, u8 fn, u8 off) {
    outl(0xCF8, (1U << 31) | ((u32)bus << 16) | ((u32)dev << 11)
              | ((u32)fn << 8) | (off & 0xFC));
    return inl(0xCFC);
}
static void pci_cfg_write32(u8 bus, u8 dev, u8 fn, u8 off, u32 val) {
    outl(0xCF8, (1U << 31) | ((u32)bus << 16) | ((u32)dev << 11)
              | ((u32)fn << 8) | (off & 0xFC));
    outl(0xCFC, val);
}
static u8 pci_cfg_read8(u8 bus, u8 dev, u8 fn, u8 off) {
    u32 v = pci_cfg_read32(bus, dev, fn, off & 0xFC);
    return (u8)(v >> ((off & 3) * 8));
}
static void pci_cfg_write16(u8 bus, u8 dev, u8 fn, u8 off, u16 val) {
    u32 w = pci_cfg_read32(bus, dev, fn, off & 0xFC);
    u32 shift = (off & 3) * 8;
    w = (w & ~(0xFFFFu << shift)) | ((u32)val << shift);
    pci_cfg_write32(bus, dev, fn, off & 0xFC, w);
}

static bool     g_present;
static u64      g_capacity_sectors;
static bool     g_modern;
static u8       g_irq_line;

/* legacy registers (I/O or MMIO at BAR0) */
static bool     g_mmio;
static u16      g_port_base;
static volatile u32 *g_regs;

/* modern capability regions: base + is-io flag per region */
typedef struct { u64 base; bool io; } vp_region_t;
static vp_region_t r_common, r_notify, r_isr, r_dev;
static u32         g_notify_mult;

static vq_desc_t  *g_desc;
static vq_avail_t *g_avail;
static vq_used_t  *g_used;
static paddr_t     g_desc_phys, g_avail_phys, g_used_phys;
static u16         g_last_used;

static blk_req_hdr_t *g_req;
static u8           *g_data;
static paddr_t       g_req_phys, g_data_phys;

static volatile bool g_io_done;
static volatile u32  g_irq_count;
static bool          g_uses_msix;
#define BLK_MSIX_VEC 61

static void cpu_relax(void) { __asm__ volatile("pause"); }

/* ---------------- legacy register accessors ---------------- */
static inline u32 vq_read(u32 off) {
    if (g_mmio) return g_regs[off / 4];
    return inl(g_port_base + off);
}
static inline void vq_write(u32 off, u32 val) {
    if (g_mmio) { g_regs[off / 4] = val; __asm__ volatile("mfence" ::: "memory"); }
    else        outl(g_port_base + off, val);
}
static inline u8 vq_read8(u32 off) {
    if (g_mmio) return *(volatile u8 *)((u8 *)g_regs + off);
    return inb(g_port_base + off);
}
static inline void vq_write8(u32 off, u8 val) {
    if (g_mmio) { *(volatile u8 *)((u8 *)g_regs + off) = val; __asm__ volatile("mfence" ::: "memory"); }
    else        outb(g_port_base + off, val);
}

/* ---------------- modern register accessors ---------------- */
static inline u32 mread32(vp_region_t *r, u32 off) {
    if (r->io) return inl((u16)(r->base + off));
    return *(volatile u32 *)phys_to_virt(r->base + off);
}
static inline void mwrite32(vp_region_t *r, u32 off, u32 val) {
    if (r->io) outl((u16)(r->base + off), val);
    else { *(volatile u32 *)phys_to_virt(r->base + off) = val;
           __asm__ volatile("mfence" ::: "memory"); }
}
static inline u16 mread16(vp_region_t *r, u32 off) {
    if (r->io) return inw((u16)(r->base + off));
    return *(volatile u16 *)phys_to_virt(r->base + off);
}
static inline void mwrite16(vp_region_t *r, u32 off, u16 val) {
    if (r->io) outw((u16)(r->base + off), val);
    else { *(volatile u16 *)phys_to_virt(r->base + off) = val;
           __asm__ volatile("mfence" ::: "memory"); }
}
static inline u8 mread8(vp_region_t *r, u32 off) {
    if (r->io) return inb((u16)(r->base + off));
    return *(volatile u8 *)phys_to_virt(r->base + off);
}
static inline void mwrite8(vp_region_t *r, u32 off, u8 val) {
    if (r->io) outb((u16)(r->base + off), val);
    else { *(volatile u8 *)phys_to_virt(r->base + off) = val;
           __asm__ volatile("mfence" ::: "memory"); }
}
static inline u64 mread64(vp_region_t *r, u32 off) {
    u32 lo = mread32(r, off);
    u32 hi = mread32(r, off + 4);
    return ((u64)hi << 32) | lo;
}
static inline void mwrite64(vp_region_t *r, u32 off, u64 val) {
    mwrite32(r, off, (u32)val);
    mwrite32(r, off + 4, (u32)(val >> 32));
}
static inline void notify_queue(void) {
    u32 off = (u32)(mread16(&r_common, C_QUEUE_NOTIFY_OFF) * g_notify_mult);
    if (r_notify.io) outw((u16)(r_notify.base + off), 0);
    else { *(volatile u16 *)phys_to_virt(r_notify.base + off) = 0;
           __asm__ volatile("mfence" ::: "memory"); }
}

/* ISR: called on the device's PCI IRQ; sets g_io_done + ACKs the line. */
void blk_irq_handler(cpu_regs_t *r) {
    (void)r;
    g_irq_count++;
    g_io_done = true;
    if (g_modern) mread8(&r_isr, 0);
    else          (void)vq_read8(VQ_ISR);
}
u8  blk_irq_line(void) { return g_irq_line; }
u32 blk_irq_count(void) { return g_irq_count; }

/* ---------------- the request path (shared by both modes) ------- */
static int vq_request(u32 type, u64 sector, void *data, u32 bytes) {
    if (!g_present) return -1;
    if (bytes == 0 || bytes % BLK_SECTOR_SIZE || bytes > 4 * KB(1)) return -1;

    if (type == BLK_T_OUT)
        memcpy(g_data, data, bytes);            /* bounce out              */

    g_req->type   = type;
    g_req->ioprio = 0;
    g_req->sector = sector;

    u16 head = (u16)(g_avail->idx & (VQ_SIZE - 1));
    g_desc[0] = (vq_desc_t){ g_req_phys,  sizeof(blk_req_hdr_t), VIRTQ_DESC_F_NEXT, 1 };
    g_desc[1] = (vq_desc_t){ g_data_phys, bytes,
                 (u16)((type == BLK_T_IN ? VIRTQ_DESC_F_WRITE : 0) | VIRTQ_DESC_F_NEXT), 2 };
    g_desc[2] = (vq_desc_t){ g_req_phys + 4080, 1, VIRTQ_DESC_F_WRITE, 0 };

    __asm__ volatile("mfence" ::: "memory");
    g_avail->ring[head] = 0;
    __asm__ volatile("mfence" ::: "memory");
    g_avail->idx++;

    g_io_done = false;
    if (g_modern) notify_queue();
    else          vq_write(VQ_QUEUE_NOTIFY, 0);

    u64 tries = 0;
    while (g_used->idx == g_last_used && !g_io_done) {
        if (++tries > 300000000ULL) return -1;
        cpu_relax();
    }
    u16 old = g_last_used;
    g_last_used = g_used->idx;

    u8 status = 0xFF;
    for (u16 s = old; s != g_last_used; s = (u16)(s + 1)) {
        u16 slot = s & (VQ_SIZE - 1);
        if (g_used->ring[slot].id == 0)
            status = *(volatile u8 *)((u8 *)g_req + 4080);
    }
    if (status != 0) return -1;
    if (type == BLK_T_IN)
        memcpy(data, g_data, bytes);            /* bounce in               */
    return 0;
}

int blk_read_sectors(u64 sector, u32 count, void *dst) {
    return vq_request(BLK_T_IN, sector, dst, count * BLK_SECTOR_SIZE);
}
int blk_write_sectors(u64 sector, u32 count, const void *src) {
    return vq_request(BLK_T_OUT, sector, (void *)src, count * BLK_SECTOR_SIZE);
}
bool blk_disk_present(void) { return g_present; }
u64  blk_disk_sectors(void) { return g_capacity_sectors; }

/* ---------------- legacy (0.9) bring-up ---------------- */
static bool legacy_setup(u8 bus, u8 dev, u8 fn, u32 bar0) {
    if (bar0 & 1) {
        g_mmio = false;
        g_port_base = (u16)(bar0 & ~3u);
        kprintf("blk: legacy I/O bar=0x%x\n", g_port_base);
    } else {
        g_mmio = true;
        g_regs = (volatile u32 *)phys_to_virt((paddr_t)(bar0 & ~0xFu));
        kprintf("blk: legacy mmio bar=0x%x\n", bar0 & ~0xFu);
    }
    vq_write8(VQ_STATUS, 1);
    vq_write8(VQ_STATUS, 3);
    u32 features = vq_read(VQ_HOST_FEAT);
    vq_write(VQ_GUEST_FEAT, 0);
    vq_write(VQ_QUEUE_SEL, 0);
    u16 qnum = (u16)vq_read(VQ_QUEUE_NUM);
    if (qnum < 2) { kprintf("blk: queue too small (%u)\n", qnum); return false; }
    vq_write(VQ_QUEUE_PFN, (u32)(g_desc_phys >> 12));
    vq_write8(VQ_STATUS, 7);                    /* ACK|DRIVER|DRIVER_OK    */
    u32 lo = vq_read(VQ_CONFIG);
    u32 hi = vq_read(VQ_CONFIG + 4);
    g_capacity_sectors = ((u64)hi << 32) | lo;
    kprintf("blk: legacy ready - %llu sectors (features=%x)\n",
            (unsigned long long)g_capacity_sectors, features);
    return true;
}

/* ---------------- MSI-X (interrupt delivery for modern virtio) ------- */
/* Modern virtio 1.0 devices deliver interrupts via MSI-X (a posted write
 * to the LAPIC).  We program table entry 0: destination = BSP LAPIC,
 * vector = 61, edge-triggered, unmasked, then enable MSI-X.  The generic
 * IRQ path EOIs via the LAPIC, so the ISR just sets g_io_done. */
bool blk_uses_msix(void) { return g_uses_msix; }

static bool msix_setup(u8 bus, u8 dev, u8 fn) {
    u8 cap = pci_cfg_read8(bus, dev, fn, 0x34) & ~3u;
    while (cap) {
        u8 id = pci_cfg_read8(bus, dev, fn, cap);
        if (id == 0xFF) break;
        if (id == 0x11) {                    /* MSI-X capability           */
            u32 tbl = pci_cfg_read32(bus, dev, fn, cap + 4);
            u8  bar = tbl & 0x7;
            u64 off = (u64)(tbl >> 3) << 3;  /* 8-byte aligned offset      */
            u32 barval = pci_cfg_read32(bus, dev, fn, 0x10 + bar * 4);
            u64 base = (u64)(barval & ~0xFu);
            if (((barval >> 1) & 3) == 2 && bar + 1 <= 5) {
                u32 hi = pci_cfg_read32(bus, dev, fn, 0x10 + (bar + 1) * 4);
                base |= (u64)hi << 32;
            }
            u64 table = base + off;
            for (u64 a = table & ~0xFFFULL; a < (table & ~0xFFFULL) + PAGE_SIZE;
                 a += PAGE_SIZE) {
                u64 virt = (u64)phys_to_virt(a);
                if (!vmm_translate(virt))
                    vmm_map(virt, a, PTE_PRESENT | PTE_RW);
            }
            volatile u32 *entry = (volatile u32 *)phys_to_virt(table);
            entry[0] = 0xFEE00000u | ((u32)apic_local_id() << 12);  /* addr */
            entry[1] = 0;                                            /* hi  */
            entry[2] = BLK_MSIX_VEC;                                 /* data */
            entry[3] = 0;                                            /* unmask */
            __asm__ volatile("mfence" ::: "memory");
            pci_cfg_write16(bus, dev, fn, cap + 2,
                            (u16)(pci_cfg_read32(bus, dev, fn, cap + 2) | 0x8000));
            irq_register(BLK_MSIX_VEC, blk_irq_handler);
            g_uses_msix = true;
            kprintf("blk: MSI-X enabled - vector %u (dest lapic %u)\n",
                    BLK_MSIX_VEC, apic_local_id());
            return true;
        }
        cap = pci_cfg_read8(bus, dev, fn, cap + 1) & ~3u;
    }
    return false;
}

/* ---------------- modern (1.0) bring-up ---------------- */
/* Map a device MMIO region into our page tables (the bootloader's HHDM
 * covers RAM but NOT PCI MMIO above 4G - e.g. QEMU puts the 64-bit BAR at
 * 0xC00000000).  Maps the same physical address at the HHDM offset so
 * phys_to_virt() in the accessors works. */
static void ensure_mmio_mapped(u64 base, u64 len) {
    u64 first = base & ~0xFFFULL;
    u64 last  = (base + len + 0xFFF) & ~0xFFFULL;
    for (u64 a = first; a < last; a += PAGE_SIZE) {
        u64 virt = (u64)phys_to_virt(a);   /* accessors use phys_to_virt */
        if (!vmm_translate(virt))
            vmm_map(virt, a, PTE_PRESENT | PTE_RW);
    }
}

static bool modern_caps(u8 bus, u8 dev, u8 fn) {
    bool found = false;
    u8 cap = pci_cfg_read8(bus, dev, fn, 0x34) & ~3u;
    while (cap) {
        u8 id = pci_cfg_read8(bus, dev, fn, cap);
        if (id == 0xFF) break;
        if (id == 0x09) {                    /* vendor-specific (virtio)  */
            u8 cfg_type = pci_cfg_read8(bus, dev, fn, cap + 3);
            u8 bar      = pci_cfg_read8(bus, dev, fn, cap + 4);
            u32 offset  = pci_cfg_read32(bus, dev, fn, cap + 8);
            u32 length  = pci_cfg_read32(bus, dev, fn, cap + 12);
            if (cfg_type >= 1 && cfg_type <= 4 && length) {
                u32 barval = pci_cfg_read32(bus, dev, fn, 0x10 + bar * 4);
                bool io    = (barval & 1) != 0;
                u64 base64 = (u64)(barval & ~0xFu);
                /* 64-bit BAR: the high 32 bits live in the next BAR slot */
                if (((barval >> 1) & 3) == 2 && bar + 1 <= 5) {
                    u32 hi = pci_cfg_read32(bus, dev, fn, 0x10 + (bar + 1) * 4);
                    base64 |= (u64)hi << 32;
                }
                u64 base = base64 + offset;
                vp_region_t *r = NULL;
                if      (cfg_type == VP_CAP_COMMON) r = &r_common;
                else if (cfg_type == VP_CAP_NOTIFY) { r = &r_notify;
                    g_notify_mult = pci_cfg_read32(bus, dev, fn, cap + 16); }
                else if (cfg_type == VP_CAP_ISR)    r = &r_isr;
                else if (cfg_type == VP_CAP_DEVICE) r = &r_dev;
                if (r) { r->base = base; r->io = io; }
                found = true;
            }
        }
        cap = pci_cfg_read8(bus, dev, fn, cap + 1) & ~3u;
    }
    return found;
}

static bool modern_setup(u8 bus, u8 dev, u8 fn) {
    r_common = (vp_region_t){0,false};
    r_notify = (vp_region_t){0,false};
    r_isr    = (vp_region_t){0,false};
    r_dev    = (vp_region_t){0,false};
    if (!modern_caps(bus, dev, fn)) { kprintf("blk: mdbg no caps\n"); return false; }
    if (!r_common.base || !r_notify.base || !r_dev.base) {
        kprintf("blk: modern caps incomplete (c=%llx n=%llx d=%llx)\n",
                (unsigned long long)r_common.base,
                (unsigned long long)r_notify.base,
                (unsigned long long)r_dev.base);
        return false;
    }
    if (!r_common.io) ensure_mmio_mapped(r_common.base, 0x1000);
    if (!r_notify.io) ensure_mmio_mapped(r_notify.base, 0x1000);
    if (!r_isr.io)    ensure_mmio_mapped(r_isr.base, 0x1000);
    if (!r_dev.io)    ensure_mmio_mapped(r_dev.base, 0x1000);

    mwrite8(&r_common, C_DEVICE_STATUS, 0);       /* reset                 */
    mwrite8(&r_common, C_DEVICE_STATUS, ST_ACK);
    mwrite8(&r_common, C_DEVICE_STATUS, ST_ACK | ST_DRIVER);

    u64 devfeat = 0;
    mwrite32(&r_common, C_DEV_FEAT_SEL, 0); devfeat  = mread32(&r_common, C_DEV_FEAT);
    mwrite32(&r_common, C_DEV_FEAT_SEL, 1); devfeat |= (u64)mread32(&r_common, C_DEV_FEAT) << 32;
    if (!(devfeat & FEAT_VERSION_1)) {
        kprintf("blk: device lacks VERSION_1\n");
        return false;
    }
    mwrite32(&r_common, C_DRV_FEAT_SEL, 0); mwrite32(&r_common, C_DRV_FEAT, 0);
    mwrite32(&r_common, C_DRV_FEAT_SEL, 1); mwrite32(&r_common, C_DRV_FEAT, (u32)(FEAT_VERSION_1 >> 32));

    mwrite8(&r_common, C_DEVICE_STATUS, ST_ACK | ST_DRIVER | ST_FEATURES_OK);
    u8 st = mread8(&r_common, C_DEVICE_STATUS);
    if (!(st & ST_FEATURES_OK)) {
        kprintf("blk: FEATURES_OK rejected\n");
        return false;
    }
    mwrite8(&r_common, C_DEVICE_STATUS, ST_ACK | ST_DRIVER | ST_FEATURES_OK | ST_DRIVER_OK);

    mwrite16(&r_common, C_QUEUE_SEL, 0);
    u16 qnum = mread16(&r_common, C_QUEUE_SIZE);
    if (qnum < 2) { kprintf("blk: modern queue too small (%u)\n", qnum); return false; }
    mwrite64(&r_common, C_QUEUE_DESC,   g_desc_phys);
    mwrite64(&r_common, C_QUEUE_DRIVER, g_avail_phys);
    mwrite64(&r_common, C_QUEUE_DEVICE, g_used_phys);
    /* use MSI-X entry 0 for queue + config interrupts (0xFFFF = INTx) */
    mwrite16(&r_common, C_MSIX_VECTOR, 0);
    mwrite16(&r_common, C_QUEUE_MSIX_VEC, 0);
    mwrite16(&r_common, C_QUEUE_ENABLE, 1);

    g_capacity_sectors = mread64(&r_dev, 0);
    kprintf("blk: modern (virtio 1.0) ready - %llu sectors (feat=%llx)\n",
            (unsigned long long)g_capacity_sectors, (unsigned long long)devfeat);
    return true;
}

/* ---------------- probe + init ---------------- */
void blk_init(void) {
    if (g_present) return;
    int found = -1;
    u8 fbus = 0, fdev = 0, ffn = 0;
    for (int bus = 0; bus < 4 && found < 0; bus++)
        for (int d = 0; d < 32 && found < 0; d++) {
            u16 vendor = pci_cfg_read32(bus, d, 0, 0x00) & 0xFFFF;
            if (vendor != 0x1AF4) continue;
            u16 device = (pci_cfg_read32(bus, d, 0, 0x00) >> 16) & 0xFFFF;
            if (device == 0x1001 || device == 0x1042) {
                u32 cls = pci_cfg_read32(bus, d, 0, 0x08);
                if (((cls >> 24) & 0xFF) != 0x01) continue;   /* storage */
                found = 1; fbus = bus; fdev = d; ffn = 0;
            }
        }
    if (found < 0) {
        kprintf("blk: no virtio-blk device found - RAM-only filesystem\n");
        return;
    }

    u32 cmd = pci_cfg_read32(fbus, fdev, ffn, 0x04);
    pci_cfg_write32(fbus, fdev, ffn, 0x04, cmd | 0x7);   /* io+mem+busmaster */
    g_irq_line = (u8)(pci_cfg_read32(fbus, fdev, ffn, 0x3C) & 0xFF);
    kprintf("blk: virtio-blk %02x:%02x.%u irq=%u\n", fbus, fdev, ffn, g_irq_line);

    paddr_t vq_p = pmm_alloc_pages(5);
    g_desc_phys  = vq_p;
    g_avail_phys = vq_p + PAGE_SIZE;
    g_used_phys  = vq_p + 2 * PAGE_SIZE;
    g_req_phys   = vq_p + 3 * PAGE_SIZE;
    g_data_phys  = vq_p + 4 * PAGE_SIZE;
    g_desc  = phys_to_virt(g_desc_phys);
    g_avail = phys_to_virt(g_avail_phys);
    g_used  = phys_to_virt(g_used_phys);
    g_req   = phys_to_virt(g_req_phys);
    g_data  = phys_to_virt(g_data_phys);
    memset(g_desc, 0, PAGE_SIZE);
    memset(g_avail, 0, PAGE_SIZE);
    memset(g_used, 0, PAGE_SIZE);
    g_last_used = 0;

    u32 bar0 = pci_cfg_read32(fbus, fdev, ffn, 0x10);
    if (modern_setup(fbus, fdev, ffn)) {
        g_modern = true;
        msix_setup(fbus, fdev, ffn);   /* interrupt-driven modern delivery */
    } else {
        kprintf("blk: falling back to legacy (0.9) interface\n");
        g_modern = false;
        if (!legacy_setup(fbus, fdev, ffn, bar0)) {
            kprintf("blk: legacy setup failed - RAM-only\n");
            pmm_free_pages(vq_p, 5);
            return;
        }
    }
    g_present = true;
    kprintf("blk: virtio-blk ready (%s mode) - %llu KiB\n",
            g_modern ? "modern" : "legacy",
            (unsigned long long)(g_capacity_sectors * BLK_SECTOR_SIZE / 1024));
}
