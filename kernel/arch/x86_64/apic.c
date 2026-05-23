/* Yart OS - Local APIC + IOAPIC stubs.
 *
 * The kernel boots fine on the legacy 8259 PIC; this file is the seed for
 * SMP and a per-CPU timer.  We expose three primitives:
 *   - lapic_present()        : CPUID feature check
 *   - lapic_enable_xapic()   : map the LAPIC MMIO page (HHDM) and enable
 *                              software via the spurious-vector register
 *   - lapic_eoi()            : signal end-of-interrupt
 *
 * No IDT slots are touched here.  Once we discover the IOAPIC via MADT we
 * can move IRQ routing off the 8259 and free up cleaner vectors.
 */
#include <yart/types.h>
#include <yart/console.h>
#include <yart/mm.h>

#define IA32_APIC_BASE_MSR 0x1B

#define LAPIC_REG_ID         0x020
#define LAPIC_REG_VERSION    0x030
#define LAPIC_REG_TPR        0x080
#define LAPIC_REG_EOI        0x0B0
#define LAPIC_REG_SVR        0x0F0   /* spurious-interrupt vector */

static volatile u32 *lapic_mmio;

static inline u64 rdmsr(u32 m) {
    u32 lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(m));
    return ((u64)hi << 32) | lo;
}
static inline void wrmsr(u32 m, u64 v) {
    __asm__ volatile ("wrmsr" :: "a"((u32)v), "d"((u32)(v >> 32)), "c"(m));
}
static inline void cpuid(u32 leaf, u32 *a, u32 *b, u32 *c, u32 *d) {
    __asm__ volatile ("cpuid"
        : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
        : "a"(leaf), "c"(0));
}

bool lapic_present(void) {
    u32 a,b,c,d; cpuid(1, &a,&b,&c,&d);
    return (d >> 9) & 1;
}

void lapic_enable_xapic(void) {
    if (!lapic_present()) {
        kprintf("apic: LAPIC not present\n");
        return;
    }
    u64 base = rdmsr(IA32_APIC_BASE_MSR);
    paddr_t phys = base & ~0xFFFULL;
    /* enable globally */
    wrmsr(IA32_APIC_BASE_MSR, base | (1 << 11));
    lapic_mmio = (volatile u32 *)phys_to_virt(phys);
    /* enable software via SVR (set bit 8, vector 0xFF for spurious) */
    lapic_mmio[LAPIC_REG_SVR / 4] = (1 << 8) | 0xFF;
    u32 id  = lapic_mmio[LAPIC_REG_ID / 4] >> 24;
    u32 ver = lapic_mmio[LAPIC_REG_VERSION / 4] & 0xFF;
    kprintf("apic: LAPIC id=%u ver=0x%x mmio=%p\n", id, ver, lapic_mmio);
}

void lapic_eoi(void) {
    if (lapic_mmio) lapic_mmio[LAPIC_REG_EOI / 4] = 0;
}
