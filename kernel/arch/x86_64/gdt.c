/* Yart OS - per-CPU GDT + TSS (flat 64-bit, kernel + user code/data).
 *
 * Every CPU gets its OWN GDT + TSS + IST stacks:
 *   - slot 0..5 : null/kernel-code/kernel-data/user-code/user-data/user-code
 *     (slot 5 is a second user-code selector for the fast syscall/sysret
 *      path: sysret hardcodes CS = STAR[63:48]+16 and SS = STAR[63:48]+8,
 *      so with STAR[63:48]=0x18 it returns CS=0x2B -> this slot and
 *      SS=0x23 -> slot 4.  The int 0x80 / iretq path keeps using USER_CS
 *      0x1B at slot 3.)
 *   - BSP  (cpu 0): TSS at slots 6-7, selector 0x30, IST stacks [0]
 *   - AP   (cpu N): TSS at slots 8-9, selector 0x40, IST stacks [N]
 * A shared TSS would be a correctness bug on SMP: two APs running user
 * tasks would clobber each other's RSP0, and a shared IST stack would
 * corrupt if two CPUs faulted simultaneously.  Per-CPU everything.
 *
 * Privilege plumbing:
 *   RSP0  : per-task kernel stack used for ring-3 -> ring-0 entries
 *           (int 0x80, user exceptions, IRQs while in user).  The
 *           scheduler switches it per task via tss_set_rsp0().
 *   IST1  : #DF / #PF handler stack (nested-fault safe, separate from RSP0).
 *   IST2  : NMI / MCE handler stack.
 *   GS    : kernel GS.base holds the per-CPU area; the swapgs pair is armed
 *           (isr.asm entry/exit) so kernel code can reach cpu_local_t.
 */
#include <yart/types.h>
#include <yart/hal.h>
#include <yart/string.h>
#include <yart/cpu.h>
#include <yart/console.h>
#include <yart/user.h>     /* USER_CS/USER_DS/SYS_USER_CS shared defines */

typedef struct PACKED {
    u16 limit_lo;
    u16 base_lo;
    u8  base_mid;
    u8  access;
    u8  flags_limit_hi;
    u8  base_hi;
} gdt_entry_t;

typedef struct PACKED {
    u16 length;
    u16 base_lo;
    u8  base_mid1;
    u8  type;
    u8  flags;
    u8  base_mid2;
    u32 base_hi;
    u32 reserved;
} tss_desc_t;

typedef struct PACKED {
    u16 limit;
    u64 base;
} gdtr_t;

typedef struct PACKED {
    u32 reserved0;
    u64 rsp[3];
    u64 reserved1;
    u64 ist[7];
    u64 reserved2;
    u16 reserved3;
    u16 iopb_offset;
} tss_t;

#define KERNEL_CS 0x08
#define KERNEL_DS 0x10
/* USER_CS/USER_DS/SYS_USER_CS come from yart/user.h (shared with the
 * syscall layer and syscall_entry.asm) */
#define TSS_SEL   0x30      /* BSP TSS (slots 6-7)  */
#define AP_TSS_SEL 0x40     /* AP TSS  (slots 8-9)  */
#define MAX_CPUS  8

static gdt_entry_t gdt[MAX_CPUS][10];
static tss_t       tss[MAX_CPUS];
static gdtr_t      gdtr[MAX_CPUS];

static u8 ist1_stack[MAX_CPUS][KB(16)] ALIGNED(16);  /* #DF / #PF   */
static u8 ist2_stack[MAX_CPUS][KB(16)] ALIGNED(16);  /* NMI / MCE   */
static u8 user_kstack[KB(16)] ALIGNED(16);           /* BSP boot RSP0 */

/* BSP per-CPU area (per-AP areas are allocated on AP startup) */
static cpu_local_t g_bsp_cpu ALIGNED(64);

cpu_local_t *bsp_cpu_local(void) { return &g_bsp_cpu; }

static void set_gate(gdt_entry_t *g, u8 access, u8 flags) {
    g->limit_lo = 0;
    g->base_lo = 0;
    g->base_mid = 0;
    g->access = access;
    g->flags_limit_hi = flags;
    g->base_hi = 0;
}

extern void gdt_flush(gdtr_t *p);   /* asm */
extern void tss_flush(u16 sel);     /* asm */

/* Per-task kernel stack (RSP0), on THIS CPU's TSS.  Called before
 * iretq'ing into a task; runs with valid GS on whichever CPU switches. */
void tss_set_rsp0(u64 rsp0) {
    cpu_local_t *c = get_cpu_local();
    u32 idx = (c && c->cpu_id < MAX_CPUS) ? c->cpu_id : 0;
    tss[idx].rsp[0] = rsp0;
    if (c) c->ap_krsp0 = rsp0;   /* mirror for the fast syscall/sysret path */
}
u64  tss_get_rsp0(void) {
    cpu_local_t *c = get_cpu_local();
    u32 idx = (c && c->cpu_id < MAX_CPUS) ? c->cpu_id : 0;
    return tss[idx].rsp[0];
}

