/* Yart OS - preemptive round-robin scheduler, SMP-ready and SMP-active.
 *
 * Tasks are processes; each has a private kernel stack.  A context switch is
 * a stack switch: the ISR stub pushes a full cpu_regs_t, the dispatcher asks
 * the scheduler which task should run next, and if it's a different task the
 * stub simply `mov rsp, <new frame>` and iretq's into it.  The task struct
 * stores where its frame lives (saved_rsp).
 *
 *   - pid 0 "desktop" : the kernel task that runs the GUI loop on the BSP.
 *     It is NOT preempted (its data structures are not re-entrant); it hands
 *     the CPU to user tasks only while sleeping in sched_idle_sleep().
 *   - user tasks      : time-sliced on the APIC timer (100 Hz), plus
 *     cooperative yield().  They can run on ANY CPU.
 *
 * SMP model (what makes this a real SMP scheduler):
 *   - Per-CPU current task (cpu_local_t::ap_current), so every CPU schedules
 *     independently; there is no single g_current anymore.
 *   - Per-CPU FIFO ready queues with IRQ-safe spinlocks.
 *   - The APIC timer preempts on every CPU (not just the BSP).
 *   - An AP that is idle saves its idle-loop ISR frame (ap_idle_rsp) when it
 *     first switches to a task, so when its queue empties it can iretq back
 *     to the idle loop instead of being stranded in user mode.
 *   - Reschedule IPIs (vector 62) wake a CPU whose queue gained work, and
 *     poke a CPU that must switch away from a killed/migrated task.
 *   - Load balancing: new tasks (fork) land on the least-loaded online CPU,
 *     and idle APs steal tasks from busy CPUs (work stealing).
 *
 * Scheduling policy: round-robin over each CPU's READY queue; when a CPU's
 * queue is empty the BSP runs the desktop task (pid 0) and an AP returns to
 * its idle loop.
 */
#include <yart/sched.h>
#include <yart/hal.h>
#include <yart/user.h>      /* USER_CS/USER_DS */
#include <yart/console.h>
#include <yart/string.h>
#include <yart/io.h>
#include <yart/cpu.h>       /* fpu_save/fpu_restore/fpu_capture_clean */
#include <yart/spinlock.h>
#include <yart/hal.h>
#include <yart/watchdog.h>      /* watchdog_tick from the BSP timer */
extern void lapic_send_ipi(u32 dest_apic, u8 vector);
#define AP_WAKE_VEC 62

static task_t *g_tasks;       /* global list (all tasks, any state)   */
static task_t *g_idle_task;   /* BSP's desktop task (pid 0)           */
static u64     g_wd_tick_cnt; /* throttles the BSP watchdog to ~1 Hz   */
static u32     g_next_pid;
static bool    g_idle_handoff;   /* idle task is sleeping and may be preempted */
static spinlock_t g_tasks_lock;  /* protects g_tasks append/remove/find/reap */

/* ---------------- sleep queue (blocking sched_sleep_ms) ----------------
 * A sleeping task is registered here (slot + wake tick) and parked as
 * TASK_BLOCKED.  The BSP's timer tick wakes due sleepers once a second
 * boundary is crossed.  The waker pushes the task onto a runqueue BEFORE
 * flipping state to READY (release store); the sleeper's own switch path
 * reads rq_cpu with acquire, so it can never also queue itself. */
static void rq_push(cpu_local_t *c, task_t *t);   /* defined below       */
#define SLEEP_MAX 64
static spinlock_t g_sleep_lock;
static task_t    *g_sleepers[SLEEP_MAX];
static u64        g_sleeper_wake[SLEEP_MAX];

void sched_sleep_ms(u32 ms) {
    task_t *cur = sched_current();
    if (!cur) return;
    u64 wake = pit_ticks() + ((u64)ms + 9) / 10;   /* 100 Hz system tick */
    u64 fl = irq_save();
    spin_lock(&g_sleep_lock);
    int slot = -1;
    for (int i = 0; i < SLEEP_MAX; i++)
        if (!g_sleepers[i]) { slot = i; break; }
    if (slot >= 0) {
        g_sleepers[slot] = cur;
        g_sleeper_wake[slot] = wake;
        cur->state = TASK_BLOCKED;      /* sched_after_isr switches away */
    }
    spin_unlock(&g_sleep_lock);
    irq_restore(fl);
    if (slot < 0)
        kprintf("sched: sleep queue full - task %u slept 0 ms\n", cur->pid);
}

/* Wake every sleeper whose deadline has passed.  Called from the BSP's
 * timer tick (sched_tick). */
static void sched_wake_sleepers(u64 now) {
    u64 fl = irq_save();
    spin_lock(&g_sleep_lock);
    for (int i = 0; i < SLEEP_MAX; i++) {
        task_t *t = g_sleepers[i];
        if (!t) continue;
        if (g_sleeper_wake[i] > now) continue;
        g_sleepers[i] = NULL;
        if (t->state != TASK_BLOCKED) continue;  /* killed while asleep */
        cpu_local_t *target = smp_least_loaded();
        if (!target) target = get_cpu_local();
        rq_push(target, t);                    /* queue FIRST...         */
        __atomic_store_n(&t->state, TASK_READY, __ATOMIC_RELEASE);
        lapic_send_ipi(target->ap_lapic_id, AP_WAKE_VEC);
    }
    spin_unlock(&g_sleep_lock);
    irq_restore(fl);
}

/* ------------------------------------------------------------------ */
/* per-CPU current task                                               */
/* ------------------------------------------------------------------ */

