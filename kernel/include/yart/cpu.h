#pragma once
#include <yart/types.h>
#include <yart/io.h>

/* x86-64 model-specific registers the kernel uses */
#define MSR_FS_BASE        0xC0000100UL
#define MSR_GS_BASE        0xC0000101UL
#define MSR_KERNEL_GS_BASE 0xC0000102UL
#define MSR_EFER           0xC0000080UL
#define MSR_STAR           0xC0000081UL
#define MSR_LSTAR          0xC0000082UL
#define MSR_SFMASK         0xC0000084UL
#define IA32_APIC_BASE     0x1BUL

static ALWAYS_INLINE u64 rdmsr64(u32 msr) {
    u32 lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((u64)hi << 32) | lo;
}
static ALWAYS_INLINE void wrmsr64(u32 msr, u64 v) {
    __asm__ volatile ("wrmsr" :: "a"((u32)v), "d"((u32)(v >> 32)), "c"(msr));
}
static ALWAYS_INLINE void swapgs(void) { __asm__ volatile ("swapgs"); }
static ALWAYS_INLINE void cpuid_id(u32 leaf, u32 *a, u32 *b, u32 *c, u32 *d) {
    __asm__ volatile ("cpuid"
        : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
        : "a"(leaf), "c"(0));
}

/* control registers */
static ALWAYS_INLINE u64 read_cr0(void) {
    u64 v; __asm__ volatile ("mov %%cr0, %0" : "=r"(v)); return v;
}
static ALWAYS_INLINE void write_cr0(u64 v) {
    __asm__ volatile ("mov %0, %%cr0" :: "r"(v) : "memory");
}
static ALWAYS_INLINE u64 read_cr4(void) {
    u64 v; __asm__ volatile ("mov %%cr4, %0" : "=r"(v)); return v;
}
static ALWAYS_INLINE void write_cr4(u64 v) {
    __asm__ volatile ("mov %0, %%cr4" :: "r"(v) : "memory");
}

/* Supervisor-Mode Access/Execution Prevention: stac() temporarily allows
 * kernel accesses to user pages (SMAP), clac() re-forbids them.
 *
 * We use a runtime branch on g_have_smap so the kernel still boots on CPUs
 * (like qemu64) that don't implement the SMAP extensions — executing `stac`
 * on such a CPU #UDs.  On hardware / `-cpu host` the branch predicts hot. */
extern u32 g_cpu_features;
#define CPUFEAT_SMAP 2
#define CPUFEAT_SMEP 1
static ALWAYS_INLINE void stac(void) {
    if (g_cpu_features & CPUFEAT_SMAP) __asm__ volatile("stac" ::: "cc", "memory");
    else                              __asm__ volatile("" ::: "memory");
}
static ALWAYS_INLINE void clac(void) {
    if (g_cpu_features & CPUFEAT_SMAP) __asm__ volatile("clac" ::: "cc", "memory");
    else                              __asm__ volatile("" ::: "memory");
}
static ALWAYS_INLINE bool cpu_has_smep(void) {
    u32 a, b, c, d;
    cpuid_id(7, &a, &b, &c, &d);     /* leaf 7, subleaf 0 */
    return (b >> 7) & 1;
}
static ALWAYS_INLINE bool cpu_has_smap(void) {
    u32 a, b, c, d;
    cpuid_id(7, &a, &b, &c, &d);
    return (b >> 20) & 1;
}
/* Enable SMEP/SMAP only if the CPU reports support (qemu64 lacks them). */
static ALWAYS_INLINE u32 smep_smap_enable(void) {
    u64 cr4 = read_cr4();
    u32 got = 0;
    if (cpu_has_smep()) { cr4 |= (1ULL << 20); got |= CPUFEAT_SMEP; }
    if (cpu_has_smap()) { cr4 |= (1ULL << 21); got |= CPUFEAT_SMAP; }
    write_cr4(cr4);
    g_cpu_features = got;
    return got;   /* bit0 = SMEP, bit1 = SMAP */
}

/* FPU/SSE state save-restore (FXSAVE/FXRSTOR, 512-byte areas) */
static ALWAYS_INLINE void fpu_save(void *p) {
    __asm__ volatile ("fxsave (%0)" :: "r"(p) : "memory");
}
static ALWAYS_INLINE void fpu_restore(void *p) {
    __asm__ volatile ("fxrstor (%0)" :: "r"(p) : "memory");
}

/* Enable the x87/SSE/AVX units and return a clean FPU area (via fninit). */
static ALWAYS_INLINE void fpu_enable(void) {
    u64 cr0 = read_cr0();
    cr0 &= ~(1ULL << 2);        /* EM=0: x87 present          */
    cr0 |=  (1ULL << 1);        /* MP=1: monitor x87          */
    write_cr0(cr0);
    u64 cr4 = read_cr4();
    cr4 |= (1ULL << 9);         /* OSFXSR: enable SSE         */
    cr4 |= (1ULL << 10);        /* OSXMMEXCPT                 */
    write_cr4(cr4);
    __asm__ volatile ("fninit");
}
static ALWAYS_INLINE void fpu_capture_clean(void *p) {
    __asm__ volatile ("fninit");
    fpu_save(p);
}

