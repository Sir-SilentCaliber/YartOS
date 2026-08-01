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
#include <yart/cpu.h>
#include <yart/limine.h>
#include <yart/sched.h>
#include <yart/acpi.h>   /* g_madt.lapic_addr */
#define IA32_APIC_BASE_MSR 0x1B
#define AP_WAKE_VEC 62

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
     * AP's private TSS (selector 0x38) is installed right after. */
    smp_ap_switch_gdt(cpu);
    idt_reload();
    wrmsr64(MSR_GS_BASE, (u64)c);         /* this AP's per-CPU area */
    ap_install_tss(cpu, c->ap_rsp0);      /* per-AP TSS: RSP0 = this AP's
                                             kernel stack + its own ISTs */

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
        if ((c->ap_ticks & 0x7F) == 0x7F)
            kprintf("smp: AP %u alive, ticks=%lu, rq=%u, steals=%lu, work=%lu\n",
                    cpu, (unsigned long)c->ap_ticks,
                    (unsigned)__atomic_load_n(&c->ap_rq_count, __ATOMIC_RELAXED),
                    (unsigned long)c->ap_steals,
                    (unsigned long)__atomic_load_n(&g_ap_work, __ATOMIC_RELAXED));
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
    /* Limine starts the APs once we return from the bootloader entry point
     * with the requests processed - they come up asynchronously after this
     * function, so give them a moment and report what we see. */
    for (volatile u64 t = 0; t < 50000000ULL && g_ap_online < g_ap_count; t++)
        __asm__ volatile("pause");
    kprintf("smp: %d/%d AP(s) online, %lu total CPUs\n",
            g_ap_online, g_ap_count, (unsigned long)(r->cpu_count));
    return g_ap_online;
}