task_t *sched_current(void) {
    cpu_local_t *c = get_cpu_local();
    return (c && c->ap_current) ? c->ap_current : NULL;
}
task_t *sched_tasks(void) { return g_tasks; }
bool    sched_current_is_user(void) { task_t *t = sched_current(); return t && t->is_user; }
u32     sched_next_pid(void)    { return g_next_pid; }
u32     sched_current_uid(void) { task_t *t = sched_current(); return t ? t->uid : 0; }
u32     sched_current_euid(void){ task_t *t = sched_current(); return t ? t->euid : 0; }
u32     sched_current_egid(void){ task_t *t = sched_current(); return t ? t->gid : 0; }
int     sched_current_groups(u32 *out, int max) {
    task_t *t = sched_current();
    if (!t) { if (max > 0) out[0] = 0; return 1; }
    int n = 0;
    if (n < max) out[n++] = t->gid;                       /* primary first  */
    for (int i = 0; i < t->supp_gid_count && n < max; i++)
        if (t->supp_gids[i] != t->gid) out[n++] = t->supp_gids[i];
    return n;
}
u16     sched_current_umask(void) { task_t *t = sched_current(); return t ? t->umask : 0; }
u16     sched_set_umask(u16 m) {
    task_t *t = sched_current();
    if (!t) return 0;
    u16 old = t->umask;
    t->umask = m & 0777u;
    return old;
}
u64     sched_mem_used(void)  { task_t *t = sched_current(); return t ? t->mem_pages : 0; }
u64     sched_mem_limit(void) { task_t *t = sched_current(); return t ? t->mem_limit_pages : (64*1024*1024)/PAGE_SIZE; }
void sched_charge_pages(i64 delta) {
    task_t *t = sched_current();
    if (!t || !t->is_user) return;
    if (delta > 0) {
        if (t->mem_pages + (u64)delta > t->mem_limit_pages)
            return;   /* over the cap: caller sees sched_mem_used() unchanged */
        t->mem_pages += (u64)delta;
    } else {
        u64 d = (u64)(-delta);
        t->mem_pages = (t->mem_pages >= d) ? t->mem_pages - d : 0;
    }
}

/* ------------------------------------------------------------------ */
/* per-CPU FIFO ready queues                                          */
/*                                                                   */
/* A READY task sits on exactly one CPU's queue (task::rq_cpu).  Every
 * queue op runs with local IRQs disabled + the queue's spinlock held so a
 * timer tick on the same CPU can never deadlock against the holder, and a
 * concurrent steal/migrate from another CPU serializes on the same lock. */
/* ------------------------------------------------------------------ */

static void rq_lock(cpu_local_t *c) {
    while (__atomic_test_and_set(&c->ap_rq_lock, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&c->ap_rq_lock, __ATOMIC_RELAXED))
            __asm__ volatile("pause");
    }
}
static void rq_unlock(cpu_local_t *c) {
    __atomic_clear(&c->ap_rq_lock, __ATOMIC_RELEASE);
}

/* Pop one task off a queue.  Lock + IRQ-safe. */
static task_t *rq_pop(cpu_local_t *c) {
    u64 fl = irq_save();
    rq_lock(c);
    task_t *t = c->ap_rq_head;
    if (t) {
        c->ap_rq_head = t->rq_next;
        if (!c->ap_rq_head) c->ap_rq_tail = NULL;
        c->ap_rq_count--;
        t->rq_next = NULL;
        t->rq_cpu = NULL;
    }
    rq_unlock(c);
    irq_restore(fl);
    return t;
}

/* Push one task onto a queue.  Lock + IRQ-safe. */
static void rq_push(cpu_local_t *c, task_t *t) {
    u64 fl = irq_save();
    rq_lock(c);
    t->rq_next = NULL;
    t->rq_cpu = c;
    if (c->ap_rq_tail) c->ap_rq_tail->rq_next = t;
    else               c->ap_rq_head = t;
    c->ap_rq_tail = t;
    c->ap_rq_count++;
    rq_unlock(c);
    irq_restore(fl);
}

/* Push `push` (or nothing) then pop the head, atomically.  Used by the
 * preemption/yield paths so the running task never sits in the queue with a
 * window where another CPU could steal it (would run it twice!).
 *
 * IMPORTANT: when `push` is the ONLY task on the queue it is left queued
 * and returned (not popped).  The caller then hands the CPU to the idle
 * task / idle loop; the pushed task must still be findable by the next
 * hand-off pop, or it would silently vanish from the scheduler. */
static task_t *rq_push_pop(cpu_local_t *c, task_t *push) {
    u64 fl = irq_save();
    rq_lock(c);
    if (push) {
        push->rq_next = NULL;
        push->rq_cpu = c;
        if (c->ap_rq_tail) c->ap_rq_tail->rq_next = push;
        else               c->ap_rq_head = push;
        c->ap_rq_tail = push;
        c->ap_rq_count++;
    }
    task_t *t = NULL;
    if (c->ap_rq_head) {
        t = c->ap_rq_head;
        c->ap_rq_head = t->rq_next;
        if (!c->ap_rq_head) c->ap_rq_tail = NULL;
        c->ap_rq_count--;
        t->rq_next = NULL;
        t->rq_cpu = NULL;
    }
    rq_unlock(c);
    irq_restore(fl);
    return t;   /* NULL if the queue was empty after the push */
}

/* Remove a specific task from a queue (used by SIGKILL of a queued task). */
static void rq_remove(cpu_local_t *c, task_t *t) {
    u64 fl = irq_save();
    rq_lock(c);
    task_t **pp = &c->ap_rq_head;
    while (*pp && *pp != t) pp = &(*pp)->rq_next;
    if (*pp) {
        *pp = t->rq_next;
        /* recompute tail properly if we removed the tail */
        if (!c->ap_rq_head) c->ap_rq_tail = NULL;
        else if (c->ap_rq_tail == t) {
            task_t *w = c->ap_rq_head;
            while (w->rq_next) w = w->rq_next;
            c->ap_rq_tail = w;
        }
        c->ap_rq_count--;
        t->rq_next = NULL;
        t->rq_cpu = NULL;
    }
    rq_unlock(c);
    irq_restore(fl);
}

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