/* Boot-time battery: verifies descriptor access bytes / DPLs, selector
 * arithmetic, TSS type, RSP0/IST1/IST2 separation and the per-CPU GS map. */
static void gdt_selftest(void) {
    bool ok = true;
    kprintf("gdt: selftest\n");
    for (int i = 1; i <= 5; i++) {
        gdt_entry_t *e = &gdt[0][i];
        u8 acc = e->access;
        int dpl = (acc >> 5) & 3;
        bool present = (acc & 0x80) != 0;
        bool code    = (acc & 0x08) != 0;
        bool longmode = (e->flags_limit_hi & 0x20) != 0;
        bool exp_code = (i == 1 || i == 3 || i == 5);
        int  exp_dpl  = (i >= 3) ? 3 : 0;
        kprintf("  gdt[%d] sel=0x%02x acc=0x%02x dpl=%d %s %s L=%d\n",
                i, i * 8, acc, dpl, present ? "P" : "-",
                code ? "code" : "data", longmode);
        if (dpl != exp_dpl || code != exp_code || !present || !longmode) {
            kprintf("    !! descriptor %d wrong (want dpl=%d %s L=1)\n",
                    i, exp_dpl, exp_code ? "code" : "data");
            ok = false;
        }
    }
    if (KERNEL_CS != 1 * 8 || KERNEL_DS != 2 * 8 ||
        USER_CS   != 3 * 8 + 3 || USER_DS != 4 * 8 + 3 ||
        SYS_USER_CS != 5 * 8 + 3 ||
        TSS_SEL   != 6 * 8 || AP_TSS_SEL != 8 * 8) {
        kprintf("  !! selector arithmetic broken\n");
        ok = false;
    }
    tss_desc_t *td = (tss_desc_t *)&gdt[0][6];
    if (td->type != 0x89 && td->type != 0x8B) {   /* 0x8B = busy after ltr */
        kprintf("  !! TSS type=0x%02x expected 0x89/0x8b\n", td->type);
        ok = false;
    }
    u64 r0 = tss[0].rsp[0], i1 = tss[0].ist[0], i2 = tss[0].ist[1];
    bool r0ok = r0 > (u64)user_kstack && r0 <= (u64)(user_kstack + sizeof user_kstack);
    bool i1ok = i1 > (u64)ist1_stack[0] && i1 <= (u64)(ist1_stack[0] + sizeof ist1_stack[0]);
    bool i2ok = i2 > (u64)ist2_stack[0] && i2 <= (u64)(ist2_stack[0] + sizeof ist2_stack[0]);
    bool distinct = (r0 != i1 && r0 != i2 && i1 != i2);
    kprintf("  rsp0=%p (user kstack)  ist1=%p (#DF/#PF)  ist2=%p (NMI/MCE)\n",
            (void *)r0, (void *)i1, (void *)i2);
    if (!r0ok || !i1ok || !i2ok || !distinct) {
        kprintf("    !! RSP0/IST layout violation (rsp0 must not alias IST1)\n");
        ok = false;
    }
    cpu_local_t *c = get_cpu_local();
    if (c != &g_bsp_cpu || c->magic != CPU_LOCAL_MAGIC || c->self != (u64)c) {
        kprintf("  !! per-CPU GS mapping broken: got %p magic=%llx\n",
                (void *)c, (unsigned long long)c->magic);
        ok = false;
    } else {
        kprintf("  gs: per-cpu @ %p magic=%llx (swapgs armed)\n",
                (void *)c, (unsigned long long)c->magic);
    }
    kprintf("gdt: selftest %s\n", ok ? "PASS" : "FAIL");
}

/* Reload the CURRENT CPU's GDT + TSS (APs start on Limine's GDT with its
 * own selectors; we must switch them to ours so iretq/IRQs work). */
void gdt_reload(void) {
    cpu_local_t *c = get_cpu_local();
    u32 idx = (c && c->cpu_id < MAX_CPUS) ? c->cpu_id : 0;
    u64 gs = rdmsr64(MSR_GS_BASE);          /* gdt_flush wipes GS.base    */
    gdt_flush(&gdtr[idx]);
    wrmsr64(MSR_GS_BASE, gs);
    tss_flush(idx == 0 ? TSS_SEL : AP_TSS_SEL);
}

/* Switch an AP onto ITS OWN GDT: lgdt + reload data segments + far-jump to
 * our kernel-code selector (0x08).  AP-safe: no ltr here (loading the BSP's
 * busy TSS on another CPU would #GP); ap_install_tss() installs + loads the
 * AP's private TSS right after. */
