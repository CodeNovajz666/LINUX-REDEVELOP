#ifndef SMP_H
#define SMP_H

#include <stdint.h>
#include <stdbool.h>
#include "novix.h"
#include "percpu.h"
#include "shell.h"                             /* shell_emit_t (smp_report_cpus) */

/* interrupt vector layout (v0.7.0):
 *   0x00-0x1F  CPU exceptions
 *   0x20-0x2F  legacy ISA IRQs, delivered by the IOAPIC (BSP only)
 *   0x30       LAPIC timer (every CPU, local preemption)
 *   0x50       RESCHED IPI (wake/steal hint)
 *   0x51       TLB-shootdown IPI
 *   0x80       syscall (int 0x80, DPL 3)
 *   0xFE       LAPIC error
 *   0xFF       LAPIC spurious (no EOI) */
#define VEC_LAPIC_TIMER   0x30
#define IPI_RESCHED       0x50
#define IPI_TLB_SHOOTDOWN 0x51

/* boot-time: parse the MP table, switch the machine to APIC mode
 * (LAPIC + IOAPIC, 8259 parked). Runs before sti. */
void smp_early(void);

/* boot-time, after the first tasks exist: copy the trampoline to 0x8000
 * and bring every AP through INIT-SIPI-SIPI. APs come up parked (their
 * LAPIC timer masked) until smp_release(). */
void smp_start_aps(void);

/* runtime: open the gate -- APs unmask their LAPIC timer and join the
 * runqueues. Called by init once the single-core boot gates are green. */
void smp_release(void);

int  smp_cpu_count(void);                 /* discovered (MP table) */
int  smp_cpus_online(void);               /* through ap_entry */

/* resched IPI to every other online CPU (cheap: they re-check their
 * runqueues; parked APs ignore it) */
void smp_resched_others(void);

/* Invalidate one page on every CPU currently running `page_dir` (CR3
 * match).  Requests are published into a fixed ring before the broadcast,
 * so a slow target CPU cannot lose a range to a newer request; if it falls
 * more than TLB_RING_SIZE behind it reloads CR3 (full flush) instead.
 * Callers hold sched_lock, which serializes concurrent senders. */
void smp_tlb_shootdown(uint64_t page_dir, uint64_t va);
/* same, for a whole page-aligned range [from, to): one broadcast instead
 * of one IPI per page (the handler full-flushes when the range is wide) */
void smp_tlb_shootdown_range(uint64_t page_dir, uint64_t from, uint64_t to);
void smp_tlb_shootdown_handler(void);     /* isr.c: vector 0x51 */

/* count of TLB shootdown IPI broadcasts actually sent (ktest 32 probe) */
uint32_t smp_tlb_ipis_sent(void);

/* shell "cpus" command */
void smp_dump_cpus(void);
/* v1.0.0: the same `cpus` report streamed to a caller sink (sys_cpus) */
void smp_report_cpus(shell_emit_t emit, void *ctx);

#endif /* SMP_H */
