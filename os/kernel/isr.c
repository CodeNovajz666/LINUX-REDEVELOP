#include "novix.h"
#include "isr.h"
#include "pic.h"
#include "apic.h"
#include "timer.h"
#include "keyboard.h"
#include "console.h"
#include "klog.h"
#include "syscall.h"
#include "task.h"
#include "sched.h"
#include "paging.h"
#include "mm.h"
#include "signal.h"
#include "percpu.h"
#include "smp.h"
#include "spinlock.h"

/*
 * Top-level interrupt dispatch, called from isr.S.
 *
 * One uniform frame for every entry (exceptions, IRQs, int 0x80 syscalls).
 * isr_handler() returns the stack pointer of the frame to resume -- the
 * scheduler tail below may hand back a *different task's* frame, so a
 * single iretq in isr.S does both "return from interrupt" and "context
 * switch" (design carried over unchanged from the i386 kernel).
 *
 * Ring3 faults kill the offending process (Unix 128+signo convention);
 * ring0 faults are kernel bugs and panic.
 */

static const char *exc_name(uint64_t n) {
    switch (n) {
    case 0: return "divide-by-zero";
    case 1: return "debug";
    case 2: return "NMI";
    case 3: return "breakpoint";
    case 4: return "overflow";
    case 5: return "bound range";
    case 6: return "invalid opcode";
    case 7: return "device n/a";
    case 8: return "double fault";
    case 10: return "invalid TSS";
    case 11: return "segment not present";
    case 12: return "stack fault";
    case 13: return "general protection fault";
    case 14: return "page fault";
    case 16: return "x87 fp exception";
    case 17: return "alignment check";
    case 18: return "machine check";
    case 19: return "simd fp exception";
    default: return "exception";
    }
}

static int exc_signo(uint64_t n) {
    switch (n) {
    case 0: return SIGFPE;       /* #DE */
    case 6: return SIGILL;       /* #UD */
    default: return SIGSEGV;     /* #GP, #PF, ... */
    }
}

void dump_regs(const char *why, struct pt_regs *r) {
    console_enter_panic();          /* v0.7.0: dumps must print past a dead lock holder */
    uint64_t cr3, cr2;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    kprintf("\n*** %s: %s (int %u, err %08llx) ***\n",
            why, exc_name(r->int_no), (unsigned)r->int_no,
            (unsigned long long)r->err_code);
    kprintf("  rip=%016llx cs=%04x rflags=%08llx rsp=%016llx ss=%04x\n",
            (unsigned long long)r->rip, (unsigned)(r->cs & 0xFFFF),
            (unsigned long long)r->rflags, (unsigned long long)r->rsp,
            (unsigned)(r->ss & 0xFFFF));
    /* v0.7.0 triage line: which CPU (LAPIC id), whose page tables (cr3),
     * what it faulted on (cr2), and whether the per-CPU %gs anchor is sane.
     * rdmsr/lapic_read cannot fault, so this line survives a broken %gs. */
    kprintf("  lapic=%u cr3=%016llx cr2=%016llx gsbase=%016llx\n",
            apic_lapic_id(), (unsigned long long)cr3,
            (unsigned long long)cr2,
            (unsigned long long)rdmsr(0xC0000101u));
    kprintf("  rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx\n",
            (unsigned long long)r->rax, (unsigned long long)r->rbx,
            (unsigned long long)r->rcx, (unsigned long long)r->rdx);
    kprintf("  rsi=%016llx rdi=%016llx rbp=%016llx\n",
            (unsigned long long)r->rsi, (unsigned long long)r->rdi,
            (unsigned long long)r->rbp);
    kprintf("  r8=%016llx r9=%016llx r10=%016llx r11=%016llx\n",
            (unsigned long long)r->r8, (unsigned long long)r->r9,
            (unsigned long long)r->r10, (unsigned long long)r->r11);
    kprintf("  r12=%016llx r13=%016llx r14=%016llx r15=%016llx\n",
            (unsigned long long)r->r12, (unsigned long long)r->r13,
            (unsigned long long)r->r14, (unsigned long long)r->r15);
    /* Panic path must not trust %gs: if the fault being dumped left this_cpu()
     * broken, dereferencing it just floods the console with recursive #GPs
     * (the very bug that made the first SMP AP crash unreadable). */
    struct cpu *cpu = this_cpu();
    if (cpu < &cpus[0] || cpu >= &cpus[SMP_MAX]) cpu = NULL;
    struct task *t = cpu ? cpu->current : NULL;
    if (t)
        kprintf("  task: %s (pid %d, %s, from ring%llu)\n", t->name, t->pid,
                t->is_user ? "user" : "kernel",
                (unsigned long long)(r->cs & 3));
    else
        kprintf("  task: <this_cpu broken, gsbase=%016llx>\n",
                (unsigned long long)rdmsr(0xC0000101u));
}