#define CPU_LOCAL_MAGIC 0x59415254434CULL   /* "YARTCL" */

/* Per-CPU area, reached through GS.base (kernel mode only, swapgs armed).
 * One exists per CPU: the BSP's is a static in gdt.c, each AP's is
 * allocated in smp_start_aps().
 * NOTE: self must live at offset 0 so that "%gs:0" loads the area pointer. */
typedef struct cpu_local_s {
    u64  self;                /* == (u64)this, sanity check             */
    u64  magic;               /* CPU_LOCAL_MAGIC                        */
    u32  cpu_id;              /* 0 = BSP, 1.. = APs                     */
    u32  irq_nesting;         /* interrupt re-entry depth                */
    u64  ap_rsp0;             /* this CPU's kernel stack top (RSP0)      */
    u64  ap_idle_rsp;         /* idle-loop ISR frame (AP user->idle hop) */
    u32  ap_lapic_id;         /* LAPIC ID of this CPU                    */
    u32  ap_up;               /* 1 once this CPU finished its trampoline */
    u64  ap_ticks;            /* per-CPU timer tick count                */
    struct task *ap_current;  /* this CPU's running task (NULL = idle)   */
    u64  ap_resched_ipis;     /* count of reschedule IPIs received       */
    struct cpu_local_s *next;
    /* per-CPU ready queue (FIFO; sched.c).  ap_rq_lock is a byte so the
     * __atomic_test_and_set spinlock works; all queue ops run with local
     * IRQs disabled so a timer tick can never deadlock on our own lock. */
    volatile u8 ap_rq_lock;
    struct task *ap_rq_head;
    struct task *ap_rq_tail;
    u32  ap_rq_count;
    struct task *ap_next;     /* preempted task parked while this CPU idles
                                 (never also on the queue - no duplicates) */
    u64  ap_steals;           /* tasks stolen from other CPUs (stats)    */
    /* single-slot per-CPU KERNEL work item (runs in the AP idle loop;
     * kept OUT of the user ready queue so the scheduler never switches
     * to it as if it were a task) */
    void (*ap_kwork_fn)(void *);
    void  *ap_kwork_arg;
    u64 *pml4_current;        /* the page tables CR3 points at on THIS CPU */
    u64  ap_krsp0;            /* current task's kernel stack top (RSP0).
                                 Kept in sync with tss.rsp[0] by
                                 tss_set_rsp0(); the fast syscall/sysret
                                 entry loads it here (the syscall
                                 instruction does not switch stacks). */
    u64  ap_syscall_rsp;      /* scratch: user RSP captured at fast-syscall
                                 entry.  The syscall instruction does not
                                 push the user stack pointer, and every GPR
                                 belongs to the caller, so the entry stashes
                                 RSP here (memory, no register clobbered)
                                 before switching to the kernel stack. */
    volatile u8 ap_yield_pending;   /* sched_yield() set: force immediate
                                       reschedule via self-IPI */
    /* TLB shootdown (vector 63): a CPU that mutates SHARED kernel page
     * tables (direct map, kstack guards) or a live foreign PML4 (wm
     * surface maps) raises this IPI on every other CPU.  The handler
     * flushes (full CR3 reload, or one VA) until gen catches up with req;
     * the sender waits for gen >= req.  A global lock serializes
     * shootdowns so the req/gen pairing stays unambiguous.  Appended at
     * the END on purpose: the fast-syscall asm pins ap_krsp0 /
     * ap_syscall_rsp offsets, which must not move. */
    volatile u64 tlb_req;           /* flush request generation           */
    volatile u64 tlb_gen;           /* completed-flush generation (ack)   */
    volatile u64 tlb_va;            /* VA to invalidate (single flush)    */
    volatile u8  tlb_full;          /* 1 = full flush (CR3 reload)        */
} cpu_local_t;

/* The fast syscall/sysret assembly entry must touch two per-CPU slots by
 * fixed offsets ("gs:[...]"): the current kernel RSP0 and a scratch slot
 * for the user RSP.  We pin both here so the asm can use constants; if the
 * struct is ever reshuffled the build fails loudly instead of silently
 * reading the wrong field. */
#define CPU_LOCAL_RSP0_OFF    152   /* offsetof(cpu_local_t, ap_krsp0)      */
#define CPU_LOCAL_SCRATCH_OFF 160   /* offsetof(cpu_local_t, ap_syscall_rsp)*/
_Static_assert(offsetof(cpu_local_t, ap_krsp0) == CPU_LOCAL_RSP0_OFF,
               "cpu_local_t::ap_krsp0 moved; update CPU_LOCAL_RSP0_OFF");
_Static_assert(offsetof(cpu_local_t, ap_syscall_rsp) == CPU_LOCAL_SCRATCH_OFF,
               "cpu_local_t::ap_syscall_rsp moved; update CPU_LOCAL_SCRATCH_OFF");

/* Kernel-mode only.  Requires GS.base == per-cpu area, which the swapgs
 * dance in isr.asm / user_run_elf guarantees for every path except NMI. */
static ALWAYS_INLINE cpu_local_t *get_cpu_local(void) {
    cpu_local_t *p;
    __asm__ volatile ("movq %%gs:0, %0" : "=r"(p));
    return p;
}
