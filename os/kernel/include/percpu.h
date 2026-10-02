#ifndef PERCPU_H
#define PERCPU_H

#include <stdint.h>
#include <stdbool.h>
#include "novix.h"

struct task;

/*
 * v0.7.0 per-CPU state. Each CPU's IA32_GS_BASE (MSR 0xC0000101) points at
 * its struct cpu, so %gs-relative addressing reaches it from any context.
 * User programs never load GS, so the kernel keeps GS for itself and no
 * swapgs is needed anywhere.
 *
 * struct cpu embeds the per-CPU GDT + TSS (RSP0 switches with the current
 * task) and the scheduling state that must be per-CPU: `current`,
 * `need_resched` and the RR rotation cursor. The task list and task states
 * stay global, guarded by sched_lock (see spinlock.h for the order).
 */

/* x86_64 TSS: privilege stacks are RSP0/1/2 at offsets 4/12/20 (ISTs
 * unused). iomap_base at offset 92 terminates the (empty) I/O bitmap. */
struct tss64 {
    uint32_t prev;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t ist[7];
    uint32_t zero0;
    uint32_t zero1;
    uint16_t iomap_base;
    uint16_t pad;
} __attribute__((packed));

struct cpu {
    struct cpu *self;          /* %gs:0 -> this struct (this_cpu anchor) */
    int id;                    /* logical index 0..ncpu-1 */
    int apic_id;               /* hardware LAPIC id */
    volatile bool online;      /* AP completed ap_entry (BSP: true) */
    struct task *current;      /* what this CPU is executing */
    struct task *idle;         /* per-CPU idle task (never enqueued) */
    volatile bool need_resched;
    struct task *rr_last;      /* SCHED_ROUND_ROBIN rotation cursor */
    uint32_t lapic_ticks;      /* calibrated LAPIC-timer count per tick */
    uint64_t gdt[7] __attribute__((aligned(8)));   /* 5 descriptors + TSS64 */
    struct tss64 tss __attribute__((aligned(16)));
};

extern struct cpu cpus[SMP_MAX];
extern int ncpu;               /* 1 when no MP table / -smp 1 */

static inline struct cpu *this_cpu(void) {
    struct cpu *c;
    __asm__ volatile("movq %%gs:0, %0" : "=r"(c) :: "memory");
    return c;
}

/* set this CPU's GS base (wrmsr; call once per CPU at bringup) */
void percpu_set_gs(struct cpu *c);

/* BSP init: zero cpus[], wire cpus[0], set its GS base */
void percpu_init(void);

#endif /* PERCPU_H */
