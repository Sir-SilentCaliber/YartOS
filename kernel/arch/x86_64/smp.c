/* Yart OS - SMP support via the Limine SMP protocol.
 *
 * The bootloader (Limine) starts the Application Processors itself and
 * hands the kernel a list of CPUs.  For each AP we set its `goto_address`
 * to our entry; Limine jumps the AP there in 64-bit long mode with the
 * kernel's page tables and a fresh per-AP stack already set up - no
 * INIT-SIPI-SIPI trampolines needed (which are fragile under QEMU/TCG).
 *
 * Each AP: adopts its OWN GDT/IDT (gdt.c builds a private GDT + TSS per
 * CPU), installs its per-CPU GS area, arms its LAPIC timer, and enters an
 * idle loop.  Real work arrives as:
 *   - kernel work items (per-CPU single slot, sched_queue_ap_work) run
 *     inline in the idle loop;
 *   - USER tasks land on the AP's ready queue via migration / fork
 *     load-balancing; a wake IPI (vec 62) makes the AP's interrupt path
 *     switch to them (see sched_tick) - the AP never raw-switches, so it
 *     can always iretq back to this loop when its queue empties.
 * The AP's own timer preempts its user tasks exactly like the BSP's.
 */
#include <yart/types.h>
#include <yart/console.h>
#include <yart/string.h>
#include <yart/mm.h>
#include <yart/hal.h>
#include <yart/io.h>    /* invlpg / write_cr3 / irq_save (TLB shootdown) */
#include <yart/spinlock.h>   /* g_tlb_lock (serialize shootdowns)       */

/* forward decl: the AP-entry + smp_start_aps paths register the vec-63
 * handler before its definition below */
void smp_tlb_ipi_handler(cpu_regs_t *r);
#include <yart/cpu.h>
#include <yart/limine.h>
#include <yart/sched.h>
#include <yart/syscall.h>
#include <yart/acpi.h>   /* g_madt.lapic_addr */
#define IA32_APIC_BASE_MSR 0x1B
#define AP_WAKE_VEC 62
#define TLB_FLUSH_VEC 63            /* TLB shootdown IPI (see below)       */

__attribute__((used, section(".limine_requests")))
static volatile struct limine_smp_request smp_request = {
    .id = LIMINE_SMP_REQUEST, .revision = 1, .response = 0, .flags = 0
};

extern unsigned int g_cpu_count_hint;   /* console.c */

static void ap_demo_work(void *arg) {
    u32 who = (u32)(uintptr_t)arg;
    kprintf("smp: demo work executed by AP %u (queued by BSP)\n", who);
}

static int  g_ap_count;
static int  g_ap_online;
static bool g_smp_ready;
static u32  g_bsp_lapic_id;
static cpu_local_t *g_ap_areas[8];       /* indexed by cpu id (1..7) */
cpu_local_t *smp_get_ap_area(u32 idx) { return g_ap_areas[idx % 8]; }
volatile unsigned long g_ap_work;        /* shared parallel-work counter */

/* AP entry: called by Limine on each AP (rdi = limine_smp_info*).
 * extra_argument carries the cpu id; the per-CPU area is found by id. */
