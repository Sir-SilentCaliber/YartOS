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
 * kernel accesses to user pages (SMAP), clac() re-forbids them. */
static ALWAYS_INLINE void stac(void) { __asm__ volatile("stac" ::: "cc", "memory"); }
static ALWAYS_INLINE void clac(void) { __asm__ volatile("clac" ::: "cc", "memory"); }
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
    if (cpu_has_smep()) { cr4 |= (1ULL << 20); got |= 1; }
    if (cpu_has_smap()) { cr4 |= (1ULL << 21); got |= 2; }
    write_cr4(cr4);
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
} cpu_local_t;

/* Kernel-mode only.  Requires GS.base == per-cpu area, which the swapgs
 * dance in isr.asm / user_run_elf guarantees for every path except NMI. */
static ALWAYS_INLINE cpu_local_t *get_cpu_local(void) {
    cpu_local_t *p;
    __asm__ volatile ("movq %%gs:0, %0" : "=r"(p));
    return p;
}