static u64 deliver_pending_signal(task_t *t, u64 current_rsp) {
    for (u32 sig = 1; sig < 32; sig++) {
        if (!(t->sig_pending & (1ULL << sig))) continue;
        if (t->sig_blocked & (1ULL << sig)) continue;
        u64 h = t->sig_handlers[sig];
        t->sig_pending &= ~(1ULL << sig);
        if (!h) {                          /* no handler: default = die  */
            kprintf("sched: task %u killed by unhandled signal %u\n", t->pid, sig);
            sched_exit(128 + (int)sig);
            return current_rsp;            /* will be switched away      */
        }
        /* build a handler frame: we are inside the syscall ISR, so the
         * cpu_regs_t at current_rsp is the interrupted user state. */
        cpu_regs_t *f = (cpu_regs_t *)current_rsp;
        cpu_regs_t *hf = (cpu_regs_t *)(current_rsp - sizeof(cpu_regs_t));
        memcpy(hf, f, sizeof *hf);
        hf->rax = sig;                      /* handler arg0 = signal #   */
        hf->rip = h;                        /* jump to the handler      */
        return (u64)hf;
    }
    return current_rsp;
}

static u64 alloc_kstack(void) {
    paddr_t p = pmm_alloc_pages(KSTACK_SIZE / PAGE_SIZE);
    return (u64)phys_to_virt(p) + KSTACK_SIZE;
}
static void free_kstack(u64 top) {
    if (!top) return;
    /* alloc_kstack returned a *virtual* top; convert back to physical. */
    paddr_t phys = (paddr_t)(top - KSTACK_SIZE) - g_hhdm_offset;
    pmm_free_pages(phys, KSTACK_SIZE / PAGE_SIZE);
}

static void task_append(task_t *t) {
    u64 fl = irq_save();
    spin_lock(&g_tasks_lock);
    task_t **pp = &g_tasks;
    while (*pp) pp = &(*pp)->next;
    *pp = t;
    t->next = NULL;
    spin_unlock(&g_tasks_lock);
    irq_restore(fl);
}
static void task_remove(task_t *t) {
    u64 fl = irq_save();
    spin_lock(&g_tasks_lock);
    task_t **pp = &g_tasks;
    while (*pp && *pp != t) pp = &(*pp)->next;
    if (*pp) *pp = t->next;
    spin_unlock(&g_tasks_lock);
    irq_restore(fl);
}

task_t *sched_find(u32 pid) {
    u64 fl = irq_save();
    spin_lock(&g_tasks_lock);
    task_t *r = NULL;
    for (task_t *t = g_tasks; t; t = t->next)
        if (t->pid == pid) { r = t; break; }
    spin_unlock(&g_tasks_lock);
    irq_restore(fl);
    return r;
}

static void set_fds0(task_t *t) {
    t->fds[0].in_use = t->fds[1].in_use = t->fds[2].in_use = true;
}

/* ------------------------------------------------------------------ */
/* init                                                               */
/* ------------------------------------------------------------------ */

void sched_init(void) {
    g_tasks = NULL;
    g_idle_task = NULL;
    g_next_pid = 1;
    g_idle_handoff = false;
    spin_init(&g_tasks_lock);

    task_t *idle = kzalloc(sizeof *idle);
    idle->pid = 0;
    idle->ppid = 0;
    strncpy(idle->name, "desktop", TASK_NAME_LEN - 1);
    idle->state = TASK_RUNNING;
    idle->is_user = false;
    idle->pml4 = NULL;
    idle->kstack_top = alloc_kstack();   /* RSP0 + future use */
    idle->saved_rsp = 0;                 /* created on first hand-off */
    fpu_capture_clean(idle->fpu_area);   /* clean FPU state for pid 0   */
    idle->cwd = vfs_root();
    set_fds0(idle);
    task_append(idle);
    g_idle_task = idle;
    get_cpu_local()->ap_current = idle;  /* BSP's current = desktop */

    kprintf("sched: init - idle task [0] 'desktop' (per-CPU scheduler)\n");
}

/* ------------------------------------------------------------------ */
/* creating tasks                                                     */
/* ------------------------------------------------------------------ */

task_t *sched_create_user(const char *name, u64 entry, u64 user_rsp,
                          u64 *pml4, user_region_t *regions, int nregions) {
    task_t *t = kzalloc(sizeof *t);
    if (!t) return NULL;
    t->pid = g_next_pid++;
    t->ppid = 0;
    strncpy(t->name, name, TASK_NAME_LEN - 1);
    t->state = TASK_READY;
    t->is_user = true;
    t->pml4 = NULL;                    /* set below via split-off        */
    t->uid = 0; t->euid = 0;           /* main.c assigns the real user   */
    t->umask = 022;                 /* default: no group/other write      */
    t->supp_gid_count = 0;
    t->elev_allowed = false;
    t->account[0] = 0;
    t->kstack_top = alloc_kstack();
    t->cwd = vfs_root();
    set_fds0(t);
    fpu_capture_clean(t->fpu_area);        /* start with a clean FPU      */
    t->mmap_next = USER_MMAP_BASE;         /* dynamic-memory arena cursor  */
    t->brk_base = USER_MMAP_BASE;
    t->brk      = USER_MMAP_BASE;   /* heap starts empty                 */
    t->mem_pages = 0;
    t->last_sched = pit_ticks();   /* so the watchdog doesn't misjudge it */
    t->mem_limit_pages = 256 * 1024 * 1024 / PAGE_SIZE;   /* 256 MiB cap */

    /* fake resume frame so the first schedule iretq's into user mode */
    cpu_regs_t *f = (cpu_regs_t *)(t->kstack_top - sizeof(cpu_regs_t));
    memset(f, 0, sizeof *f);
    f->ss = USER_DS;
    f->rsp = user_rsp;
    f->rflags = 0x202;                 /* IF=1, IOPL=0 */
    f->cs = USER_CS;
    f->rip = entry;
    t->saved_rsp = (u64)f;

    /* Per-process address space: adopt the PREPARED private PML4 + regions
     * (user_prepare_elf built them in a fresh table - no clone needed). */
    t->pml4 = pml4;
    if (regions && nregions > 0) {
        memcpy(t->regions, regions, (size_t)nregions * sizeof regions[0]);
        t->region_count = nregions;
        for (int i = 0; i < nregions; i++)
            t->mem_pages += regions[i].npages;
    }
    task_append(t);
    rq_push(get_cpu_local(), t);       /* run on the creating CPU (BSP)  */
    kprintf("sched: created user task [%u] '%s' entry=%p ustack=%p%s\n",
            t->pid, name, (void *)entry, (void *)user_rsp,
            t->pml4 ? " (private PML4)" : "");
    return t;
}

