/* Yart OS - Local APIC + IOAPIC + APIC timer.
 *
 *  1. acpi.c parses the MADT (LAPIC/IOAPIC addresses + ISA IRQ overrides).
 *  2. apic_init() masks the legacy PICs FIRST (a QEMU/TCG quirk: enabling the
 *     LAPIC while the PIC can still deliver an IRQ, e.g. the PIT's IRQ0,
 *     makes QEMU inject a bogus 0xffffffff ExtINT whose IDT gate check then
 *     #GPs), enables the LAPIC, programs IOAPIC redirections for the IRQs we
 *     use, calibrates the LAPIC timer by polling the PIT counter register
 *     (no interrupts needed), and switches the system tick to the APIC timer.
 *  3. Any failure restores the PIT and falls back to the legacy PIC path.
 */
#include <yart/types.h>
#include <yart/console.h>
#include <yart/string.h>
#include <yart/mm.h>
#include <yart/io.h>
#include <yart/hal.h>
#include <yart/cpu.h>
#include <yart/acpi.h>
#include <yart/blk.h>

#define IA32_APIC_BASE_MSR 0x1B
#define APIC_BASE_ENABLE   (1ULL << 11)
#define APIC_BASE_BSP      (1ULL << 8)

/* LAPIC registers */
#define LAPIC_ID            0x020
#define LAPIC_VER           0x030
#define LAPIC_TPR           0x080
#define LAPIC_EOI           0x0B0
#define LAPIC_SVR           0x0F0
#define LAPIC_ESR           0x280
#define LAPIC_LVT_TIMER     0x320
#define LAPIC_LVT_THERMAL   0x330
#define LAPIC_LVT_PERF      0x340
#define LAPIC_LVT_LINT0     0x350
#define LAPIC_LVT_LINT1     0x360
#define LAPIC_LVT_ERROR     0x370
#define LAPIC_TIMER_ICR     0x380
#define LAPIC_TIMER_CCR     0x390
#define LAPIC_TIMER_DIV     0x3E0

#define LVT_MASK       (1u << 16)
#define LVT_PERIODIC   (1u << 17)
#define LVT_ACTIVE_LOW (1u << 13)
#define LVT_LEVEL      (1u << 15)
#define DIV_BY_16      0x3

/* IOAPIC registers */
#define IOAPIC_IOREGSEL 0x00
#define IOAPIC_IOWIN    0x10
#define IOAPIC_VER      0x01
#define IOAPIC_REDTBL   0x10

#define SPURIOUS_VECTOR  0xFF
#define APIC_TIMER_VECTOR 48
#define IRQ_VEC_BASE     32
#define IRQ_TIMER_VEC    32    /* PIT slot while it is the time source */

#define PIT_HZ 100

/* PIT channel 0 ports (for IRQ-free counter polling during calibration) */
#define PIT_CH0 0x40
#define PIT_CMD 0x43

static volatile u32 *lapic_mmio;
static volatile u32 *ioapic_mmio;
static u32 ioapic_gsi_base;
static u32 ioapic_redirs;
static u32 lapic_id;
static u64 lapic_bus_hz;

bool g_apic_active = false;

u8 apic_local_id(void) { return (u8)lapic_id; }


extern void yart_timer_irq(cpu_regs_t *r);   /* pit.c: ticks++ */

static inline u32 lapic_read(u32 reg)  { return lapic_mmio[reg / 4]; }
static inline void lapic_write(u32 reg, u32 val) { lapic_mmio[reg / 4] = val; }


/* ---- IPI delivery via the LAPIC ICR ---- */
#define LAPIC_ICR 0x300
#define ICR_INIT        (0x500u)      /* INIT, edge, physical           */
#define ICR_STARTUP     (0x600u)      /* STARTUP, edge, physical        */
#define ICR_DEST_FIXED  (0x400u)      /* fixed delivery                 */

