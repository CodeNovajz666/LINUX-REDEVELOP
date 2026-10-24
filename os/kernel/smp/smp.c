#include "novix.h"
#include "smp.h"
#include "apic.h"
#include "percpu.h"
#include "gdt.h"
#include "isr.h"
#include "console.h"
#include "heap.h"
#include "task.h"
#include "sched.h"
#include "paging.h"
#include "spinlock.h"
#include "lib.h"

/*
 * SMP bring-up and inter-processor interrupts (v0.7.0).
 *
 * Boot protocol (MP spec, "_INIT-SIPI-SIPI_"):
 *
 *   kmain (BSP)
 *     smp_early()      parse the MP table, switch to APIC mode
 *     smp_start_aps()  copy the trampoline to 0x8000, launch every AP;
 *                      each AP runs ap_entry() on its own boot stack and
 *                      parks there (timer masked, interrupts off)
 *     ...gates run strictly single-core...
 *     smp_release()    open the gate: APs start their LAPIC timer and
 *                      join the global runqueue through their idle task
 *
 * Scheduling model: ONE global runqueue guarded by sched_lock (see
 * spinlock.h). Every CPU's scheduler takes sched_lock, picks any
 * TASK_RUNNABLE task and marks it TASK_RUNNING -- the state machine plus
 * the lock is what keeps a task on exactly one CPU at a time. Load
 * balancing is implicit: idle CPUs simply pull the next task.
 */

/* set by mp.c */
extern bool mp_imcr_present;
int mp_parse(void);

extern char ap_trampoline[], ap_trampoline_end[];
extern char tramp_pml4[], tramp_entry[], tramp_stack[], tramp_cpu[];
extern char tramp_gdt_ptr[], tramp_gdt[];
extern void ap_context_enter(uint64_t kesp);       /* trampoline.S */

static volatile bool smp_gate;                     /* set by smp_release */

/* TLB shootdown mailbox: one slot, senders serialized by tlb_lock,
 * fire-and-forget (no ack -- see the header contract). */
static spinlock_t tlb_lock = SPINLOCK_INIT;
static volatile uint64_t tlb_pd, tlb_va;
static uint32_t tlb_broadcasts;

static inline void invlpg(uint64_t va) {
    __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
}

/* ---- boot-time ---- */

void smp_early(void) {
    ncpu = mp_parse();                             /* 1 when no MP table */
    apic_init();                                   /* LAPIC + IOAPIC, 8259 parked */
}

static char *tramp_data(char *sym) {
    return (char *)(uintptr_t)TRAMPOLINE_ADDR + (sym - ap_trampoline);
}

static void ap_idle_main(void *arg) {
    struct cpu *c = arg;
    (void)c;
    /* interrupts are on (EFL_IF in the first frame); every LAPIC-timer tick
     * re-evaluates the runqueue, so halt is the whole job */
    for (;;) halt();
}

/* AP C entry, jumped to from the trampoline with IF=0. Never returns. */
void ap_entry(int cpu_idx) {
    struct cpu *c;

    if (cpu_idx <= 0 || cpu_idx >= ncpu || cpus[cpu_idx].online) {
        for (;;) halt();                           /* bogus launch: stay dead */
    }
    c = &cpus[cpu_idx];

    /* Order matters: the real GDT first (selector 0x08 must be the 64-bit
     * code the IDT gates reference -- the trampoline's mini-GDT still has a
     * 32-bit descriptor there), the shared IDT second (from here on ANY
     * fault on this CPU dumps instead of triple-faulting into silence),
     * and only then the %gs anchor and everything that reads this_cpu(). */
    gdt_init_cpu(c);                               /* private GDT + TSS + LTR */
    idt_load();                                    /* shared IDT */
    percpu_set_gs(c);                              /* %gs -> our struct cpu */
    c->lapic_ticks = cpus[0].lapic_ticks;          /* BSP calibrated it */
    apic_percpu_init(c);                           /* LAPIC on, timer MASKED */

    /* per-CPU idle: a normal kernel thread, never picked by other CPUs
     * (is_idle) -- this CPU falls back to it when the runqueue is empty */
    c->idle = task_create_kernel("idle", ap_idle_main, c, 0);
    if (!c->idle) panic("ap%d: cannot create idle task", cpu_idx);
    c->idle->is_idle = true;

    c->online = true;

    /* park until the BSP opens the gate; the timer is masked and no IPI
     * can be pending (broadcasts only happen after release), so a plain
     * poll is safe. */
    while (!smp_gate) cpu_pause();

    /* adopt the idle context FIRST, then un-park: when the timer's first
     * tick fires (iretq below sets IF), this CPU is a valid scheduler
     * context with current/rsp0/kesp all wired up. */
    c->current = c->idle;
    c->idle->state = TASK_RUNNING;
    c->idle->on_cpu = cpu_idx;               /* adopted outside schedule(): take ownership by hand */
    tss_set_kstack(c->idle->kstack_top);
    apic_timer_start(c);
    ap_context_enter(c->idle->kesp);               /* noreturn */
}