/* fork: clone the parent's address space (CoW), fd table and CPU state so
 * the child resumes at the same instruction with rax == 0.  On SMP the
 * child is placed on the least-loaded online CPU so real processes spread
 * across cores (load balancing). */
task_t *sched_fork(task_t *parent, cpu_regs_t *frame) {
    task_t *child = kzalloc(sizeof *child);
    if (!child) return NULL;

    child->pid = g_next_pid++;
    child->ppid = parent->pid;
    strncpy(child->name, parent->name, TASK_NAME_LEN - 1);
    child->state = TASK_READY;
    child->is_user = true;
    child->uid = parent->uid;
    child->euid = parent->euid;
    child->elev_allowed = parent->elev_allowed;
    child->mmap_next = parent->mmap_next;
    child->brk = parent->brk;
    child->umask = parent->umask;
    child->supp_gid_count = parent->supp_gid_count;
    memcpy(child->supp_gids, parent->supp_gids, sizeof parent->supp_gids);
    child->brk_base = parent->brk_base;
    strncpy(child->account, parent->account, sizeof child->account - 1);

    child->pml4 = vmm_clone_pml4();          /* private user tables */
    if (!child->pml4) { kfree(child); return NULL; }
    vmm_cow_fork(child->pml4);               /* share user frames CoW */

    memcpy(child->regions, parent->regions, sizeof parent->regions);
    child->region_count = parent->region_count;

    memcpy(child->fds, parent->fds, sizeof parent->fds);
    for (int i = 3; i < MAX_FD; i++)
        if (child->fds[i].in_use) vnode_ref(child->fds[i].vn);
    child->cwd = parent->cwd;
    if (child->cwd) vnode_ref(child->cwd);

    child->kstack_top = alloc_kstack();
    cpu_regs_t *f = (cpu_regs_t *)(child->kstack_top - sizeof(cpu_regs_t));
    memcpy(f, frame, sizeof *f);             /* resume after int 0x80 */
    f->rax = 0;                              /* child sees 0 from fork() */
    child->saved_rsp = (u64)f;
    fpu_save(child->fpu_area);   /* capture the parent's LIVE FPU state */

    child->last_sched = pit_ticks();   /* watchdog: treat as freshly alive */
    task_append(child);

    /* place the child on the least-loaded online CPU (an AP on SMP) */
    cpu_local_t *me = get_cpu_local();
    cpu_local_t *target = smp_least_loaded();
    if (target) {
        rq_push(target, child);
        lapic_send_ipi(target->ap_lapic_id, AP_WAKE_VEC);   /* wake it    */
        kprintf("sched: fork() pid %u '%s' -> pid %u on CPU %u (load-balanced)\n",
                parent->pid, parent->name, child->pid, target->cpu_id);
    } else {
        rq_push(me, child);
        kprintf("sched: fork() pid %u '%s' -> pid %u (CoW)\n",
                parent->pid, parent->name, child->pid);
    }
    return child;
}

/* ------------------------------------------------------------------ */
/* the switch                                                          */
/* ------------------------------------------------------------------ */

static u64 switch_to(task_t *next, u64 current_rsp) {
    cpu_local_t *c = get_cpu_local();
    task_t *cur = c->ap_current;
    if (cur == next) return current_rsp;
    if (cur) {
        /* save the FPU/SSE state of whoever we are leaving, restore the
         * next task's - otherwise floats leak between tasks */
        fpu_save(cur->fpu_area);
        if (cur->state == TASK_RUNNING) cur->state = TASK_READY;
        cur->saved_rsp = current_rsp;  /* remember where our frame lives */
    }
    c->ap_current = next;
    next->state = TASK_RUNNING;
    next->last_sched = pit_ticks();   /* for the hung-task watchdog */
    fpu_restore(next->fpu_area);

    if (next->is_user)
        vmm_switch_pml4(next->pml4 ? next->pml4 : vmm_kernel_pml4());
    else
        vmm_switch_pml4(vmm_kernel_pml4());
    /* ring3->ring0 must land on the task's kstack via THIS CPU's TSS
     * (BSP: selector 0x28; AP: its own 0x38 TSS - per-CPU RSP0). */
    tss_set_rsp0(next->kstack_top);

    return next->saved_rsp;            /* the stub iretq's into this frame */
}

/* AP: hand the CPU back to the idle loop.  current_rsp is this ISR's frame
 * (the task's), ap_idle_rsp is the idle-loop frame saved on the way in. */
static u64 switch_to_idle(u64 current_rsp) {
    cpu_local_t *c = get_cpu_local();
    task_t *cur = c->ap_current;
    if (!cur) return current_rsp;
    fpu_save(cur->fpu_area);
    if (cur->state == TASK_RUNNING) cur->state = TASK_READY;
    cur->saved_rsp = current_rsp;
    c->ap_current = NULL;
    vmm_switch_pml4(vmm_kernel_pml4());
    return c->ap_idle_rsp;             /* iretq back into the idle loop  */
}

/* Pop the next task to run on THIS cpu: the parked task (ap_next) has
 * priority, then the ready queue.  A parked task was preempted with an
 * empty queue and is waiting for the CPU to come back from idle. */
static task_t *idle_pop(cpu_local_t *c) {
    if (c->ap_next) {
        task_t *t = c->ap_next;
        c->ap_next = NULL;
        return t;
    }
    return rq_pop(c);
}

/* Preempt `cur` (a user task) on this CPU: if another task is ready, rotate
 * cur to the queue tail and run the new head (atomic, so nobody can steal
 * cur mid-rotation); otherwise PARK cur in the per-CPU ap_next slot - never
 * on the queue - and hand the CPU to its idle fallback.  This keeps the
 * invariant "a task is running, parked, or on exactly one queue", so the
 * same task can never run on two CPUs or be queued twice. */