static void lapic_send_icr(u32 dest_apic, u32 icr_lo) {
    /* wait for previous IPI to clear (with a timeout so a stuck delivery
     * can't hang the whole OS) */
    u64 tries = 0;
    while (lapic_read(LAPIC_ICR) & (1u << 12)) {
        if (++tries > 100000000ULL) { kprintf("apic: ICR send timeout\n"); break; }
        __asm__ volatile("pause");
    }
    /* xAPIC ICR is two 32-bit halves: the destination APIC id lives in the
     * HIGH dword (0x310, bits 63:56), the vector/flags in the LOW (0x300).
     * Writing dest into the low dword would put it in the ignored extended-
     * destination bits and silently deliver every IPI to the BSP. */
    lapic_write(LAPIC_ICR + 0x10, (u32)dest_apic << 24);   /* ICRH: dest   */
    lapic_write(LAPIC_ICR, icr_lo);                        /* ICRL: vector */
    __asm__ volatile("mfence" ::: "memory");
}

/* INIT-SIPI-SIPI startup sequence for an AP.  `tramp_page` is the 4K page
 * (physical address) holding the trampoline (must be below 1 MiB). */
/* INIT-SIPI-SIPI: INIT level-assert, INIT level-deassert, 10 ms, then two
 * STARTUP IPIs.  This exact sequence is what QEMU/real hardware expect -
 * a plain edge INIT can reset the whole machine. */
void lapic_start_ap(u32 dest_apic, u32 tramp_page) {
    u32 vec = (tramp_page >> 12) & 0xFF;
    kprintf("apic: BSP lapic id = %u, sending INIT-assert to lapic %u (icr=0x%x)\n",
            (unsigned)apic_local_id(), dest_apic,
            ((u32)dest_apic << 24) | 0x0C500u);
    lapic_write(LAPIC_ICR, ((u32)dest_apic << 24) | 0x0C500u);
    for (volatile u64 i = 0; i < 2000000ULL; i++) __asm__ volatile("pause");
    kprintf("apic: INIT-deassert\n");
    lapic_write(LAPIC_ICR, ((u32)dest_apic << 24) | 0x08500u);
    for (volatile u64 i = 0; i < 20000000ULL; i++) __asm__ volatile("pause");
    kprintf("apic: STARTUP lapic %u vec 0x%x\n", dest_apic, vec);
    /* Some QEMU/TCG versions reset the machine on an INIT IPI from guest
     * code; SIPI alone reaches an AP that is already waiting in real mode
     * (which OVMF leaves them in), so we try SIPI-SIPI directly. */
    lapic_send_icr(dest_apic, 0x600u | vec);
    __asm__ volatile("mfence" ::: "memory");
    for (volatile u64 i = 0; i < 200000ULL; i++) __asm__ volatile("pause");
    lapic_send_icr(dest_apic, 0x600u | vec);
}

/* Send a reschedule IPI to another CPU (destination LAPIC id). */
void lapic_send_ipi(u32 dest_apic, u8 vector) {
    lapic_send_icr(dest_apic, ICR_DEST_FIXED | vector);
}
u32 lapic_read_id_reg(void) { return lapic_read(LAPIC_ID); }
static inline u32 ioapic_read(u8 reg) {
    ioapic_mmio[IOAPIC_IOREGSEL / 4] = reg;
    return ioapic_mmio[IOAPIC_IOWIN / 4];
}
static inline void ioapic_write(u8 reg, u32 val) {
    ioapic_mmio[IOAPIC_IOREGSEL / 4] = reg;
    ioapic_mmio[IOAPIC_IOWIN / 4] = val;
}

static bool lapic_present(void) {
    u32 a, b, c, d;
    cpuid_id(1, &a, &b, &c, &d);
    return (d >> 9) & 1;
}

