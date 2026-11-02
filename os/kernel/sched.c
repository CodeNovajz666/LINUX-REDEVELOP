#include "novix.h"
#include "sched.h"
#include "gdt.h"
#include "paging.h"
#include "syscall.h"
#include "timer.h"
#include "signal.h"
#include "console.h"
#include "percpu.h"
#include "spinlock.h"
#include "lib.h"

/*
 * Preemptive scheduler (v0.7.0: SMP-aware). All scheduling decisions
 * converge in schedule(), which returns the kesp of the task to resume;
 * the interrupt epilogue in isr.S loads it into ESP and a single iret
 * completes the switch.
 *
 * SMP model: ONE global runqueue (the all-task list) guarded by sched_lock.
 * Ownership: a task carries on_cpu == -1 while parked and the owner's id
 * while some CPU executes it. A task is pickable only when RUNNABLE *and*
 * parked, and the owner parks it inside the same locked critical section
 * in which it picks the successor -- so at most one CPU can ever adopt a
 * task. This closes the sleep window race (task_sleep_until() marks a task
 * SLEEPING and drops sched_lock *before* the sleeper's kyield() reaches
 * schedule(): a remote tick/task_wake may flip it back to RUNNABLE in that
 * gap, and without ownership a second CPU could adopt -- and then write --
 * a kstack the first CPU was still executing on; that double-run was the
 * kswapd corruption). need_resched, the RR rotation cursor and the idle
 * fallback are per-CPU; every CPU takes its own LAPIC-timer tick and
 * preempts locally. Load balancing is implicit: an idle CPU simply pulls
 * the next runnable task.
 */

spinlock_t sched_lock = SPINLOCK_INIT;

static sched_policy_t policy = SCHED_PRIORITY;

sched_policy_t sched_policy(void) { return policy; }

const char *sched_policy_name(sched_policy_t p) {
    static const char *names[] = { "PRIORITY", "RR", "FCFS", "EDF", "RMS" };
    return p < SCHED_POLICY_COUNT ? names[p] : "?";
}

void sched_set_policy(sched_policy_t p) {
    if (p >= SCHED_POLICY_COUNT) return;
    irq_flags_t f;
    spin_lock_irqsave(&sched_lock, &f);
    policy = p;
    spin_unlock_irqrestore(&sched_lock, f);
    /* every CPU re-evaluates on its next tick */
    for (int i = 0; i < SMP_MAX; i++)
        if (cpus[i].online) cpus[i].need_resched = true;
}

void sched_kick(void) { this_cpu()->need_resched = true; }
void sched_kick_clear(void) { this_cpu()->need_resched = false; }
bool sched_should_switch(void) { return this_cpu()->need_resched; }

void sched_init(void) {
    policy = SCHED_PRIORITY;
}

/* ---- policy pick: choose the best RUNNABLE task (never an idle) ---- */

static bool task_is_idle(struct task *t) { return t->is_idle; }

static inline uint32_t eff_deadline(struct task *t) { return t->deadline ? t->deadline : 0xFFFFFFFFu; }
static inline uint32_t eff_period(struct task *t)   { return t->period   ? t->period   : 0xFFFFFFFFu; }

/* pickable: ready AND parked (on_cpu == -1) -- never an owned or idle task */
static bool task_pickable(struct task *t) {
    return t->state == TASK_RUNNABLE && t->on_cpu < 0 && !task_is_idle(t);
}

static struct task *pick_rr(struct cpu *c) {
    struct task *first = NULL;
    for (struct task *t = task_first(); t; t = task_next(t)) {
        if (task_pickable(t)) { first = t; break; }
    }
    if (!first) return NULL;
    struct task *start = c->rr_last ? task_next(c->rr_last) : task_first();
    if (!start) start = task_first();