static void limine_ap_entry(struct limine_smp_info *info) {
    u32 cpu = (u32)info->extra_argument;
    cpu_local_t *c = g_ap_areas[cpu % 8];

    c->cpu_id = cpu;
    c->ap_lapic_id = info->lapic_id;
    c->ap_up = 1;

    /* enable this AP's LAPIC */
    u64 base = rdmsr64(IA32_APIC_BASE_MSR);
    wrmsr64(IA32_APIC_BASE_MSR, base | (1ULL << 11));
    volatile u32 *lapic = (volatile u32 *)phys_to_virt((paddr_t)g_madt.lapic_addr);
    lapic[0x0F0 / 4] = (1u << 8) | 0xFF;
    lapic[0x080 / 4] = 0;
    __asm__ volatile("mfence" ::: "memory");

    kprintf("smp: AP %u online (lapic id %u, processor id %u)\n",
            cpu, info->lapic_id, info->processor_id);
    g_ap_online++;

    /* adopt OUR per-CPU GDT + shared IDT so this AP can take our interrupt
     * vectors.  ORDER MATTERS: switching the GDT reloads GS from a flat
     * descriptor (base 0), wiping any per-CPU GS base - so set GS.base
     * AFTER.  smp_ap_switch_gdt does NOT ltr (the BSP's TSS is busy); the
     * AP's private TSS (selector 0x40) is installed right after. */
    smp_ap_switch_gdt(cpu);
    idt_reload();
    wrmsr64(MSR_GS_BASE, (u64)c);         /* this AP's per-CPU area */
    ap_install_tss(cpu, c->ap_rsp0);      /* per-AP TSS: RSP0 = this AP's
                                             kernel stack + its own ISTs */
    syscall_install_percpu();             /* EFER.SCE + STAR/LSTAR/SFMASK
                                             for SYSCALL on this AP */
    /* CR0/CR4 are PER-CPU: the BSP's fpu_enable() did not touch this AP,
     * so without this every SSE instruction in a user task on this core
     * (#UD) - e.g. the first XMM load in an exec'd binary. */
    fpu_enable();

    /* per-AP APIC timer: this core now ticks on our vector 48 and can
     * preempt user tasks exactly like the BSP */
    apic_timer_start_this_cpu();
    sti();

    kprintf("smp: AP %u live on its own GDT/TSS, timer armed\n", cpu);

    /* idle loop: kernel work items run inline; user tasks arrive via wake
     * IPI and are switched to by the interrupt path.  When this CPU's
     * queue is empty we steal work from busier CPUs (load balancing). */
    for (;;) {
        if (c->ap_kwork_fn)
            sched_ap_try_work(c);
        if (!__atomic_load_n(&c->ap_rq_head, __ATOMIC_RELAXED))
            sched_ap_steal(c);            /* idle + empty: try to steal */
        __atomic_fetch_add(&g_ap_work, 1, __ATOMIC_RELAXED);
        /* idle: a wake IPI (vec 62) or our own timer interrupts the hlt;
         * if a user task was switched to, its excursion iretq's back to
         * this point (ap_idle_rsp was saved on the way in) */
        c->ap_idle_rsp = 0;
        __asm__ volatile("sti; hlt");
    }
}

/* Queue a kernel work item to every online AP: proves the wake-IPI path
 * delivers work to all cores even before any user task exists. */
void smp_ap_kwork_demo(void) {
    for (u32 i = 1; i < 8; i++) {
        cpu_local_t *c = g_ap_areas[i];
        if (c && c->ap_up)
            sched_queue_ap_work(c, ap_demo_work, (void *)(uintptr_t)c->cpu_id);
    }
}

/* Least-loaded online CPU for a freshly-forked task (APs only; the BSP keeps
 * the desktop + its own tasks).  Returns NULL when no AP is online. */
cpu_local_t *smp_least_loaded(void) {
    cpu_local_t *best = NULL;
    u32 best_load = 0xFFFFFFFFu;
    for (u32 i = 1; i < 8; i++) {
        cpu_local_t *c = g_ap_areas[i];
        if (!c || !c->ap_up) continue;
        u32 load = __atomic_load_n(&c->ap_rq_count, __ATOMIC_RELAXED);
        if (c->ap_current && c->ap_current->is_user) load++;
        if (load < best_load) { best_load = load; best = c; }
    }
    return best;
}

