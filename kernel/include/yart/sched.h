#pragma once
#include <yart/types.h>
#include <yart/fs.h>
#include <yart/mm.h>
#include <yart/hal.h>   /* cpu_regs_t */
typedef struct task task_t;
struct cpu_local_s;
typedef struct cpu_local_s cpu_local_t;

#define TASK_NAME_LEN 16
#define MAX_FD 32
#define KSTACK_SIZE KB(16)

typedef struct {
    vnode_t *vn;
    u64      pos;
    u32      flags;
    bool     in_use;
} fd_entry_t;

typedef enum { TASK_READY, TASK_RUNNING, TASK_BLOCKED, TASK_ZOMBIE } task_state_t;

/* One process.  The scheduler saves/restores a full cpu_regs_t frame on each
 * task's private kernel stack; the frame is the one the ISR pushed, so a
 * switch is just "remember which frame, restore another, iretq". */
typedef struct task {
    u32            pid, ppid;
    char           name[TASK_NAME_LEN];
    task_state_t   state;
    int            exit_status;
    bool           is_user;        /* false = kernel/desktop task        */
    u64           *pml4;           /* NULL = kernel page tables          */
    u32            uid;            /* real user id                       */
    u32            euid;           /* effective uid (doas can make it 0)  */
    u32            gid;            /* primary group id                         */
    u32            supp_gids[8];   /* supplementary groups                     */
    u8             supp_gid_count;
    u16            umask;          /* file-creation mask (022 default)         */
    bool           elev_allowed;   /* may elevate to root (admin user)    */
    char           account[32];    /* account name for password checks    */
    u64            kstack_top;     /* RSP0 for ring3->ring0 entries      */
    u64            saved_rsp;      /* cpu_regs_t frame on the kstack     */
    u8             fpu_area[512] __attribute__((aligned(16)));  /* FXSAVE */
    u64            mmap_next;   /* next free VA for mmap() (per process)   */
    u64            last_sched;  /* pit_ticks() when last switched to (watchdog
                                   uses this to spot a READY-but-starved task) */
    u64            brk;         /* program break (top of the data segment)     */
    u64            brk_base;    /* bottom of the heap region                   */
    u64            sig_handlers[8]; /* per-signal handler VAs (0 = default)    */
    u64            sig_pending;     /* pending signal bitmask (1<<sig)         */
    u64            sig_blocked;     /* blocked signal bitmask                   */
    u64            mem_pages;      /* physical pages this task currently uses */
    u64            mem_limit_pages;/* cap (default 256 MiB)                  */
    fd_entry_t     fds[MAX_FD];
    vnode_t       *cwd;
    bool           waiting;        /* blocked on waitpid (reserved)      */
    u32            wait_pid;
    user_region_t  regions[MAX_USER_REGIONS];   /* per-process demand map */
    int            region_count;
    /* SMP scheduling linkage.  `next` chains the GLOBAL all-tasks list
     * (sched_find/waitpid/reap); `rq_next` chains THIS CPU's ready queue.
     * They MUST be separate: queue ops rewire rq_next and would corrupt the
     * global list if they shared one pointer. */
    struct cpu_local_s *rq_cpu;  /* which CPU's ready queue this task is on
                                    (NULL = not queued)                   */
    struct task   *rq_next;
    struct task   *next;
} task_t;

void    sched_init(void);
task_t *sched_tasks(void);          /* head of the global all-tasks list   */
task_t *sched_current(void);
/* OOM killer (called from pmm_alloc_page when RAM is exhausted): kill the
 * user task holding the most physical pages so the OS survives. */
int     sched_oom_kill_one(void);
void    oom_selftest(void);
bool    sched_current_is_user(void);
u32     sched_current_uid(void);    /* 0 when no task (boot/root) */
u32     sched_current_euid(void);
u32     sched_current_egid(void);
int     sched_current_groups(u32 *out, int max);  /* [primary, supp...]     */
u16     sched_current_umask(void);
u16     sched_set_umask(u16 m);                    /* returns old            */
/* per-task memory accounting (enforced by the VMM) */
u64     sched_mem_used(void);
u64     sched_mem_limit(void);
void    sched_charge_pages(i64 delta);   /* +reserve -release  */

task_t *sched_create_user(const char *name, u64 entry, u64 user_rsp);
task_t *sched_fork(task_t *parent, cpu_regs_t *frame);   /* NULL on error */

u64     sched_tick(u64 current_rsp);      /* timer IRQ: maybe preempt     */
u64     sched_after_isr(u64 current_rsp); /* syscall/fault: maybe switch  */
void    sched_yield(void);                /* cooperative hand-off         */
void    sched_exit(int status);           /* zombie + wake parent; no ret */
void    sched_idle_sleep(void);           /* idle hand-off then hlt       */
/* SMP: an AP runs this when it has a ready task on ITS runqueue.
 * Returns true if it found + resumed work.  The AP calls it in its loop. */
bool    sched_ap_try_work(cpu_local_t *c);
void    sched_queue_ap_work(cpu_local_t *c, void (*fn)(void *), void *arg);
int     sched_migrate_to_ap(cpu_local_t *c, task_t *t);
void    sched_ap_steal(cpu_local_t *c);   /* idle AP: steal work (LB) */
/* smp.c: least-loaded online CPU (AP) for a new task; NULL if no APs. */
cpu_local_t *smp_least_loaded(void);
cpu_local_t *smp_get_ap_area(u32 idx);   /* per-AP area by cpu id (1..7) */
int     sched_waitpid(u32 pid, int *status_out);
void    sched_reap_orphans(void);
/* SIGKILL: mark another task dead (exit_status -9); it never runs again
 * and is reaped by its parent or the orphan reaper.  Returns 0 on success,
 * -1 if the pid is invalid / not killable. */
int     sched_kill(u32 pid);
int     sched_signal(u32 pid, u32 sig);   /* deliver a signal (SIGKILL=9) */
u32     sched_next_pid(void);
task_t *sched_find(u32 pid);

/* Called by the VMM: claim the boot-time reserved user regions for a task
 * (they were reserved before the task existed). */
void    vmm_take_boot_regions(task_t *t);