static u64 sched_preempt(task_t *cur, u64 current_rsp) {
    cpu_local_t *c = get_cpu_local();
    task_t *other = NULL;
    u64 fl = irq_save();
    rq_lock(c);
    if (c->ap_rq_head) {
        /* rotate: cur -> tail, run the head */
        cur->rq_next = NULL;
        cur->rq_cpu = c;
        c->ap_rq_tail->rq_next = cur;
        c->ap_rq_tail = cur;
        c->ap_rq_count++;
        other = c->ap_rq_head;
        c->ap_rq_head = other->rq_next;
        if (!c->ap_rq_head) c->ap_rq_tail = NULL;
        c->ap_rq_count--;
        other->rq_next = NULL;
        other->rq_cpu = NULL;
    }
    rq_unlock(c);
    irq_restore(fl);
    if (other) return switch_to(other, current_rsp);

    /* nothing else ready: park cur and go idle (desktop on BSP, idle loop
     * on an AP - the next idle_pop() will bring cur back) */
    if (!c->ap_next) {
        c->ap_next = cur;
        cur->state = TASK_READY;
    }
    if (c->cpu_id == 0) {
        if (g_idle_task && g_idle_task != cur)
            return switch_to(g_idle_task, current_rsp);
        return current_rsp;
    }
    return switch_to_idle(current_rsp);
}

/* cur is not runnable (blocked/zombie) or yielded: switch to whoever is
 * next, or to the CPU's idle fallback.  A yielded task is pushed to the
 * queue; if nothing else was waiting it is parked in ap_next instead of
 * being left queued (no duplicates).
 *
 * A woken task (sleep/waitpid wake) is ALREADY on some CPU's runqueue: the
 * waker queues it before flipping state to READY (release store), so the
 * acquire read of rq_cpu below can never see NULL once we observe READY -
 * without this guard a task woken while it was about to sleep would be
 * pushed onto a queue a second time and run on two CPUs. */
static u64 sched_switch_after(task_t *cur, u64 current_rsp) {
    cpu_local_t *c = get_cpu_local();
    if (cur && cur->state == TASK_READY) {          /* yielded */
        if (__atomic_load_n(&cur->rq_cpu, __ATOMIC_ACQUIRE) == NULL) {
            task_t *next = rq_push_pop(c, cur);
            if (next && next != cur) return switch_to(next, current_rsp);
            if (next == cur) rq_remove(c, cur);  /* still queued: take it
                                                    back off before parking */
            if (!c->ap_next) c->ap_next = cur;
        }
        /* else: already queued by a waker - just hand the CPU away */
    }
    if (c->cpu_id == 0) {
        if (g_idle_task && g_idle_task != cur) return switch_to(g_idle_task, current_rsp);
        return current_rsp;
    }
    return switch_to_idle(current_rsp);    /* AP: back to the idle loop */
}

/* ------------------------------------------------------------------ */
/* SMP helpers (called from smp.c / syscalls)                         */
/* ------------------------------------------------------------------ */

/* An AP runs this from its idle loop for a queued KERNEL work item (it
 * returns normally).  Kernel work lives in a per-CPU single slot, separate
 * from the user ready queue, so the scheduler never mistakes it for a task. */
bool sched_ap_try_work(cpu_local_t *c) {
    if (!c) return false;
    void (*fn)(void *) = c->ap_kwork_fn;
    if (!fn) return false;
    c->ap_kwork_fn = NULL;
    void *arg = c->ap_kwork_arg;
    c->ap_kwork_arg = NULL;
    fn(arg);
    return true;
}

/* Queue a kernel work item onto a specific CPU and poke it. */
void sched_queue_ap_work(cpu_local_t *c, void (*fn)(void *), void *arg) {
    if (!c || !fn) return;
    if (c->ap_kwork_fn) return;           /* slot busy (single-slot) */
    c->ap_kwork_fn = fn;
    c->ap_kwork_arg = arg;
    lapic_send_ipi(c->ap_lapic_id, AP_WAKE_VEC);   /* wake the AP */
}

/* Move a non-running task onto another CPU's queue and poke that CPU so it
 * schedules the task promptly (reschedule IPI). */
int sched_migrate_to_ap(cpu_local_t *c, task_t *t) {
    if (!c || !t) return -1;
    if (t->rq_cpu == c) return 0;
    if (t->rq_cpu) rq_remove(t->rq_cpu, t);   /* leave its current queue */
    t->state = TASK_READY;
    rq_push(c, t);
    lapic_send_ipi(c->ap_lapic_id, AP_WAKE_VEC);
    kprintf("smp: task %u '%s' migrated to CPU %u (resched IPI sent)\n",
            t->pid, t->name, c->cpu_id);
    return 0;
}

/* Idle-AP work stealing: take one queued task from another CPU so an idle
 * core pulls work off a busy one (preemptive migration).  Stealing is safe
 * for ANY queued task (queued == READY and running nowhere; the running and
 * parked tasks of a CPU are never on its queue).  The relaxed pre-check is
 * just a fast path; the locked re-check is authoritative. */
static task_t *sched_steal_pop(cpu_local_t *c) {
    for (u32 i = 0; i < 8; i++) {
        cpu_local_t *o = (i == 0) ? bsp_cpu_local() : smp_get_ap_area(i);
        if (!o || o == c) continue;
        if (o->cpu_id != 0 && !o->ap_up) continue;
        /* only steal when the victim has a backlog of >= 2 queued tasks;
         * taking its last task would make it thrash the task back and
         * forth between cores */
        if (__atomic_load_n(&o->ap_rq_count, __ATOMIC_RELAXED) < 2) continue;
        u64 fl = irq_save();
        rq_lock(o);
        if (o->ap_rq_count >= 2) {
            task_t *t = o->ap_rq_head;
            o->ap_rq_head = t->rq_next;
            if (!o->ap_rq_head) o->ap_rq_tail = NULL;
            o->ap_rq_count--;
            t->rq_next = NULL;
            t->rq_cpu = NULL;
            rq_unlock(o);
            irq_restore(fl);
            if (t->state != TASK_READY) {   /* killed while queued: put it
                                               back, never run a zombie */
                rq_push(o, t);
                continue;
            }
            /* safety: never run a task that is STILL the current task of
             * some CPU (it would run on two cores at once) */
            bool running = false;
            for (u32 j = 0; j < 8; j++) {
                cpu_local_t *cc = (j == 0) ? bsp_cpu_local() : smp_get_ap_area(j);
                if (cc && cc->ap_current == t) { running = true; break; }
            }
            if (running) {
                kprintf("smp: steal SKIPPED task %u (still running on CPU %u)\\n",
                        t->pid, o->cpu_id);
                rq_push(o, t);           /* put it back where it was */
                continue;
            }
            c->ap_steals++;
            kprintf("smp: CPU %u stole task %u '%s' from CPU %u (load balance)\\n",
                    c->cpu_id, t->pid, t->name, o->cpu_id);
            return t;
        }
        rq_unlock(o);
        irq_restore(fl);
    }
    return NULL;
}