    for (struct task *t = start; t; t = task_next(t))
        if (task_pickable(t) && t->quantum > 0) return t;
    for (struct task *t = task_first(); t != start; t = task_next(t)) {
        if (!t) break;
        if (task_pickable(t) && t->quantum > 0) return t;
    }
    /* everyone expired: refresh and take the first after rr_last */
    for (struct task *t = task_first(); t; t = task_next(t))
        if (task_pickable(t)) t->quantum = RR_QUANTUM;
    return task_pickable(start) ? start : first;
}

static struct task *pick_policy(struct task *current_t) {
    (void)current_t;                     /* policies key off task state only */
    if (policy == SCHED_ROUND_ROBIN) {
        struct task *t = pick_rr(this_cpu());
        if (t) return t;
    }
    struct task *best = NULL;
    for (struct task *t = task_first(); t; t = task_next(t)) {
        if (!task_pickable(t)) continue;
        if (!best) { best = t; continue; }
        switch (policy) {
        case SCHED_PRIORITY:
            if (t->priority > best->priority) best = t;
            break;
        case SCHED_FCFS:
            break;                          /* list order == creation order */
        case SCHED_EDF:
            if (eff_deadline(t) < eff_deadline(best)) best = t;
            break;
        case SCHED_RMS:
            if (eff_period(t) < eff_period(best)) best = t;
            break;
        default:
            break;
        }
    }
    return best;                             /* idle fallback lives in schedule() */
}

uint64_t schedule(struct pt_regs *r) {
    irq_flags_t f;
    spin_lock_irqsave(&sched_lock, &f);
    struct cpu *c = this_cpu();
    struct task *prev = c->current;
    struct task *next;

    if (prev && prev->on_cpu == c->id) {
        /* park prev: drop ownership inside the same critical section as
         * the pick, so the moment it becomes stealable nobody can race
         * us onto its kstack. State stays SLEEPING/WAITING/ZOMBIE when
         * prev blocked or died -- only RUNNING demotes to RUNNABLE. */
        prev->kesp = (uint64_t)(uintptr_t)r;
        if (prev->state == TASK_RUNNING) prev->state = TASK_RUNNABLE;
        prev->on_cpu = -1;
    } else {
        /* prev was adopted by another CPU while we sat parked between
         * task_sleep_until()/waitpid() and this kyield: it owns the kesp
         * and the state now -- write neither (that double-own was the
         * kswapd double-run). Just switch to someone else. */
        prev = NULL;
    }

    next = pick_policy(prev);
    if (!next) {
        if (prev && prev->state == TASK_RUNNABLE) {
            next = prev;                        /* nothing better: keep running */
        } else {
            /* this CPU's own idle takes over */
            next = c->idle;
            if (!next) panic("schedule: no idle task for cpu%d", c->id);
            next->state = TASK_RUNNABLE;
        }
    }

    next->state = TASK_RUNNING;
    next->on_cpu = c->id;                       /* adopt: ours until we park it */
    next->ticks_run++;
    next->cpu = c->id;
    next->priority = next->base_priority;   /* undo aging boosts */
    next->wait_ticks = 0;
    next->quantum = (policy == SCHED_ROUND_ROBIN) ? RR_QUANTUM : PRIO_QUANTUM;
    if (policy == SCHED_ROUND_ROBIN) c->rr_last = next;

#ifdef SCHED_TRACE
    {
        struct pt_regs *fr = (struct pt_regs *)(uintptr_t)next->kesp;
        kprintf("[sched] cpu%d %s(pid%d,k=%08llx) -> %s(pid%d,k=%08llx,stk=%08llx..%08llx) "
                "fr: rip=%016llx cs=%04x\n",
                c->id,
                prev ? prev->name : "?", prev ? prev->pid : -1,
                prev ? (unsigned long long)prev->kesp : 0,
                next->name, next->pid, (unsigned long long)next->kesp,
                (unsigned long long)next->kstack_base,
                (unsigned long long)next->kstack_top,
                fr ? (unsigned long long)fr->rip : 0, fr ? (unsigned)(fr->cs & 0xFFFF) : 0);
    }
#endif
    if (next->page_dir != paging_cur_cr3())
        paging_load_cr3(next->page_dir);
    tss_set_kstack(next->kstack_top);

    /* second signal delivery point (v0.5.0): next's frame is about to be
     * iretq'd into user space and its CR3 is loaded, so its user stack is
     * writable -- the exact preconditions signal_deliver_pending needs.
     * This is how a signal sent to a *running* task (preempted by this very
     * tick) or parked in sleep/wait catches up with it. */
    if (next->is_user)
        signal_deliver_pending(next, (struct pt_regs *)(uintptr_t)next->kesp);

    c->current = next;
    c->need_resched = false;
    spin_unlock_irqrestore(&sched_lock, f);
    return next->kesp;
}

