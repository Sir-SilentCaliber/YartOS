/* Yart OS - IDT, ISR/IRQ dispatch, exception panic screen */
#include <yart/types.h>
#include <yart/hal.h>
#include <yart/console.h>
#include <yart/string.h>
#include <yart/io.h>

typedef struct PACKED {
    u16 off_lo;
    u16 sel;
    u8  ist;
    u8  type_attr;
    u16 off_mid;
    u32 off_hi;
    u32 zero;
} idt_entry_t;

typedef struct PACKED { u16 limit; u64 base; } idtr_t;

static idt_entry_t idt[256];
static idtr_t      idtr;
static irq_handler_t handlers[256];

extern u64 isr_stub_table[];   /* from isr.asm */

static void set_gate(int n, u64 handler, u8 ist, u8 type_attr) {
    idt[n].off_lo = handler & 0xFFFF;
    idt[n].sel    = 0x08;
    idt[n].ist    = ist;
    idt[n].type_attr = type_attr;
    idt[n].off_mid = (handler >> 16) & 0xFFFF;
    idt[n].off_hi  = (handler >> 32);
    idt[n].zero    = 0;
}

void irq_register(u8 v, irq_handler_t h) { handlers[v] = h; }

static const char *exc_names[32] = {
    "Divide-by-zero", "Debug", "NMI", "Breakpoint",
    "Overflow", "Bound Range Exceeded", "Invalid Opcode", "Device Not Available",
    "Double Fault", "Coproc Seg Overrun", "Invalid TSS", "Segment Not Present",
    "Stack-Segment Fault", "General Protection", "Page Fault", "Reserved",
    "x87 FP", "Alignment Check", "Machine Check", "SIMD FP",
    "Virtualization", "Control Protection", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Hypervisor Injection", "VMM Comm", "Security", "Reserved"
};

void isr_dispatch(cpu_regs_t *r) {
    if (r->vector < 32) {
        cli();
        kprintf("\n=== CPU EXCEPTION ===\n");
        kprintf("vec=%lu (%s)  err=0x%lx\n",
                r->vector,
                r->vector < 32 ? exc_names[r->vector] : "?",
                r->err);
        kprintf("RIP=%p  CS=0x%lx  RFLAGS=0x%lx\n",
                (void *)r->rip, r->cs, r->rflags);
        kprintf("RSP=%p  SS=0x%lx\n", (void *)r->rsp, r->ss);
        kprintf("RAX=%p RBX=%p RCX=%p RDX=%p\n",
                (void *)r->rax, (void *)r->rbx,
                (void *)r->rcx, (void *)r->rdx);
        kprintf("RSI=%p RDI=%p RBP=%p\n",
                (void *)r->rsi, (void *)r->rdi, (void *)r->rbp);
        if (r->vector == 14) kprintf("CR2=%p\n", (void *)read_cr2());
        kpanic("Unhandled CPU exception #%lu (%s)",
               r->vector,
               r->vector < 32 ? exc_names[r->vector] : "unknown");
    }

    if (handlers[r->vector]) handlers[r->vector](r);

    /* IRQs land at 32..47 (after PIC remap) */
    if (r->vector >= 32 && r->vector < 48) {
        pic_eoi(r->vector - 32);
    }
}

void idt_init(void) {
    memset(idt, 0, sizeof idt);
    memset(handlers, 0, sizeof handlers);
    for (int i = 0; i < 256; i++) {
        u8 ist = (i == 8 || i == 14) ? 1 : 0;   /* DF, PF on IST1 */
        u8 attr = (i == 0x80) ? 0xEE : 0x8E;     /* DPL=3 for user syscall */
        set_gate(i, isr_stub_table[i], ist, attr);
    }
    idtr.limit = sizeof idt - 1;
    idtr.base  = (u64)&idt;
    __asm__ volatile ("lidt %0" :: "m"(idtr));
}