static bool lapic_enable(u32 phys_addr) {
    if (!lapic_present()) return false;
    u64 base = rdmsr64(IA32_APIC_BASE_MSR);
    wrmsr64(IA32_APIC_BASE_MSR, base | APIC_BASE_ENABLE);
    lapic_mmio = (volatile u32 *)phys_to_virt((paddr_t)phys_addr);
    if (lapic_read(LAPIC_VER) == 0) {
        kprintf("apic: LAPIC MMIO dead at 0x%x\n", phys_addr);
        return false;
    }
    /* software enable + spurious vector, clear pending errors */
    lapic_write(LAPIC_SVR, (1u << 8) | SPURIOUS_VECTOR);
    lapic_write(LAPIC_TPR, 0);
    lapic_write(LAPIC_ESR, 0);
    (void)lapic_read(LAPIC_ESR);
    lapic_id = lapic_read(LAPIC_ID) >> 24;
    u32 ver = lapic_read(LAPIC_VER) & 0xFF;
    kprintf("apic: LAPIC id=%u ver=0x%x mmio=%p\n", lapic_id, ver, lapic_mmio);
    return true;
}

static void lapic_eoi(void) {
    if (lapic_mmio) lapic_write(LAPIC_EOI, 0);
}

/* Spurious LAPIC interrupt (vector 0xFF): nothing to EOI. */
void lapic_spurious_irq(cpu_regs_t *r) { (void)r; }

static void lapic_mask_ext_ints(void) {
    /* masked entries still get a sane vector (never 0) */
    lapic_write(LAPIC_LVT_LINT0,   LVT_MASK | SPURIOUS_VECTOR);
    lapic_write(LAPIC_LVT_LINT1,   LVT_MASK | SPURIOUS_VECTOR);
    lapic_write(LAPIC_LVT_THERMAL, LVT_MASK | SPURIOUS_VECTOR);
    lapic_write(LAPIC_LVT_PERF,    LVT_MASK | SPURIOUS_VECTOR);
    lapic_write(LAPIC_LVT_ERROR,   LVT_MASK | SPURIOUS_VECTOR);
    lapic_write(LAPIC_LVT_TIMER,   LVT_MASK | APIC_TIMER_VECTOR);
}

/* ---- IOAPIC ---- */
static u32 gsi_of(u8 irq) {
    for (int i = 0; i < g_madt.override_count; i++)
        if (g_madt.override[i].irq == irq) return g_madt.override[i].gsi;
    return irq;
}
static u16 flags_of(u8 irq) {
    for (int i = 0; i < g_madt.override_count; i++)
        if (g_madt.override[i].irq == irq) return g_madt.override[i].flags;
    return 0;
}

static void route_irq(u8 irq, u8 vector, bool masked) {
    u32 gsi = gsi_of(irq);
    if (gsi < ioapic_gsi_base || gsi >= ioapic_gsi_base + ioapic_redirs) {
        kprintf("apic: IRQ%u -> gsi %u out of range\n", irq, gsi);
        return;
    }
    u16 fl = flags_of(irq);
    u32 lo = (u32)vector
           | ((fl & 1) ? LVT_ACTIVE_LOW : 0)
           | ((fl & 2) ? LVT_LEVEL      : 0)
           | (masked  ? LVT_MASK        : 0);
    u32 hi = lapic_id << 24;              /* physical mode, dest = BSP */
    u32 idx = IOAPIC_REDTBL + 2 * (gsi - ioapic_gsi_base);
    ioapic_write((u8)idx, lo);
    ioapic_write((u8)(idx + 1), hi);
    kprintf("apic: ioapic IRQ%u -> gsi%u vec%u %s\n",
            irq, gsi, vector, masked ? "masked" : "live");
}
void apic_route_irq(u8 irq, u8 vector, bool masked) { route_irq(irq, vector, masked); }