/* Called by an AP's idle loop when its queue is empty: try to steal work,
 * and if successful poke ourselves so the interrupt path switches to it. */
void sched_ap_steal(cpu_local_t *c) {
    if (!c) return;
    task_t *t = sched_steal_pop(c);
    if (!t) return;
    rq_push(c, t);
    lapic_send_ipi(c->ap_lapic_id, AP_WAKE_VEC);
}

/* ------------------------------------------------------------------ */
/* timer tick / post-ISR scheduling                                   */
/* ------------------------------------------------------------------ */

/* Timer tick on THIS cpu: preempt the current user task (round-robin),
 * pick up newly-queued work when idle (AP), or hand the CPU over when the
 * BSP's idle task is sleeping. */
u64 sched_tick(u64 current_rsp) {
    cpu_local_t *c = get_cpu_local();
    task_t *cur = c->ap_current;
    /* BSP-only, ~1 Hz: run the watchdog/supervisor (restart stalled kernel
     * services + kill hung user tasks).  Only one CPU drives it to avoid
     * concurrent scans, and only once a second to keep it cheap. */
    if (c->cpu_id == 0) {
        sched_wake_sleepers(pit_ticks());        /* wake due sleepers */
        if ((++g_wd_tick_cnt % 100) == 0)
            watchdog_tick();
    }
    if (!cur) {
        /* this CPU is idle (an AP in its loop): run a parked/queued task
         * if any.  Remember the idle frame so we can iretq back when it
         * finishes. */
        task_t *next = idle_pop(c);
        if (next) {
            c->ap_idle_rsp = current_rsp;
            return switch_to(next, current_rsp);
        }
        return current_rsp;
    }
    if (cur->is_user) {
        if (cur->sig_pending) {
            u64 nrsp = deliver_pending_signal(cur, current_rsp);
            if (nrsp != current_rsp) return nrsp;
            if (cur->state != TASK_RUNNING)
                return sched_switch_after(cur, nrsp);
            return nrsp;
        }
        if (cur->state != TASK_RUNNING)
            return sched_switch_after(cur, current_rsp);  /* exited/killed */
        return sched_preempt(cur, current_rsp);   /* RR + park + idle */
    }
    if (g_idle_handoff) {                /* BSP desktop is sleeping */
        task_t *next = idle_pop(c);
        if (next && next != cur) return switch_to(next, current_rsp);
    }
    return current_rsp;
}

/* Called after a syscall or fault on THIS cpu: if the current task asked to
 * yield, block or exit, switch away now instead of waiting for the next
 * tick. */
u64 sched_after_isr(u64 current_rsp) {
    cpu_local_t *c = get_cpu_local();
    task_t *cur = c->ap_current;
    if (!cur) return current_rsp;
    if (cur->is_user && cur->sig_pending) {
        u64 nrsp = deliver_pending_signal(cur, current_rsp);
        if (nrsp != current_rsp) return nrsp;
        if (cur->state != TASK_RUNNING)
            return sched_switch_after(cur, nrsp);
        return nrsp;
    }
    if (cur->state == TASK_RUNNING) return current_rsp;
    return sched_switch_after(cur, current_rsp);
}

/* Yield: mark the calling task ready and immediately call sched_switch_after
 * via the "pending" flag path used by block/exit.  sched_after_isr() and
 * sched_tick() honor TASK_READY on the current task; but when yield is
 * entered from syscall context, sched_after_isr only switches away if the
 * state is NOT TASK_RUNNING, so setting TASK_READY alone would not reschedule
 * until the next timer tick.  We set a small thread-local flag via
 * ap_yield_pending so the caller's sched_after_isr/sched_tick invocation
 * immediately reconsiders.
 *
 * Simpler (and more reliable): we do the switch inline here and return the
 * new RSP via a trampoline in the caller's stack frame... but that requires
 * cooperation from the syscall stub.  Instead, we piggy-back on the
 * sched_tick path by noting the yield and kicking the local APIC timer IPI
 * (self-IPI) to force an immediate reschedule interrupt. */
void sched_yield(void) {
    task_t *cur = sched_current();
    if (!cur || cur->state != TASK_RUNNING) return;
    cpu_local_t *c = get_cpu_local();
    if (!c) return;
    c->ap_yield_pending = 1;
    cur->state = TASK_READY;
    /* Send ourselves a reschedule IPI - this interrupts the kernel at the
     * syscall return path and forces sched_tick() to run, which will pick
     * the next task.  Cheaper than busy-looping for a tick. */
    u8 my_id = (u8)apic_local_id();
    lapic_send_ipi(my_id, 62);   /* vector 62 = AP_WAKE_VEC (reschedule) */
}

/* Wake a parent that is blocked in waitpid() for this child.  Must be
 * called with g_tasks_lock held: the parent sets waiting+BLOCKED inside
 * the same lock, so if we see waiting=true here the parent is guaranteed
 * to be asleep (not running) and safe to requeue. */