/* ---- tick: wake sleepers, aging, decide preemption (per-CPU) ---- */

/* one pass over the task list per tick: flip expired sleepers to RUNNABLE
 * and credit the runnable-but-not-run with aging (the original two loops
 * were behavior-identical -- a task woken this tick was aged by the second
 * pass anyway -- but they walked the whole list twice under sched_lock on
 * every tick of every CPU) */
static void wake_and_age(uint64_t tick) {
    for (struct task *t = task_first(); t; t = task_next(t)) {
        if (t->state == TASK_SLEEPING && tick >= t->wake_tick)
            t->state = TASK_RUNNABLE;
        if (t->state != TASK_RUNNABLE || task_is_idle(t)) continue;
        t->wait_ticks++;
        if (t->wait_ticks >= AGING_INTERVAL && t->priority < PRIO_MAX) {
            t->priority++;                  /* anti-starvation boost */
            t->wait_ticks = 0;
        }
    }
}

void sched_on_tick(uint64_t tick) {
    irq_flags_t f;
    spin_lock_irqsave(&sched_lock, &f);
    wake_and_age(tick);

    struct task *cur = this_cpu()->current;
    if (!cur) { this_cpu()->need_resched = true; spin_unlock_irqrestore(&sched_lock, f); return; }

    /* any parked runnable non-idle task means the idle must step aside
     * (owned ones are mid-switch on their owner and not ours to take) */
    if (task_is_idle(cur)) {
        for (struct task *t = task_first(); t; t = task_next(t))
            if (task_pickable(t)) {
                this_cpu()->need_resched = true;
                break;
            }
        spin_unlock_irqrestore(&sched_lock, f);
        return;
    }

    switch (policy) {
    case SCHED_ROUND_ROBIN:
        if (--cur->quantum <= 0) this_cpu()->need_resched = true;
        break;
    case SCHED_PRIORITY:
        if (--cur->quantum <= 0) { this_cpu()->need_resched = true; break; }
        for (struct task *t = task_first(); t; t = task_next(t))
            if (task_pickable(t) && t->priority > cur->priority) {
                this_cpu()->need_resched = true;
                break;
            }
        break;
    case SCHED_EDF:
    case SCHED_RMS: {
        struct task *best = pick_policy(cur);
        if (best && best != cur) this_cpu()->need_resched = true;
        break;
    }
    case SCHED_FCFS:
    default:
        break;                              /* run to block/exit */
    }
    spin_unlock_irqrestore(&sched_lock, f);
}

void sched_notify_new_task(struct task *t) {
    (void)t;
    if (policy == SCHED_EDF || policy == SCHED_RMS || policy == SCHED_PRIORITY) {
        /* every online CPU re-checks its runqueue at the next tick; no IPI
         * needed for latency teaching-scale tolerates (<= 10 ms) */
        for (int i = 0; i < SMP_MAX; i++)
            if (cpus[i].online) cpus[i].need_resched = true;
    }
}

/* cooperative yield from any privilege level */
void kyield(void) {
    uint64_t ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"((uint64_t)SYS_YIELD) : "memory");
}