uint64_t isr_handler(struct pt_regs *r) {
    uint64_t n = r->int_no;

    if (n == 0x80) {
        syscall_dispatch(r);
    } else if (n < 32) {
        struct task *cur = current_task();
        /* v0.6.0: syscalls dereference user pointers directly on the
         * caller's page tables, so a demand-paged or COW user page can
         * fault right here in ring 0 (a pipe write memcpying from
         * not-yet-touched .rodata, a read into a post-fork COW page).
         * Resolve it exactly like a user-mode fault before treating the
         * exception as a user kill or a kernel bug; mm_fault fails for
         * any address no VMA covers, so genuine kernel faults still
         * reach the panic path below. */
        if (n == 14 && cur && cur->is_user) {
            uint64_t cr2;
            __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
            if ((r->err_code & 0x3) == 0x3 && cur->mm) {    /* write+present: COW */
                /* v1.2.0: the COW resolution rewrites page tables that a
                 * sibling thread may be faulting into on another CPU -- hold
                 * the shared mm's lock around it (order: mm.lock > cow_lock) */
                irq_flags_t mf;
                spin_lock_irqsave(&cur->mm->lock, &mf);
                int rc = paging_cow_fault(cur->page_dir, cr2);
                bool stale_retry = false;
                if (rc != 0) {
                    /* a sibling resolved this COW page a moment ago: our TLB
                     * still holds the old read-only translation while the
                     * (shared) PTE is already present+rw -- invalidate and
                     * resume instead of killing an innocent thread */
                    uint64_t pte = paging_pte_read(cur->page_dir, cr2);
                    if ((pte & PTE_P) && (pte & PTE_RW)) {
                        rc = 0;
                        stale_retry = true;
                    }
                }
                spin_unlock_irqrestore(&cur->mm->lock, mf);
                if (rc == 0) {
                    __asm__ volatile("invlpg (%0)" :: "r"(cr2) : "memory");
                    if (!stale_retry) g_mmstat.fault_cow++;
                    goto done;                              /* resolved: resume */
                }
            }
            if ((r->err_code & 0x1) == 0 &&                 /* non-present: demand */
                mm_fault(cur, cr2, (int)(r->err_code & 0x2)) == 0)
                goto done;                                  /* resolved: resume */
        }
        if ((r->cs & 3) == 3) {
            if (cur && cur->is_user) {
                int signo = exc_signo(n);
                /* a caught signal defers to the handler instead of dying:
                 * queue it and let the return path below rewrite the frame
                 * (a handler that returns lands on the faulting instruction
                 * again -- Unix behavior, the program's own choice) */
                if (cur->sighandlers[signo] && signo != SIGKILL) {
                    atomic_or32(&cur->sigpending, 1u << (signo - 1));
                    goto done;
                }
                /* message format kept from the i386 kernel: the serial
                 * test harness greps for "killed by sigN" / SIGSEGV */
                if (n == 14) {
                    uint64_t cr2;
                    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
                    kprintf("pid %d (%s): %s at rip=%08llx fault addr=%08llx -- killed by SIGSEGV\n",
                            cur->pid, cur->name, exc_name(n),
                            (unsigned long long)r->rip, (unsigned long long)cr2);
                } else {
                    kprintf("pid %d (%s): %s at rip=%08llx -- killed by sig%d\n",
                            cur->pid, cur->name, exc_name(n),
                            (unsigned long long)r->rip, signo);
                }
                task_kill(signo);        /* never returns */
            }
        }
        dump_regs("kernel panic", r);
        panic("unhandled exception %u", (unsigned)n);
    } else if (n == VEC_LAPIC_TIMER) {
        /* the per-CPU preemption clock (v0.7.0): every CPU ticks locally,
         * but only the BSP advances the global tick count so uptime and
         * sleep deadlines stay wall-clock-true */
        apic_eoi();
        timer_tick();
    } else if (n == IPI_RESCHED) {
        apic_eoi();
        this_cpu()->need_resched = true;   /* re-check the runqueue at exit */
    } else if (n == IPI_TLB_SHOOTDOWN) {
        apic_eoi();
        smp_tlb_shootdown_handler();
    } else if (n == 0xFE) {
        apic_eoi();                        /* LAPIC error: ack and move on */
    } else if (n == 0xFF) {
        /* LAPIC spurious: NO EOI (spec) */
    } else if (n == IRQ_KEYBOARD + 32) {
        apic_eoi();                        /* IOAPIC-delivered (v0.7.0) */
        keyboard_irq();
    } else if (n == IRQ_SERIAL + 32) {
        apic_eoi();
        keyboard_serial_irq();
    } else if (n >= 32 && n <= 47) {
        apic_eoi();                        /* other ISA lines: just ack */
    } else {
        dump_regs("kernel panic", r);
        panic("unhandled exception %u", (unsigned)n);
    }

done:
    /* signal delivery point (v0.5.0): the frame below belongs to the task
     * that entered this interrupt and is about to be iretq'd into user
     * space -- the one moment rewriting it is safe. A task that died inside
     * this interrupt (task_exit switches stacks in a nested int 0x80) never
     * reaches this point, so current is still the frame's owner here. */
    {
        struct task *cur = current_task();
        if (cur && cur->is_user && (r->cs & 3) == 3)
            signal_deliver_pending(cur, r);
    }
    /* scheduler tail: switch tasks if the current one was preempted,
     * blocked or killed inside this interrupt. Interrupts go on before the
     * first task exists (kmain enables sti ahead of sched_init), so a boot-
     * time tick has nothing to switch to -- drop the request instead. */
    if (sched_should_switch()) {
        if (current_task())
            return schedule(r);
        sched_kick_clear();
    }
    return (uint64_t)(uintptr_t)r;
}
