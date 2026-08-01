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
extern void lapic_send_ipi(u32 dest_apic, u8 vector);
#define AP_WAKE_VEC 62

static task_t *g_tasks;       /* global list (all tasks, any state)   */
static task_t *g_idle_task;   /* BSP's desktop task (pid 0)           */
static u32     g_next_pid;
static bool    g_idle_handoff;   /* idle task is sleeping and may be preempted */
static spinlock_t g_tasks_lock;  /* protects g_tasks append/remove/find/reap */

/* ------------------------------------------------------------------ */
/* per-CPU current task                                               */
/* ------------------------------------------------------------------ */

task_t *sched_current(void) {
    cpu_local_t *c = get_cpu_local();
    return (c && c->ap_current) ? c->ap_current : NULL;
}
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

task_t *sched_create_user(const char *name, u64 entry, u64 user_rsp) {
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

    /* Per-process address space: clone the (kernel) page tables, CoW-share
     * the prepared user pages, and hand the task its own PML4 + regions. */
    vmm_give_current_regions_to(t);
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
 * being left queued (no duplicates). */
static u64 sched_switch_after(task_t *cur, u64 current_rsp) {
    cpu_local_t *c = get_cpu_local();
    if (cur && cur->state == TASK_READY) {          /* yielded */
        task_t *next = rq_push_pop(c, cur);
        if (next && next != cur) return switch_to(next, current_rsp);
        if (next == cur) rq_remove(c, cur);  /* it is still queued: take it
                                                back off before parking */
        if (!c->ap_next) c->ap_next = cur;
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

void sched_yield(void) {
    task_t *cur = sched_current();
    if (cur && cur->state == TASK_RUNNING)
        cur->state = TASK_READY;
}

void sched_exit(int status) {
    task_t *cur = sched_current();
    if (!cur || !cur->is_user) return;
    cur->exit_status = status;
    cur->state = TASK_ZOMBIE;
    vmm_user_teardown_all();           /* free this task's user memory */
    task_t *p = sched_find(cur->ppid);
    if (p && p->waiting) {             /* blocked parent: wake + requeue */
        p->waiting = false;
        p->state = TASK_READY;
        rq_push(get_cpu_local(), p);
    }
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
    if (sig == SIG_KILL) return sched_kill(pid);
    if (sig == 0) return 0;                 /* existence probe */
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

int sched_waitpid(u32 pid, int *status_out) {
    task_t *cur = sched_current();
    if (!cur) return -1;
    task_t *c = NULL;
    for (task_t *t = g_tasks; t; t = t->next)
        if (t->ppid == cur->pid && (pid == 0 || t->pid == pid)) { c = t; break; }
    if (!c) return -1;                    /* ECHILD */
    if (c->state != TASK_ZOMBIE) return 0; /* still running */
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
