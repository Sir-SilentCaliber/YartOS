#pragma once
#include <yart/types.h>
#include <yart/io.h>

/* Simple ticket-style spinlock for SMP.  Works on the BSP too. */
typedef struct { volatile u32 locked; } spinlock_t;

static ALWAYS_INLINE void spin_init(spinlock_t *l) { l->locked = 0; }

static ALWAYS_INLINE void spin_lock(spinlock_t *l) {
    while (__atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&l->locked, __ATOMIC_RELAXED))
            __asm__ volatile("pause");
    }
}
static ALWAYS_INLINE void spin_unlock(spinlock_t *l) {
    __atomic_store_n(&l->locked, 0, __ATOMIC_RELEASE);
}
static ALWAYS_INLINE bool spin_trylock(spinlock_t *l) {
    return !__atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE);
}
