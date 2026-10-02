#ifndef SCHED_H
#define SCHED_H

#include <stdint.h>
#include <stdbool.h>
#include "isr.h"
#include "task.h"
#include "spinlock.h"

/* policies: inherited one-to-one from the original module (src/scheduler.h) */
typedef enum {
    SCHED_PRIORITY = 0,
    SCHED_ROUND_ROBIN = 1,
    SCHED_FCFS = 2,
    SCHED_EDF = 3,
    SCHED_RMS = 4,
    SCHED_POLICY_COUNT
} sched_policy_t;

/* v0.7.0: guards task state, the all-task list and every scheduling
 * decision (the top of the lock order -- see spinlock.h). */
extern spinlock_t sched_lock;

void sched_init(void);
uint64_t schedule(struct pt_regs *r);      /* performs switch, returns new kesp */
void sched_on_tick(uint64_t tick);          /* wakes sleepers, aging, preemption */
void sched_notify_new_task(struct task *t); /* policy hook (EDF/RMS early preempt) */
void sched_kick(void);                      /* force reschedule at earliest point */
void sched_kick_clear(void);                /* drop a pending resched request */
bool sched_should_switch(void);
void kyield(void);                          /* cooperative yield from any ring */
void sched_set_policy(sched_policy_t p);
sched_policy_t sched_policy(void);
const char *sched_policy_name(sched_policy_t p);

#endif /* SCHED_H */
