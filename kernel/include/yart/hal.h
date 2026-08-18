#pragma once
#include <yart/types.h>
#include <yart/cpu.h>   /* cpu_local_t (per-CPU GDT/TSS helpers) */

/* GDT / TSS */
void gdt_init(void);
void gdt_reload(void);   /* re-load this CPU's GDT+TSS */
void smp_ap_switch_gdt(u32 cpu_index); /* AP-safe: lgdt(own GDT) + far-jump to 0x08 */
void ap_install_tss(u32 cpu_index, u64 rsp0); /* per-AP TSS + TR (0x38) */
void tss_set_rsp0(u64 rsp0);   /* sets THIS CPU's TSS RSP0 (per-CPU) */
u64  tss_get_rsp0(void);
cpu_local_t *bsp_cpu_local(void); /* the BSP's per-CPU area (id 0) */

/* TLB shootdown (SMP): invalidate a VA - or flush the whole TLB - on every
 * online CPU (self included) and wait until all have acked.  Needed
 * whenever shared kernel page tables (the direct map / kstack guard pages)
 * or a PML4 that may be loaded on another CPU right now (wm surfaces) are
 * modified.  Callers must NOT hold a spinlock an AP could be spinning on
 * with IRQs off, and must not be in an interrupt handler. */
void smp_tlb_shootdown(u64 va);       /* single-page invalidate, all CPUs */
void smp_tlb_shootdown_all(void);     /* full flush, all CPUs             */
bool smp_tlb_selftest(void);          /* boot selftest (needs APs online) */

/* IDT / interrupts */
typedef struct PACKED {
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rdi, rsi, rbp, rdx, rcx, rbx, rax;
    u64 vector, err;
    u64 rip, cs, rflags, rsp, ss;
} cpu_regs_t;

typedef void (*irq_handler_t)(cpu_regs_t *r);

void idt_init(void);
void idt_reload(void);   /* re-load IDT on this CPU */
void irq_register(u8 irq, irq_handler_t h);
void apic_route_irq(u8 irq, u8 vector, bool masked);  /* IOAPIC INTx route */
/* Called from asm; returns the RSP the ISR stub must resume from (a task
 * switch returns a different task's cpu_regs_t frame). */
u64 isr_dispatch(cpu_regs_t *r);

/* PIC */
void pic_remap(int offset1, int offset2);
void pic_mask(u8 irq);
void pic_unmask(u8 irq);
void pic_eoi(u8 irq);
void pic_eoi_careful(u8 irq);   /* spurious-IRQ aware EOI */
void pic_disable_all(void);     /* stop the legacy 8259s */

/* APIC / IOAPIC / APIC timer (apic.c).  Falls back to PIC + PIT. */
bool apic_available(void);
void apic_init(void);
bool apic_active(void);
void interrupt_eoi(u8 vector);          /* LAPIC or (careful) PIC EOI */
u8   apic_local_id(void);            /* BSP LAPIC id (MSI-X targeting)  */
void lapic_start_ap(u32 dest_apic, u32 tramp_page); /* INIT-SIPI-SIPI     */
void lapic_send_ipi(u32 dest_apic, u8 vector);      /* fixed IPI          */
u32  lapic_read_id_reg(void);
void apic_timer_start_this_cpu(void);   /* per-CPU APIC timer */
void lapic_spurious_irq(cpu_regs_t *r); /* spurious vector handler    */

/* PIT / time */
void pit_init(u32 hz);
u64  pit_ticks(void);
void sleep_ms(u32 ms);

/* RTC */
typedef struct {
    u8 second, minute, hour;
    u8 day, month;
    u16 year;
} rtc_time_t;
void rtc_read(rtc_time_t *t);