void smp_start_aps(void) {
    uint32_t size = (uint32_t)(ap_trampoline_end - ap_trampoline);
    if (ncpu < 2) return;

    memcpy((void *)(uintptr_t)TRAMPOLINE_ADDR,
           (const void *)(uintptr_t)ap_trampoline, size);

    for (int i = 1; i < ncpu; i++) {
        struct cpu *c = &cpus[i];
        uint64_t stack = (uint64_t)(uintptr_t)kmalloc(KSTACK_SIZE);
        if (!stack) { kprintf("smp: no boot stack for cpu%d\n", i); continue; }

        /* per-AP data block in the 0x8000 copy */
        *(uint64_t *)(uintptr_t)tramp_data(tramp_pml4)  = paging_kernel_dir();
        *(uint64_t *)(uintptr_t)tramp_data(tramp_entry) = (uint64_t)(uintptr_t)ap_entry;
        *(uint64_t *)(uintptr_t)tramp_data(tramp_stack) = stack + KSTACK_SIZE;
        *(uint64_t *)(uintptr_t)tramp_data(tramp_cpu)   = (uint64_t)i;
        *(uint64_t *)(uintptr_t)(tramp_data(tramp_gdt_ptr) + 2) =
            TRAMPOLINE_ADDR + (uint64_t)(tramp_gdt - ap_trampoline);

        apic_send_init((uint8_t)c->apic_id);
        apic_udelay(10000);                        /* 10 ms INIT assert */
        apic_send_startup((uint8_t)c->apic_id, (uint8_t)(TRAMPOLINE_ADDR >> 12));
        apic_udelay(200);
        apic_send_startup((uint8_t)c->apic_id, (uint8_t)(TRAMPOLINE_ADDR >> 12));

        /* wait for ap_entry to signal online (3 s) */
        for (int ms = 0; ms < 3000 && !c->online; ms += 10)
            apic_udelay(10000);
        if (c->online)
            kprintf("smp: cpu%d (apic %d) online\n", i, c->apic_id);
        else
            kprintf("smp: cpu%d failed to come online -- running without it\n", i);
    }
}

void smp_release(void) {
    smp_gate = true;
}

/* ---- runtime ---- */

int smp_cpu_count(void) { return ncpu; }

int smp_cpus_online(void) {
    int n = 0;
    for (int i = 0; i < SMP_MAX; i++)
        if (cpus[i].online) n++;
    return n;
}

void smp_resched_others(void) {
    if (ncpu < 2) return;
    apic_ipi_broadcast(IPI_RESCHED);
}

void smp_tlb_shootdown(uint64_t page_dir, uint64_t va) {
    if (ncpu < 2) return;
    irq_flags_t f;
    spin_lock_irqsave(&tlb_lock, &f);
    tlb_pd = page_dir;
    tlb_va = va;
    tlb_broadcasts++;
    apic_ipi_broadcast(IPI_TLB_SHOOTDOWN);
    spin_unlock_irqrestore(&tlb_lock, f);
}

/* isr.c vector 0x51: invalidate va locally if we run this page directory */
void smp_tlb_shootdown_handler(void) {
    if (paging_cur_cr3() == tlb_pd)
        invlpg(tlb_va);
}

uint32_t smp_tlb_ipis_sent(void) { return tlb_broadcasts; }

/* v1.0.0: the `cpus` report, streamed through a caller sink -- the kernel
 * debug shell prints via shell_emit_console; sys_cpus appends the same
 * lines into a user buffer. */
void smp_report_cpus(shell_emit_t emit, void *ctx) {
    static const char hdr[] = "CPU  APIC  ONLINE  CURRENT\n";
    emit(hdr, (int)sizeof(hdr) - 1, ctx);
    for (int i = 0; i < SMP_MAX; i++) {
        struct cpu *c = &cpus[i];
        if (i >= ncpu && !c->online) continue;
        char row[96];
        int n = ksnprintf(row, sizeof(row), "%-4d %-5d %-7s %s\n", i, c->apic_id,
                          c->online ? "yes" : "no",
                          c->current ? c->current->name : "-");
        emit(row, n, ctx);
    }
}

void smp_dump_cpus(void) {
    smp_report_cpus(shell_emit_console, 0);
}
