/* Yart OS - IDT, ISR/IRQ dispatch, exception handling.
 *
 * isr_dispatch() returns the RSP the ISR stub should resume from.  For a
 * normal (non-switching) interrupt that is the frame it was handed; the
 * scheduler may return a *different* task's frame, and the stub iretq's
 * into it - that is the context switch.
 *
 * Privilege model:
 *   - int 0x80 (DPL=3) is the only user-accessible gate.
 *   - #DF/#PF run on IST1, NMI/MCE on IST2.
 *   - user-mode faults on recoverable vectors are delivered as SIGSEGV:
 *     the task is turned into a zombie via sched_exit() and the kernel
 *     (or another task) keeps running.
 */
#include <yart/types.h>
#include <yart/hal.h>
#include <yart/console.h>
#include <yart/string.h>
#include <yart/io.h>
#include <yart/user.h>
#include <yart/task.h>
#include <yart/cpu.h>
#include <yart/mm.h>
#include <yart/sched.h>

#define VEC_NMI      2
#define VEC_DF       8
#define VEC_PF       14
#define VEC_MCE      18
#define VEC_SYS      0x80

#define IST_NONE     0
#define IST_DF_PF    1
#define IST_NMI_MCE  2

#define PF_W  (1u << 1)
#define PF_U  (1u << 2)

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
    idt[n].off_hi  = (u32)(handler >> 32);
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

static volatile u64 g_nmi_count, g_mce_count;

/* NMI: log and continue.  Runs on IST2 and must NOT touch the per-CPU GS
 * base (an NMI can land inside the swapgs window). */
/* NMI handler: MUST be lock-free and must NOT print.  An NMI can land on a
 * CPU that is inside kprintf (holding the console spinlock with IRQs off);
 * calling kprintf here would self-deadlock and freeze the whole machine.
 * Count and continue (the count is reported elsewhere). */
static void nmi_handler(cpu_regs_t *r) {
    (void)r;
    g_nmi_count++;
}

static void mce_handler(cpu_regs_t *r) {
    g_mce_count++;
    cli();
    kprintf("\n=== CPU EXCEPTION ===\n");
    kprintf("vec=18 (Machine Check)  err=0x%lx  count=%llu\n",
            r->err, (unsigned long long)g_mce_count);
    kprintf("RIP=%p  CS=0x%lx  RFLAGS=0x%lx\n",
            (void *)r->rip, r->cs, r->rflags);
    kpanic("Machine Check (usually fatal)");
}

static bool from_user_frame(const cpu_regs_t *r) { return (r->cs & 3) == 3; }

static bool recoverable_user_fault(u64 vec) {
    switch (vec) {
    case 0:  case 3:  case 4:  case 5:  case 6:  case 7:
    case 11: case 12: case 13: case 16: case 17:
        return true;
    default:
        return false;
    }
}

static void dump_exception(cpu_regs_t *r) {
    kprintf("\n=== CPU EXCEPTION ===\n");
    kprintf("vec=%lu (%s)  err=0x%lx\n",
            r->vector, r->vector < 32 ? exc_names[r->vector] : "?",
            r->err);
    kprintf("RIP=%p  CS=0x%lx  RFLAGS=0x%lx\n",
            (void *)r->rip, r->cs, r->rflags);
    kprintf("RSP=%p  SS=0x%lx\n", (void *)r->rsp, r->ss);
    kprintf("RAX=%p RBX=%p RCX=%p RDX=%p\n",
            (void *)r->rax, (void *)r->rbx,
            (void *)r->rcx, (void *)r->rdx);
    kprintf("RSI=%p RDI=%p RBP=%p\n",
            (void *)r->rsi, (void *)r->rdi, (void *)r->rbp);
}

/* Turn the faulting user task into a zombie and switch away.  Returns the
 * RSP the stub should resume (the next task's frame). */
static u64 kill_user_task(const char *what, u64 vec, u64 err, u64 rsp) {
    kprintf("\ncpu: SIGSEGV! %s (vec=%lu err=0x%lx) by task %d - killing task\n",
            what, vec, err, task_getpid());
    sched_exit(-11);                       /* never returns to this task */
    return sched_after_isr(rsp);
}

/* Page fault (vector 14). */
static u64 page_fault(cpu_regs_t *r) {
    u64 va  = read_cr2();
    u64 err = r->err;
    bool write = (err & PF_W) != 0;
    bool from_user = from_user_frame(r) || (err & PF_U);

    /* Demand paging / swap-in / copy-on-write: a normal, resolvable event. */
    if (vmm_resolve_user_fault(va, write)) return (u64)r;

    if (from_user && sched_current_is_user()) {
        kprintf("vmm: SIGSEGV! user page fault at va=0x%lx (err=0x%lx) rip=%p\n",
                va, err, (void *)r->rip);
        return kill_user_task("Page Fault", r->vector, err, (u64)r);
    }

    cli();
    dump_exception(r);
    kprintf("vmm: kernel page fault at va=0x%lx (err=0x%lx)\n", va, err);
    kpanic("Unhandled CPU exception #14 (Page Fault)");
}

