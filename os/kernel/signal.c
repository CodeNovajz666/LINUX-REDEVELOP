#include "novix.h"
#include "isr.h"
#include "task.h"
#include "sched.h"
#include "signal.h"
#include "smp.h"
#include "spinlock.h"
#include "lib.h"
#include "paging.h"
#include "mm.h"

/* Frame preparation failed (OOM or an unusable stack).  `task_kill_task`
 * takes sched_lock, which schedule() already holds while delivering to the
 * task it is about to switch to -- only the interrupt-return path (t ==
 * current) may kill here; the schedule path drops the signal instead of
 * iretq'ing into a freshly-made zombie. */
static void signal_fail_delivery(struct task *t, int sig) {
    atomic_and32(&t->sigpending, ~(1u << (sig - 1)));
    if (t == current_task()) task_kill_task(t, sig);
}

/* Make [va, va+len) safe for a kernel-side write before copying the signal
 * frame onto the user stack.  Demand-zero pages, swap markers and COW
 * copies all need resolving here: this runs from the interrupt return path
 * (and from schedule(), holding sched_lock), where taking a #PF would
 * re-enter schedule()/the fault path and can deadlock or triple-fault. */
static int signal_prefault_frame(struct task *t, uint64_t va, uint64_t len) {
    uint64_t first = va & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t last = (va + len - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    for (uint64_t p = first; p <= last; p += PAGE_SIZE) {
        uint64_t pte = paging_pte_read(t->page_dir, p);
        if (pte & PTE_P) {
            if ((pte & PTE_COW) && paging_cow_fault(t->page_dir, p) != 0)
                return -1;
        } else if (mm_fault(t, p, 1) != 0) {
            return -1;
        }
    }
    return 0;
}

/*
 * Signal delivery (v0.5.0) -- the third Unix stage after generation
 * (sys_kill / exceptions set sigpending) and queuing: rewriting the frame
 * the CPU is about to iretq into, so the user task "returns" straight into
 * its handler. This is a teaching-scale clone of Linux's
 * setup_rt_frame/rt_sigreturn pair.
 *
 * User stack layout after delivery (grows down):
 *
 *   new_rsp + 0    sigrestorer        <- handler's `ret` lands here
 *   new_rsp + 8    saved pt_regs      <- SYS_SIGRETURN copies it back out
 *   ...            (original rsp above)
 *
 * The handler is entered with rdi = signo (SysV arg register) and rsp =
 * new_rsp; the SysV ABI's "rsp ≡ 8 (mod 16) at function entry" holds because
 * the interrupted rsp satisfies it and the frame size is a multiple of 16.
 */

/* pick the lowest pending, unblocked signal; 0 = none */
static int signal_pick(const struct task *t) {
    uint32_t pend = t->sigpending & ~t->sigblocked;
    for (int sig = SIG_MIN; sig <= SIG_MAX; sig++)
        if (pend & (1u << (sig - 1))) return sig;
    return 0;
}

/* Called from the interrupt return path (isr.c for the current task,
 * sched.c's schedule() for the task being switched to) with that task's
 * live frame. Must run with the task's CR3 loaded: the saved frame is
 * written onto the *user* stack. A task caught in the kernel (cs&3 != 3,
 * e.g. preempted inside a syscall) is skipped -- its signal is delivered
 * when a later switch finds it with a user frame. */
void signal_deliver_pending(struct task *t, struct pt_regs *r) {
    if (!t || !r || !t->is_user || t->state == TASK_ZOMBIE) return;
    if ((r->cs & 3) != 3) return;              /* kernel frame: not yet */

    int sig = signal_pick(t);
    if (!sig) return;

    uint64_t handler = t->sighandlers[sig];
    uint64_t frame_sz = 8 + sizeof(struct pt_regs);
    uint64_t nrsp = r->rsp - frame_sz;
    uint64_t stack_bot = USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE;

    /* defensive: a pending signal nobody can catch, or no room on the
     * stack for the frame, falls back to the default action (terminate) */
    if (!handler || nrsp < stack_bot) {
        atomic_and32(&t->sigpending, ~(1u << (sig - 1)));
        task_kill_task(t, sig);
        return;
    }

    /* resolve the frame's destination pages before touching them: this
     * may run inside schedule() with sched_lock held, so a #PF here would
     * recurse into the scheduler instead of being resolved */
    if (signal_prefault_frame(t, nrsp, frame_sz) != 0) {
        signal_fail_delivery(t, sig);
        return;
    }

    atomic_and32(&t->sigpending, ~(1u << (sig - 1)));
    *(volatile uint64_t *)(uintptr_t)nrsp = t->sigrestorer;
    memcpy((void *)(uintptr_t)(nrsp + 8), r, sizeof(*r));

    r->rip = handler;
    r->rdi = (uint64_t)sig;
    r->rax = 0;
    r->rsp = nrsp;
    /* exactly one signal per frame rewrite: the next pending one is
     * delivered after sigreturn restores this frame */
}

/* Generation stage (was sys_kill's body; also drives the shell's Ctrl+C).
 * A catchable signal for a user task only latches a pending bit -- the mask
 * is consulted later, at delivery, so a blocked signal can stay pending
 * indefinitely (POSIX semantics, teaching scale). */
int signal_send(struct task *target, int sig) {
    if (sig < SIG_MIN || sig > SIG_MAX) return -1;
    if (!target || target->state == TASK_UNUSED || target->state == TASK_ZOMBIE)
        return -1;

    /* kernel threads have no user frame to deliver into: the only possible
     * action is the default one (terminate), exactly like SIGKILL semantics */
    if (!target->is_user || sig == SIGKILL || !target->sighandlers[sig]) {
        task_kill_task(target, sig);         /* default action: terminate */
        return 0;
    }

    /* v0.7.0: atomic RMW -- the target may be delivering a (different)
     * pending signal on another CPU right now; a plain read-modify-write
     * could drop a bit latched in between */
    atomic_or32(&target->sigpending, 1u << (sig - 1));
    /* the handler runs at the next return to user; if the target is parked
     * in sleep/wait, wake it so delivery happens soon (EINTR, simplified) */
    if (task_wake(target))
        smp_resched_others();                /* it may be parked on any CPU */
    return 0;
}

/* v1.2.0: signal every live user task in a process group -- the console
 * input layer's Ctrl-C path. Targets are collected under sched_lock (the
 * same lock signal_send may take per target), then generated outside it.
 * Group members without a SIGINT handler die by the default action, a
 * handler catches it -- plain Unix semantics. 0 = at least one task was
 * signaled, -1 = the group is empty (or the arguments are bad). */
int signal_send_pgrp(int pgid, int sig) {
    if (sig < SIG_MIN || sig > SIG_MAX || pgid <= 0) return -1;
    struct task *targets[TASK_MAX];
    int n = 0;
    irq_flags_t f;
    spin_lock_irqsave(&sched_lock, &f);
    for (struct task *t = task_first(); t && n < TASK_MAX; t = task_next(t)) {
        if (t->pgid == pgid && t->is_user &&
            t->state != TASK_UNUSED && t->state != TASK_ZOMBIE)
            targets[n++] = t;
    }
    spin_unlock_irqrestore(&sched_lock, f);
    for (int i = 0; i < n; i++)
        signal_send(targets[i], sig);
    return n ? 0 : -1;
}