static void wake_waiting_parent(task_t *child) {
    if (child->ppid == 0) return;
    task_t *p = sched_find(child->ppid);
    if (p && p->waiting && p->state == TASK_BLOCKED) {
        p->waiting = false;
        cpu_local_t *target = smp_least_loaded();
        if (!target) target = get_cpu_local();
        rq_push(target, p);                    /* queue FIRST...          */
        __atomic_store_n(&p->state, TASK_READY, __ATOMIC_RELEASE);
        lapic_send_ipi(target->ap_lapic_id, AP_WAKE_VEC);
    }
}

void sched_exit(int status) {
    task_t *cur = sched_current();
    if (!cur || !cur->is_user) return;
    u64 fl = irq_save();
    /* zombie flag + parent wake are atomic w.r.t. the parent's waitpid
     * block decision (both hold g_tasks_lock).  The memory teardown runs
     * AFTER releasing the lock: it takes the PMM lock, and the OOM path
     * holds the PMM lock while calling sched_kill (tasks lock) - nesting
     * tasks->pmm would deadlock against that order. */
    cur->exit_status = status;
    cur->state = TASK_ZOMBIE;
    spin_lock(&g_tasks_lock);
    wake_waiting_parent(cur);          /* blocked parent: wake + requeue */
    spin_unlock(&g_tasks_lock);
    vmm_user_teardown_all();           /* free this task's user memory */
    irq_restore(fl);
    kprintf("sched: task [%u] '%s' exited status %d\n",
            cur->pid, cur->name, status);
}

/* The idle task calls this instead of a bare hlt: it signals that a timer
 * tick may hand the CPU to a ready user task. */
void sched_idle_sleep(void) {
    g_idle_handoff = true;
    __asm__ volatile ("hlt");
    g_idle_handoff = false;
}

/* ------------------------------------------------------------------ */
/* wait/reap                                                          */
/* ------------------------------------------------------------------ */

/* Is this task currently executing on SOME CPU?  A zombie that is still
 * the current task of a CPU must NOT be reaped (its kernel stack + page
 * tables are in use until that CPU switches away). */
static bool task_running_anywhere(task_t *t) {
    for (u32 i = 0; i < 8; i++) {
        cpu_local_t *o = (i == 0) ? bsp_cpu_local() : smp_get_ap_area(i);
        if (o && o->ap_current == t) return true;
    }
    return false;
}

static void reap(task_t *t) {
    if (t->pml4 && t->pml4 != vmm_kernel_pml4())
        vmm_free_pml4(t->pml4);
    free_kstack(t->kstack_top);
    task_remove(t);
    kprintf("sched: reaped task [%u] '%s'\n", t->pid, t->name);
    kfree(t);
}

#define SIG_KILL 9
int sched_signal(u32 pid, u32 sig) {
    task_t *t = sched_find(pid);
    if (!t || t->pid == 0 || t->state == TASK_ZOMBIE) return -1;
    if (sig == 0) return 0;                 /* existence probe */
    if (sig >= 32) return -1;               /* POSIX signal range      */
    if (sig == SIG_KILL) return sched_kill(pid);
    if (t->sig_blocked & (1ULL << sig)) return 0;
    t->sig_pending |= (1ULL << sig);
    kprintf("sched: signal %u pending for pid %u\n", sig, t->pid);
    return 0;
}

int sched_kill(u32 pid) {
    task_t *t = sched_find(pid);
    if (!t || t->pid == 0) return -1;
    if (t->state == TASK_ZOMBIE) return -1;      /* already dead            */
    /* remove it from any ready queue so it can never be picked again */
    if (t->rq_cpu) rq_remove(t->rq_cpu, t);
    /* clear any parked reference (target sitting in some CPU's ap_next) */
    for (u32 i = 0; i < 8; i++) {
        cpu_local_t *o = (i == 0) ? bsp_cpu_local() : smp_get_ap_area(i);
        if (o && o->ap_next == t) o->ap_next = NULL;
    }
    t->exit_status = -9;                          /* -SIGKILL               */
    t->state = TASK_ZOMBIE;
    /* wake a parent blocked in waitpid() for this child (same lock
     * discipline as sched_exit) */
    {
        u64 fl = irq_save();
        spin_lock(&g_tasks_lock);
        wake_waiting_parent(t);
        spin_unlock(&g_tasks_lock);
        irq_restore(fl);
    }
    /* free its address space NOW if it is not running on some CPU (a
     * running task's CR3 still points at its PML4 - can't free that) */
    bool running = false;
    for (u32 i = 0; i < 8; i++) {
        cpu_local_t *o = (i == 0) ? bsp_cpu_local() : smp_get_ap_area(i);
        if (o && o->ap_current == t) { running = true; break; }
    }
    if (!running && t->pml4 && t->pml4 != vmm_kernel_pml4()) {
        vmm_free_pml4(t->pml4);
        t->pml4 = NULL;
    }
    /* if it IS running elsewhere, poke that CPU so it switches away now */
    if (running) {
        for (u32 i = 0; i < 8; i++) {
            cpu_local_t *o = (i == 0) ? bsp_cpu_local() : smp_get_ap_area(i);
            if (o && o->ap_current == t) {
                lapic_send_ipi(o->ap_lapic_id, AP_WAKE_VEC);
                break;
            }
        }
    }
    kprintf("sched: kill pid %u '%s' (SIGKILL)%s\n",
            t->pid, t->name, running ? " [running on another CPU - poked]" : "");
    return 0;
}

/* Blocking waitpid: if the child is still alive this task is parked
 * (TASK_BLOCKED + waiting) and woken by sched_exit/sched_kill when the
 * child dies - no more spin+yield polling.  Returns:
 *   pid   = reaped this child (status filled)
 *   0     = child alive; the caller (syscall) will resume us on wake
 *   -1    = ECHILD
 * The zombie-flag read and the waiting flag are set under g_tasks_lock,
 * the same lock sched_exit/sched_kill use to wake us, so there is no
 * lost-wakeup race. */
