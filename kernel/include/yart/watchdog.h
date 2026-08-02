#pragma once
#include <yart/types.h>

/* Yart watchdog + service supervisor (kernel/sched/watchdog.c).
 *
 * A kernel service registers a name and a restart callback.  Every time the
 * service makes progress (e.g. once per desktop-loop iteration) it calls
 * watchdog_kick().  The watchdog is driven from the APIC timer (via the
 * scheduler) and, if a service has NOT kicked for WATCHDOG_TIMEOUT_TICKS,
 * declares it dead and invokes its restart callback so the component comes
 * back automatically instead of hanging the machine forever.
 *
 * It also scans for hung USER tasks (a READY task that has not been
 * scheduled for a long time - a scheduler/application bug) and SIGKILLs
 * them, so one frozen app cannot wedge the system.
 */

#define WATCHDOG_MAX_SERVICES 8
#define WATCHDOG_NAME_LEN    24

/* Register a supervised service.  Returns an opaque index for watchdog_kick,
 * or -1 if the table is full.  `restart` may be NULL (detect + log only). */
int  watchdog_register_service(const char *name, void (*restart)(void));

/* A service calls this to report "I am alive".  Cheap (a single u64 write). */
void watchdog_kick(int idx);

/* Remove a service from supervision (e.g. a one-shot selftest service). */
void watchdog_unregister(int idx);

/* Periodic check; called by the scheduler from the APIC timer (BSP only). */
void watchdog_tick(void);

/* Boot-time verification: register a synthetic service, simulate a stall and
 * prove the watchdog restarts it. */
void watchdog_selftest(void);

u64  watchdog_restart_count(void);
u64  watchdog_kill_count(void);
