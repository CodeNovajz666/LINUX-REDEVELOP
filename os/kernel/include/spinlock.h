#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <stdint.h>
#include "novix.h"

/*
 * v0.7.0 spinlocks ("关中断自旋锁", early-Linux style).
 *
 * spin_lock() itself does NOT touch the interrupt flag: the irqsave variants
 * are what shared data wants, because cli/sti is per-CPU on SMP and an
 * interrupt taken while holding a lock must not re-enter the same lock on
 * this CPU.
 *
 * LOCK ORDER (acquire in this order only, release in reverse):
 *
 *   sched_lock  >  fs_lock / pipe locks  >  pmm_lock / heap_lock  >  console_lock
 *
 * sched_lock guards task state, the per-CPU runqueues and the task list.
 * pmm/heap/console are leaves: they never acquire another lock. A caller
 * may hold sched_lock and then take any other lock; the reverse is a
 * deadlock. Violations are easier to audit than to debug -- keep it linear.
 */

typedef struct {
    volatile int locked;        /* 0 = free, 1 = held */
} spinlock_t;

#define SPINLOCK_INIT  { 0 }

static inline void spin_lock(spinlock_t *l) {
    for (;;) {
        if (!__atomic_test_and_set(&l->locked, __ATOMIC_ACQUIRE))
            return;
        do { cpu_pause(); } while (l->locked);   /* test-and-test-and-set */
    }
}

static inline void spin_unlock(spinlock_t *l) {
    __atomic_clear(&l->locked, __ATOMIC_RELEASE);
}

/* shared-data critical sections: mask local interrupts for the hold */
static inline void spin_lock_irqsave(spinlock_t *l, irq_flags_t *f) {
    *f = irq_save();
    spin_lock(l);
}

static inline void spin_unlock_irqrestore(spinlock_t *l, irq_flags_t f) {
    spin_unlock(l);
    irq_restore(f);
}

/* ---- locked RMW helpers (counters, ready flags) ---- */

static inline int atomic_fetch_add(volatile int *p, int v) {
    __asm__ volatile("lock xaddl %0, %1"
                     : "+r"(v), "+m"(*p) :: "memory", "cc");
    return v;                                   /* old value */
}
static inline void atomic_inc(volatile int *p) {
    __asm__ volatile("lock incl %0" : "+m"(*p) :: "memory", "cc");
}

/* bit-set/clear on 32-bit words (sigpending etc.): each op atomic, so a
 * signal latched from another CPU can not be lost to a concurrent clear */
static inline void atomic_or32(volatile uint32_t *p, uint32_t v) {
    __asm__ volatile("lock orl %1, %0" : "+m"(*p) : "r"(v) : "memory", "cc");
}
static inline void atomic_and32(volatile uint32_t *p, uint32_t v) {
    __asm__ volatile("lock andl %1, %0" : "+m"(*p) : "r"(v) : "memory", "cc");
}

#endif /* SPINLOCK_H */