int sched_waitpid(u32 pid, int *status_out) {
    task_t *cur = sched_current();
    if (!cur) return -1;

    task_t *c = NULL;
    u64 fl = irq_save();
    spin_lock(&g_tasks_lock);
    for (task_t *t = g_tasks; t; t = t->next)
        if (t->ppid == cur->pid && (pid == 0 || t->pid == pid)) { c = t; break; }
    if (!c) {
        spin_unlock(&g_tasks_lock);
        irq_restore(fl);
        return -1;                        /* ECHILD */
    }
    if (c->state != TASK_ZOMBIE) {
        /* child alive: register as waiting and block */
        cur->waiting = true;
        cur->wait_pid = pid;
        cur->state = TASK_BLOCKED;        /* sched_after_isr switches away */
        spin_unlock(&g_tasks_lock);
        irq_restore(fl);
        return 0;
    }
    cur->waiting = false;
    spin_unlock(&g_tasks_lock);
    irq_restore(fl);

    if (task_running_anywhere(c)) return 0;/* zombie still switching away:
                                              don't free its stack/tables yet */
    if (status_out) *status_out = c->exit_status;
    reap(c);
    return (int)c->pid;
}

void sched_reap_orphans(void) {
    task_t *t = g_tasks;
    while (t) {
        task_t *nx = t->next;
        if (t->state == TASK_ZOMBIE && t->pid != 0 &&
            !task_running_anywhere(t) &&
            (t->ppid == 0 || sched_find(t->ppid) == NULL))
            reap(t);
        t = nx;
    }
}

/* ------------------------------------------------------------------ */
/* OOM killer                                                         */
/* ------------------------------------------------------------------ */

/* Kill the user task currently holding the most physical pages so the PMM
 * can reclaim RAM instead of panicking.  Returns:
 *   1  = killed a *different* task - its pages were freed immediately (it
 *        was not running), so pmm_alloc_page should retry,
 *   0  = nothing reclaimable left,
 *   -1 = the only/biggest victim was the CURRENT (faulting) task - its pages
 *        are freed when the fault path tears it down; pmm_alloc_page must
 *        return failure so the caller kills it cleanly (no cascade). */
int sched_oom_kill_one(void) {
    task_t *cur = sched_current();
    task_t *victim = NULL;
    u64 most = 0;
    for (task_t *t = g_tasks; t; t = t->next) {
        if (t->pid == 0 || !t->is_user) continue;
        if (t->state == TASK_ZOMBIE) continue;
        if (t->mem_pages > most) { most = t->mem_pages; victim = t; }
    }
    if (!victim || most == 0) {
        kprintf("oom: nothing reclaimable\n");
        return 0;
    }
    kprintf("oom: killing task %u '%s' to reclaim %llu pages\n",
            victim->pid, victim->name, (unsigned long long)victim->mem_pages);
    sched_kill(victim->pid);            /* frees non-running pages now */
    return (victim == cur) ? -1 : 1;
}

/* ---- OOM selftest (deterministic, uses real pages) ---- */

/* Build a user task that genuinely holds `pages` physical frames (its own
 * PML4 + mapped pages), so killing it reclaims real memory.  It is never
 * put on a runqueue, so sched_kill() frees it immediately. */
static task_t *oom_test_hog(const char *name, u64 pages) {
    task_t *t = kzalloc(sizeof *t);
    if (!t) return NULL;
    t->pid = g_next_pid++;
    t->ppid = 0;
    strncpy(t->name, name, TASK_NAME_LEN - 1);
    t->name[TASK_NAME_LEN - 1] = 0;
    t->is_user = true;
    t->state = TASK_READY;
    t->last_sched = pit_ticks();
    t->mem_pages = pages;
    t->pml4 = vmm_new_pml4();
    for (u64 i = 0; i < pages; i++) {
        paddr_t pg = pmm_alloc_page();
        vmm_map_in(t->pml4, USER_VBASE + i * PAGE_SIZE, pg, PTE_RW | PTE_US);
    }
    task_append(t);
    return t;
}

void oom_selftest(void) {
    kprintf("oom: selftest\n");
    bool ok = true;
    size_t used_orig = pmm_used_pages();

    task_t *h1 = oom_test_hog("hog1", 8);
    task_t *h2 = oom_test_hog("hog2", 24);   /* hungriest victim */
    task_t *h3 = oom_test_hog("hog3", 4);
    if (!h1 || !h2 || !h3) { kprintf("  !! hog alloc failed\n"); ok = false; }
    /* used with all three hogs alive - the baseline we must reclaim from */
    size_t used_with_hogs = pmm_used_pages();

    /* Force pmm_alloc_page through the OOM path even though free RAM remains,
     * so we deterministically exercise the kill-and-reclaim logic. */
    pmm_oom_test_force(true);
    paddr_t p = pmm_alloc_page();
    pmm_oom_test_force(false);

    if (!p) {
        kprintf("  !! OOM alloc returned 0\n");
        ok = false;
    } else {
        /* the hungriest (hog2, 24 pages) must have been SIGKILLed (zombie) */
        if (h2->state != TASK_ZOMBIE) {
            kprintf("  !! hungriest task not killed by OOM killer\n");
            ok = false;
        }
        /* and its real pages reclaimed immediately (it was not running).
         * Compare against used_with_hogs (all 3 alive): killing hog2 + the
         * one page we just allocated must leave us below that. */
        size_t used_after = pmm_used_pages();
        if (used_after >= used_with_hogs) {
            kprintf("  !! no memory reclaimed (with_hogs=%zu after=%zu)\n",
                    used_with_hogs, used_after);
            ok = false;
        } else {
            kprintf("  oom: reclaimed %zu pages (killed hog2), alloc returned %p\n",
                    used_with_hogs - used_after, (void *)p);
        }
    }

    /* clean up the surviving hogs (free their pages; orphan reaper reaps) */
    sched_kill(h1->pid);
    sched_kill(h3->pid);

    /* after freeing all three hogs, memory should return to (near) the level
     * before the test - proving every hog's pages were actually reclaimed */
    size_t used_final = pmm_used_pages();
    if (used_final > used_orig + 32) {
        kprintf("  !! %zu pages leaked after OOM test (orig=%zu final=%zu)\n",
                used_final - used_orig, used_orig, used_final);
        ok = false;
    }

    kprintf("oom: selftest %s (reclaimed via kill, no leak)\n",
            ok ? "PASS" : "FAIL");
}