int smp_start_aps(void) {
    if (!smp_request.response) {
        kprintf("smp: no Limine SMP response - single CPU\n");
        return 0;
    }
    struct limine_smp_response *r = (struct limine_smp_response *)smp_request.response;
    g_bsp_lapic_id = r->bsp_lapic_id;
    g_cpu_count_hint = (unsigned)(r->cpu_count);   /* console locks now */
    kprintf("smp: Limine reports %lu CPU(s), BSP lapic %u, rev %lu%s\n",
            (unsigned long)r->cpu_count, r->bsp_lapic_id,
            (unsigned long)r->revision,
            (r->flags & LIMINE_SMP_X2APIC) ? " (x2APIC)" : "");

    /* per-CPU areas: one per AP, indexed by processor_id */
    u32 cpu = 1;
    for (u64 i = 0; i < r->cpu_count; i++) {
        struct limine_smp_info *info = r->cpus[i];
        if (info->lapic_id == r->bsp_lapic_id) continue;   /* BSP */
        if (cpu >= 8) break;

        paddr_t cp = pmm_alloc_page();
        cpu_local_t *c = phys_to_virt(cp);
        memset(c, 0, sizeof *c);
        c->self = (u64)c;
        c->magic = CPU_LOCAL_MAGIC;
        c->cpu_id = cpu;

        /* per-AP kernel stack: RSP0 for ring3->ring0 entries on this CPU
         * (the AP TSS points here until the scheduler switches per-task) */
        paddr_t ksp = pmm_alloc_pages(KSTACK_SIZE / PAGE_SIZE);
        c->ap_rsp0 = (u64)phys_to_virt(ksp) + KSTACK_SIZE;

        g_ap_areas[cpu % 8] = c;
        info->extra_argument = cpu;            /* AP learns its cpu id  */
        info->goto_address = limine_ap_entry;
        g_ap_count++;
        kprintf("smp: arming AP %u (lapic id %u, processor id %u, rsp0=%p)...\n",
                cpu, info->lapic_id, info->processor_id, (void *)c->ap_rsp0);
        cpu++;
    }
    g_smp_ready = true;
    irq_register(TLB_FLUSH_VEC, smp_tlb_ipi_handler);
    /* Limine starts the APs once we return from the bootloader entry point
     * with the requests processed - they come up asynchronously after this
     * function, so give them a moment and report what we see. */
    for (volatile u64 t = 0; t < 50000000ULL && g_ap_online < g_ap_count; t++)
        __asm__ volatile("pause");
    kprintf("smp: %d/%d AP(s) online, %lu total CPUs\n",
            g_ap_online, g_ap_count, (unsigned long)(r->cpu_count));
    return g_ap_online;
}

/* =====================================================================
 * TLB SHOOTDOWN (SMP correctness)
 *
 * invlpg() and CR3 reloads are per-CPU.  When one CPU modifies SHARED
 * kernel page tables - the HHDM direct map (kstack guard pages are
 * unmapped/remapped there, and vmm_nx_direct_map re-stamps every PTE) -
 * or a PML4 that another CPU may have loaded right now (wm surface
 * mappings installed from the app's syscall), the other CPUs' TLBs keep
 * stale entries: a freed/guarded page stays reachable, a freshly mapped
 * surface stays invisible (and can be demand-fault-shadowed by a stale
 * "not present" entry).  This is the textbook TLB-shootdown protocol:
 * request -> IPI -> flush -> ack, serialized so req/gen pairing stays
 * unambiguous.
 *
 * DEADLOCK RULES for callers: must not be in an interrupt handler, and
 * must not hold a spinlock that another CPU could be spinning on with
 * IRQs disabled (that CPU would never reach its IPI handler).  All
 * current call sites (kstack guard map/unmap, the NX pass, wm surface
 * map/unmap) satisfy this.  The sender runs with local IRQs off while
 * waiting so it cannot be preempted mid-shootdown.
 * ===================================================================== */
static spinlock_t g_tlb_lock;
/* Monotonic request sequence: every shootdown gets a fresh, unique number
 * so an AP's ack (tlb_gen == tlb_req) is unambiguous even when requests
 * are batched.  (The sender's own tlb_gen must NOT be used - it only
 * advances when the sender ITSELF receives a shootdown IPI, which never
 * happens because self is flushed locally.) */
static volatile u64 g_tlb_seq;