static bool ioapic_init(u32 phys_addr, u32 gsi_base) {
    if (!phys_addr) return false;
    ioapic_mmio = (volatile u32 *)phys_to_virt((paddr_t)phys_addr);
    u32 ver = ioapic_read(IOAPIC_VER);
    ioapic_redirs = ((ver >> 16) & 0xFF) + 1;
    ioapic_gsi_base = gsi_base;
    if (ioapic_redirs < 1 || ioapic_redirs > 120) {
        kprintf("apic: IOAPIC bogus version 0x%x\n", ver);
        return false;
    }
    kprintf("apic: IOAPIC @0x%x ver=0x%x redirs=%u gsi_base=%u\n",
            phys_addr, ver, ioapic_redirs, ioapic_gsi_base);
    /* mask everything first */
    for (u32 i = 0; i < ioapic_redirs; i++) {
        u32 idx = IOAPIC_REDTBL + 2 * i;
        ioapic_write((u8)idx, LVT_MASK);
        ioapic_write((u8)(idx + 1), 0);
    }
    /* route what we use (vector = 32 + irq keeps the PIC-era mapping) */
    route_irq(0,  IRQ_TIMER_VEC, true);     /* PIT: masked, APIC timer replaces it */
    route_irq(1,  IRQ_VEC_BASE + 1, false);   /* PS/2 keyboard */
    route_irq(12, IRQ_VEC_BASE + 12, false);  /* PS/2 mouse   */
    /* virtio-blk: interrupt-driven completion.  PCI IRQs usually sit at
     * GSI >= 16; map them to vectors 49..63 so they never collide with the
     * timer (48) or the syscall gate (0x80). */
    {
        if (blk_uses_msix()) {
            kprintf("apic: virtio-blk uses MSI-X (no INTx route needed)\n");
        }
        u8 bq = blk_irq_line();
        if (bq && !blk_uses_msix()) {
            /* PCI IRQs map to the reserved vector range 49..62 (never the
             * mouse's 44 / kbd's 33 / timer's 48 / syscall 0x80) */
            u8 vec = (u8)(49 + (bq & 0xF));
            if (vec >= 64) vec = 62;
            if (vec < 64) {
                route_irq(bq, vec, false);
                irq_register(vec, blk_irq_handler);
                kprintf("apic: virtio-blk irq %u -> vec %u (interrupt-driven)\n",
                        bq, vec);
            }
        }
    }
    return true;
}

/* ---- APIC timer ---- */

/* Latch and read the PIT channel-0 counter without interrupts.  We can't
 * rely on the PIT IRQ during calibration: the PIC is already masked (and
 * QEMU/TCG must not deliver legacy IRQs while the LAPIC is live). */
static u16 pit_counter_read(void) {
    outb(PIT_CMD, 0x00);                    /* counter 0, latch */
    u16 lo = inb(PIT_CH0);
    u16 hi = inb(PIT_CH0);
    return (u16)(lo | (hi << 8));
}

static bool lapic_timer_calibrate(void) {
    lapic_write(LAPIC_TIMER_DIV, DIV_BY_16);
    lapic_write(LAPIC_TIMER_ICR, 0);
    lapic_write(LAPIC_LVT_TIMER, LVT_MASK | APIC_TIMER_VECTOR);  /* one-shot */
    lapic_write(LAPIC_TIMER_ICR, 0xFFFFFFFFu);
    /* wait for ~2 PIT counter reloads (PIT @100Hz -> ~20 ms), IRQ-free */
    u16 last = pit_counter_read();
    int wraps = 0;
    while (wraps < 2) {
        u16 now = pit_counter_read();
        if (now > last) wraps++;    /* counter wrapped back up = one tick */
        last = now;
    }
    u32 cur = lapic_read(LAPIC_TIMER_CCR);
    u32 delta = 0xFFFFFFFFu - cur;
    if (delta < 1000u) {
        kprintf("apic: timer did not run (delta=%u)\n", delta);
        return false;
    }
    /* counter ran at bus/16 for 20 ms -> bus = delta * 16 * 50 */
    lapic_bus_hz = (u64)delta * 16ULL * 50ULL;
    kprintf("apic: timer calibration delta=%u/20ms -> bus ~%llu Hz\n",
            delta, (unsigned long long)lapic_bus_hz);
    return true;
}