/* Returns the RSP to resume from (possibly a different task's frame). */
u64 isr_dispatch(cpu_regs_t *r) {
    if (r->vector == VEC_NMI) { nmi_handler(r); return (u64)r; }
    if (r->vector == VEC_MCE) { mce_handler(r); return (u64)r; }
    if (r->vector == VEC_PF)  { return page_fault(r); }

    if (r->vector < 32) {
        if (from_user_frame(r) && sched_current_is_user() &&
            recoverable_user_fault(r->vector)) {
            kprintf("vmm: user exception vec=%lu err=0x%lx rip=%p\n",
                    r->vector, r->err, (void *)r->rip);
            return kill_user_task(exc_names[r->vector], r->vector, r->err,
                                  (u64)r);
        }
        cli();
        dump_exception(r);
        kpanic("Unhandled CPU exception #%lu (%s)",
               r->vector, r->vector < 32 ? exc_names[r->vector] : "unknown");
    }

    cpu_local_t *cpu = get_cpu_local();
    cpu->irq_nesting++;
    /* AP wake IPI (vector 62): interrupt the hlt so the loop re-checks
     * its runqueue.  Count it for debugging. */
    if (r->vector == 62) {
        cpu->ap_resched_ipis++;
        if (cpu->ap_resched_ipis < 8)
            kprintf("smp: CPU %u received reschedule IPI #%lu\n",
                    cpu->cpu_id, (unsigned long)cpu->ap_resched_ipis);
    }
    if (handlers[r->vector]) handlers[r->vector](r);
    if (r->vector >= 32 && r->vector < 64) interrupt_eoi(r->vector);
    cpu->irq_nesting--;

    /* Every CPU schedules itself now: the scheduler state is per-CPU
     * (cpu_local_t::ap_current + per-CPU ready queues).  Only the OUTERMOST
     * interrupt may reschedule (nested handlers never switch). */
    u64 nrsp = (u64)r;
    if (cpu->irq_nesting == 0) {
        bool timer = (r->vector == 32 || r->vector == 48);
        if (timer || r->vector == 62)
            nrsp = sched_tick(nrsp);          /* timer preempt + IPI wake */
        else if (r->vector == VEC_SYS) nrsp = sched_after_isr(nrsp);
    }
    return nrsp;
}

static void idt_audit(void) {
    int user_gates = 0;
    bool ok = true;
    for (int i = 0; i < 256; i++) {
        u8 attr = idt[i].type_attr;
        u8 ist  = idt[i].ist;
        int dpl = (attr >> 5) & 3;
        if (!(attr & 0x80)) { ok = false; continue; }
        if (dpl == 3) {
            user_gates++;
            if (i != VEC_SYS) { kprintf("idt: [%d] !! user gate\n", i); ok = false; }
        }
        u8 expect_ist = (i == VEC_DF || i == VEC_PF) ? IST_DF_PF
                      : (i == VEC_NMI || i == VEC_MCE) ? IST_NMI_MCE : IST_NONE;
        if (ist != expect_ist) { kprintf("idt: [%d] !! ist=%d\n", i, ist); ok = false; }
    }
    kprintf("idt: gate audit %s (%d user gate(s))\n",
            (ok && user_gates == 1) ? "PASS" : "FAIL", user_gates);
}

/* Reload the IDT on the CURRENT CPU (APs inherit Limine's temporary IDT,
 * which lacks our gates - e.g. the APIC timer vector 48). */
void idt_reload(void) {
    __asm__ volatile ("lidt %0" :: "m"(idtr));
}

void idt_init(void) {
    memset(idt, 0, sizeof idt);
    memset(handlers, 0, sizeof handlers);
    for (int i = 0; i < 256; i++) {
        u8 ist = (i == VEC_DF || i == VEC_PF) ? IST_DF_PF
               : (i == VEC_NMI || i == VEC_MCE) ? IST_NMI_MCE : IST_NONE;
        u8 attr = (i == VEC_SYS) ? 0xEE : 0x8E;
        set_gate(i, isr_stub_table[i], ist, attr);
    }
    idtr.limit = sizeof idt - 1;
    idtr.base  = (u64)&idt;
    __asm__ volatile ("lidt %0" :: "m"(idtr));
    idt_audit();
}