void smp_tlb_ipi_handler(cpu_regs_t *r) {
    (void)r;
    cpu_local_t *c = get_cpu_local();
    /* Flush until caught up.  Only one shootdown is in flight at a time
     * (global lock), so this terminates; the loop also covers a request
     * that arrived while this IPI was already pending. */
    while (c->tlb_req != c->tlb_gen) {
        if (c->tlb_full) write_cr3(read_cr3());
        else             invlpg(c->tlb_va);
        c->tlb_full = 0;
        c->tlb_gen  = c->tlb_req;   /* ack */
    }
}

static void smp_tlb_flush_others(bool full, u64 va) {
    if (!g_smp_ready) return;       /* single-CPU / boot: local flush only */
    u64 fl = irq_save();
    spin_lock(&g_tlb_lock);
    cpu_local_t *me = get_cpu_local();
    u64 req = __atomic_add_fetch(&g_tlb_seq, 1, __ATOMIC_RELAXED);
    for (u32 i = 0; i < 8; i++) {
        cpu_local_t *c = (i == 0) ? bsp_cpu_local() : smp_get_ap_area(i);
        if (!c || !c->ap_up || c == me) continue;
        c->tlb_va   = va;
        c->tlb_full = full ? 1 : 0;
        __atomic_store_n(&c->tlb_req, req, __ATOMIC_RELEASE);
        lapic_send_ipi(c->ap_lapic_id, TLB_FLUSH_VEC);
    }
    /* wait for every ack (generous timeout: TCG APs can be slow) */
    for (u32 i = 0; i < 8; i++) {
        cpu_local_t *c = (i == 0) ? bsp_cpu_local() : smp_get_ap_area(i);
        if (!c || !c->ap_up || c == me) continue;
        u64 spins = 0;
        while (__atomic_load_n(&c->tlb_gen, __ATOMIC_ACQUIRE) < req) {
            __asm__ volatile("pause");
            if (++spins > 400000000ULL) {
                kprintf("smp: !! TLB shootdown TIMEOUT waiting for CPU %u "
                        "(req=%llu gen=%llu) - stale TLB entries possible\n",
                        i, (unsigned long long)req,
                        (unsigned long long)c->tlb_gen);
                break;
            }
        }
    }
    spin_unlock(&g_tlb_lock);
    irq_restore(fl);
}

void smp_tlb_shootdown(u64 va) {
    invlpg(va);                     /* self */
    smp_tlb_flush_others(false, va);
}

void smp_tlb_shootdown_all(void) {
    write_cr3(read_cr3());          /* self: full flush */
    smp_tlb_flush_others(true, 0);
}

bool smp_tlb_selftest(void) {
    if (g_ap_count == 0) {
        kprintf("smp: TLB shootdown selftest skipped (single CPU)\n");
        return true;
    }
    u64 before[8] = {0};
    for (u32 i = 1; i < 8; i++) {
        cpu_local_t *c = smp_get_ap_area(i);
        before[i] = (c && c->ap_up) ? __atomic_load_n(&c->tlb_gen, __ATOMIC_ACQUIRE) : 0;
    }
    u64 t0 = pit_ticks();
    const int ROUNDS = 64;
    for (int i = 0; i < ROUNDS; i++)
        smp_tlb_shootdown(0xffff8000000f0000ULL + (u64)(i % 16) * PAGE_SIZE);
    smp_tlb_shootdown_all();
    u64 dt = pit_ticks() - t0;
    bool ok = true;
    for (u32 i = 1; i < 8; i++) {
        cpu_local_t *c = smp_get_ap_area(i);
        if (!c || !c->ap_up) continue;
        if (__atomic_load_n(&c->tlb_gen, __ATOMIC_ACQUIRE) != before[i] + ROUNDS + 1)
            ok = false;
    }
    kprintf("smp: TLB shootdown selftest %s (%d single + 1 full flush, %u APs, %lu ticks)\n",
            ok ? "PASS" : "FAIL", ROUNDS, (unsigned)g_ap_count, (unsigned long)dt);
    return ok;
}