static void lapic_timer_start_periodic(u32 hz) {
    u64 count = (lapic_bus_hz / 16ULL) / hz;
    if (count == 0) count = 1;
    if (count > 0xFFFFFFFFULL) count = 0xFFFFFFFFULL;
    lapic_write(LAPIC_TIMER_ICR, 0);
    lapic_write(LAPIC_TIMER_DIV, DIV_BY_16);
    lapic_write(LAPIC_LVT_TIMER, APIC_TIMER_VECTOR | LVT_PERIODIC);  /* unmask */
    lapic_write(LAPIC_TIMER_ICR, (u32)count);
    kprintf("apic: timer vec%d periodic ~%u Hz (count=%u)\n",
            APIC_TIMER_VECTOR, hz, (u32)count);
}

/* Start the APIC timer on the CURRENT CPU (BSP or AP).  The BSP is
 * calibrated in apic_init(); an AP calibrates lazily here using the
 * BSP-derived bus frequency. */
void apic_timer_start_this_cpu(void) {
    if (!lapic_mmio) return;
    u64 count = (lapic_bus_hz / 16ULL) / PIT_HZ;
    if (count == 0) count = 1;
    if (count > 0xFFFFFFFFULL) count = 0xFFFFFFFFULL;
    lapic_write(LAPIC_TIMER_DIV, DIV_BY_16);
    lapic_write(LAPIC_LVT_TIMER, APIC_TIMER_VECTOR | LVT_PERIODIC);
    lapic_write(LAPIC_TIMER_ICR, (u32)count);
}

/* ---- public interface ---- */
void interrupt_eoi(u8 vector) {
    if (g_apic_active) { lapic_eoi(); return; }
    u8 irq = vector - IRQ_VEC_BASE;
    if (irq < 16) pic_eoi_careful(irq);
    else          pic_eoi(irq);
}

bool apic_active(void) { return g_apic_active; }

void apic_init(void) {
    if (!g_madt.present) {
        kprintf("apic: no MADT - staying on 8259 PIC\n");
        return;
    }
    if (!lapic_present()) {
        kprintf("apic: CPU has no LAPIC - staying on 8259 PIC\n");
        return;
    }

    /* The PIC must be silent before the LAPIC comes up: with both live,
     * QEMU/TCG turns a legacy IRQ (e.g. the PIT's IRQ0) into a bogus
     * 0xffffffff ExtINT whose IDT gate check #GPs.  If anything below
     * fails we re-enable the PIT IRQ and stay on the PIC. */
    pic_disable_all();

    if (!lapic_enable(g_madt.lapic_addr)) {
        pic_unmask(0);
        kprintf("apic: LAPIC init failed - staying on 8259 PIC\n");
        return;
    }
    lapic_mask_ext_ints();
    irq_register(SPURIOUS_VECTOR, lapic_spurious_irq);

    if (!ioapic_init(g_madt.ioapic_addr, g_madt.ioapic_gsi_base)) {
        pic_unmask(0);
        kprintf("apic: IOAPIC init failed - staying on 8259 PIC\n");
        return;
    }

    if (!lapic_timer_calibrate()) {
        pic_unmask(0);
        kprintf("apic: timer calibration failed - staying on PIT\n");
        return;
    }

    /* commit: switch interrupt delivery to LAPIC/IOAPIC + APIC timer */
    g_apic_active = true;
    irq_register(APIC_TIMER_VECTOR, yart_timer_irq);
    lapic_timer_start_periodic(PIT_HZ);
    kprintf("apic: interrupt delivery switched to LAPIC/IOAPIC + APIC timer\n");
}
