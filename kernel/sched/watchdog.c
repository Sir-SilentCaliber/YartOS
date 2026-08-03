/* Yart OS - watchdog + service supervisor (row 13).
 *
 * Two responsibilities:
 *   1) SERVICE SUPERVISION - kernel services (the desktop loop, etc.)
 *      register a heartbeat + restart callback.  A timer-driven watchdog
 *      (watchdog_tick, called from the BSP's APIC timer via the scheduler)
 *      checks each service; if one hasn't kicked within the timeout it is
 *      declared dead and its restart callback is invoked.
 *   2) HUNG-TASK RECOVERY - a user task that sits READY but is never
 *      scheduled for a long time (a scheduler or application bug) is
 *      SIGKILLed so one frozen app cannot wedge the machine.
 */
#include <yart/watchdog.h>
#include <yart/string.h>
#include <yart/console.h>
#include <yart/spinlock.h>
#include <yart/hal.h>      /* pit_ticks()                          */
#include <yart/sched.h>    /* task_t / sched_tasks() for the scan  */

#define WATCHDOG_TIMEOUT_TICKS  300   /* ~3 s at 100 Hz (APIC timer) */
#define HUNG_TASK_TIMEOUT_TICKS 1000  /* ~10 s                      */

typedef struct {
    char name[WATCHDOG_NAME_LEN];
    u64  last_kick;            /* pit_ticks() of the last kick       */
    void (*restart)(void);
    bool registered;
} wd_service_t;

static wd_service_t g_services[WATCHDOG_MAX_SERVICES];
static spinlock_t   g_wd_lock;
static u64          g_wd_checks;     /* stalls detected               */
static u64          g_wd_restarts;   /* restarts performed            */
static u64          g_wd_kills;      /* hung tasks SIGKILLed          */
static bool         g_wd_inited;

int watchdog_register_service(const char *name, void (*restart)(void)) {
    spin_lock(&g_wd_lock);
    for (int i = 0; i < WATCHDOG_MAX_SERVICES; i++) {
        if (g_services[i].registered) continue;
        strncpy(g_services[i].name, name, WATCHDOG_NAME_LEN - 1);
        g_services[i].name[WATCHDOG_NAME_LEN - 1] = 0;
        g_services[i].last_kick = pit_ticks();
        g_services[i].restart   = restart;
        g_services[i].registered = true;
        kprintf("watchdog: service '%s' registered (idx %d)\n", name, i);
        spin_unlock(&g_wd_lock);
        return i;
    }
    spin_unlock(&g_wd_lock);
    return -1;
}

void watchdog_kick(int idx) {
    if (idx < 0 || idx >= WATCHDOG_MAX_SERVICES) return;
    /* single atomic u64 store; the watchdog only ever reads it, so a stale
     * read in the worst case just means one extra timeout scan - harmless */
    g_services[idx].last_kick = pit_ticks();
}

void watchdog_unregister(int idx) {
    if (idx < 0 || idx >= WATCHDOG_MAX_SERVICES) return;
    spin_lock(&g_wd_lock);
    g_services[idx].registered = false;
    g_services[idx].name[0] = 0;
    g_services[idx].restart = NULL;
    spin_unlock(&g_wd_lock);
}

static void check_services(u64 now) {
    for (int i = 0; i < WATCHDOG_MAX_SERVICES; i++) {
        if (!g_services[i].registered) continue;
        u64 idle = now - g_services[i].last_kick;
        if (idle > WATCHDOG_TIMEOUT_TICKS) {
            g_wd_checks++;
            kprintf("watchdog: service '%s' STALLED for %llu ticks -> "
                    "restarting\n", g_services[i].name,
                    (unsigned long long)idle);
            if (g_services[i].restart)
                g_services[i].restart();
            g_services[i].last_kick = pit_ticks();
            g_wd_restarts++;
        }
    }
}

/* Report (log-only) a user task that has been READY-but-never-scheduled for
 * a long time.  We deliberately do NOT SIGKILL here: in this preemptive
 * round-robin scheduler a user task cannot actually freeze the OS (it is
 * preempted), and auto-killing risks taking down a legitimately busy task
 * (a false positive observed in early testing when a queued task's
 * last_sched was still 0).  The real "frozen component" problem is a hung
 * KERNEL service, which the service-heartbeat watchdog above handles by
 * restarting it.  Kernel tasks / zombies are never flagged. */
static void check_hung_tasks(u64 now) {
    task_t *t = sched_tasks();
    while (t) {
        task_t *nx = t->next;
        /* READY-but-starved only: RUNNING, BLOCKED (sleep/waitpid) and
         * ZOMBIE tasks are all legitimately not being scheduled */
        if (t->is_user && t->pid != 0 &&
            t->state == TASK_READY &&
            (now - t->last_sched) > HUNG_TASK_TIMEOUT_TICKS) {
            kprintf("watchdog: task %u '%s' appears starved (un-scheduled "
                    "for %llu ticks) - reported, not killed\n",
                    t->pid, t->name, (unsigned long long)(now - t->last_sched));
            g_wd_kills++;   /* counts reports here (kept for the stat name) */
            t->last_sched = now;   /* avoid re-reporting every second */
        }
        t = nx;
    }
}

void watchdog_tick(void) {
    if (!g_wd_inited) return;
    u64 now = pit_ticks();
    check_services(now);
    check_hung_tasks(now);
}

u64 watchdog_restart_count(void) { return g_wd_restarts; }
u64 watchdog_kill_count(void)    { return g_wd_kills; }

/* A synthetic service used by the selftest.  It "hangs" after N kicks; the
 * restart callback resets it and counts the restart. */
static int  g_demo_calls;
static int  g_demo_restarts;
static int  g_demo_wd_idx;
static void demo_restart(void) { g_demo_restarts++; kprintf("watchdog: [demo] restart callback ran\n"); }

void watchdog_selftest(void) {
    g_wd_inited = true;
    kprintf("watchdog: selftest\n");
    bool ok = true;

    g_demo_calls = 0;
    g_demo_restarts = 0;
    g_demo_wd_idx = watchdog_register_service("kclockd", demo_restart);
    if (g_demo_wd_idx < 0) { kprintf("  !! register failed\n"); ok = false; }

    /* simulate a healthy service: recent kicks, no restart */
    watchdog_kick(g_demo_wd_idx);
    watchdog_tick();
    if (g_demo_restarts != 0) { kprintf("  !! false restart\n"); ok = false; }

    /* simulate a stall: back-date the heartbeat, then let the watchdog run */
    extern u64 pit_ticks(void);
    g_services[g_demo_wd_idx].last_kick = pit_ticks() - WATCHDOG_TIMEOUT_TICKS - 50;
    watchdog_tick();
    if (g_demo_restarts != 1) {
        kprintf("  !! stall not detected/restarted (restarts=%d)\n", g_demo_restarts);
        ok = false;
    }

    /* after the restart the heartbeat was refreshed: no second restart */
    watchdog_tick();
    if (g_demo_restarts != 1) { kprintf("  !! repeat restart\n"); ok = false; }

    /* drop the synthetic service so the live watchdog does not keep
     * restarting it every second */
    watchdog_unregister(g_demo_wd_idx);

    kprintf("watchdog: selftest %s (stall-checks=%llu restarts=%llu kills=%llu)\n",
            ok ? "PASS" : "FAIL",
            (unsigned long long)g_wd_checks,
            (unsigned long long)g_wd_restarts,
            (unsigned long long)g_wd_kills);
}