void smp_ap_switch_gdt(u32 cpu_index) {
    __asm__ volatile(
        "lgdt %0\n\t"
        "mov $0x10, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "mov %%ax, %%ss\n\t"
        "pushq $0x08\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        :: "m"(gdtr[cpu_index]) : "rax", "memory");
}

/* Install an AP's private TSS (GDT slots 8-9, selector 0x40): per-CPU
 * RSP0 + per-CPU IST1/IST2 stacks, then reload that AP's GDT + TR.
 * gdt_flush reloads GS from a flat descriptor (base 0), so capture the
 * per-CPU pointer BEFORE the flush (via MSR, not GS:0 which is dead at
 * that moment) and restore GS.base AFTER. */
void ap_install_tss(u32 cpu_index, u64 rsp0) {
    memset(&tss[cpu_index], 0, sizeof tss[cpu_index]);
    tss[cpu_index].rsp[0]    = rsp0;
    tss[cpu_index].ist[0]    = (u64)(ist1_stack[cpu_index] + sizeof ist1_stack[cpu_index]);
    tss[cpu_index].ist[1]    = (u64)(ist2_stack[cpu_index] + sizeof ist2_stack[cpu_index]);
    tss[cpu_index].iopb_offset = sizeof tss[cpu_index];

    tss_desc_t *td = (tss_desc_t *)&gdt[cpu_index][8];
    u64 base = (u64)&tss[cpu_index];
    td->length    = sizeof tss[cpu_index] - 1;
    td->base_lo   = base & 0xFFFF;
    td->base_mid1 = (base >> 16) & 0xFF;
    td->type      = 0x89;
    td->flags     = 0x00;
    td->base_mid2 = (base >> 24) & 0xFF;
    td->base_hi   = (base >> 32) & 0xFFFFFFFF;
    td->reserved  = 0;

    cpu_local_t *apc = (cpu_local_t *)rdmsr64(MSR_GS_BASE);
    gdtr[cpu_index].limit = sizeof gdt[cpu_index] - 1;
    gdtr[cpu_index].base  = (u64)&gdt[cpu_index];
    gdt_flush(&gdtr[cpu_index]);
    if (apc) wrmsr64(MSR_GS_BASE, (u64)apc);
    tss_flush(AP_TSS_SEL);
}

void gdt_init(void) {
    memset(gdt, 0, sizeof gdt);
    /* every CPU's GDT shares slots 0-4; build them for all CPUs now so an
     * AP can switch to its own GDT before ap_install_tss() runs */
    for (int c = 0; c < MAX_CPUS; c++) {
        set_gate(&gdt[c][0], 0x00, 0x00);   /* null */
        set_gate(&gdt[c][1], 0x9A, 0xA0);   /* kernel code  L=1 (SYSCALL CS) */
        set_gate(&gdt[c][2], 0x92, 0xA0);   /* kernel data (SYSCALL SS) */
        set_gate(&gdt[c][3], 0xFA, 0xA0);   /* user code  (0x1B, int 0x80) */
        set_gate(&gdt[c][4], 0xF2, 0xA0);   /* user data  (0x23, SYSRET SS) */
        set_gate(&gdt[c][5], 0xFA, 0xA0);   /* user code  (0x2B, SYSRET CS) */
    }

    /* BSP TSS at slots 6-7 (selector 0x30) */
    memset(&tss[0], 0, sizeof tss[0]);
    tss[0].rsp[0] = (u64)(user_kstack + sizeof user_kstack);
    tss[0].ist[0] = (u64)(ist1_stack[0] + sizeof ist1_stack[0]);
    tss[0].ist[1] = (u64)(ist2_stack[0] + sizeof ist2_stack[0]);
    tss[0].iopb_offset = sizeof tss[0];

    tss_desc_t *td = (tss_desc_t *)&gdt[0][6];
    u64 base = (u64)&tss[0];
    td->length    = sizeof tss[0] - 1;
    td->base_lo   = base & 0xFFFF;
    td->base_mid1 = (base >> 16) & 0xFF;
    td->type      = 0x89;        /* present, type=available 64-bit TSS */
    td->flags     = 0x00;
    td->base_mid2 = (base >> 24) & 0xFF;
    td->base_hi   = (base >> 32) & 0xFFFFFFFF;
    td->reserved  = 0;

    /* every CPU's GDTR points at its own GDT from the start: an AP must be
     * able to lgdt its own table in smp_ap_switch_gdt() before any TSS is
     * installed (a zeroed GDTR would triple-fault the AP immediately) */
    for (int c = 0; c < MAX_CPUS; c++) {
        gdtr[c].limit = sizeof gdt[c] - 1;
        gdtr[c].base  = (u64)&gdt[c];
    }
    gdt_flush(&gdtr[0]);
    tss_flush(TSS_SEL);

    /* Per-CPU GS: MUST be programmed after gdt_flush (which reloads GS from
     * the descriptor, clobbering GS.base with the descriptor's base 0). */
    memset(&g_bsp_cpu, 0, sizeof g_bsp_cpu);
    g_bsp_cpu.self   = (u64)&g_bsp_cpu;
    g_bsp_cpu.magic  = CPU_LOCAL_MAGIC;
    g_bsp_cpu.cpu_id = 0;
    wrmsr64(MSR_GS_BASE, (u64)&g_bsp_cpu);        /* kernel per-cpu area */
    wrmsr64(MSR_KERNEL_GS_BASE, 0);               /* user GS base = 0    */

    gdt_selftest();
}
