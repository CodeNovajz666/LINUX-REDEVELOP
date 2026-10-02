#ifndef APIC_H
#define APIC_H

#include <stdint.h>
#include <stdbool.h>
#include "percpu.h"

/* local APIC + IOAPIC (v0.7.0). The 8259 PIC is masked forever: every
 * external IRQ is routed through the IOAPIC (timer/keyboard/serial to the
 * BSP), each CPU takes its own LAPIC-timer tick, and IPIs drive resched
 * and TLB shootdown. */

/* BSP boot-time setup: enable the LAPIC, mask the PIC, wire the IOAPIC
 * (IRQ0/1/4 -> BSP), calibrate the LAPIC timer against PIT channel 2. */
void apic_init(void);

/* per-CPU setup (BSP + every AP): spurious vector, LVT masking and the
 * periodic LAPIC timer at TICK_HZ -- armed but MASKED, so a booting AP stays
 * parked (no tick, no preemption) until apic_timer_start(). */
void apic_percpu_init(struct cpu *c);

/* un-park this CPU: the LAPIC timer starts firing VEC_LAPIC_TIMER */
void apic_timer_start(struct cpu *c);

void apic_eoi(void);
uint8_t apic_lapic_id(void);

/* INIT-SIPI-SIPI sequence pieces (smp.c drives the protocol) */
void apic_send_init(uint8_t dest_apic_id);
void apic_send_startup(uint8_t dest_apic_id, uint8_t vector);

/* fixed IPI to every CPU except the caller */
void apic_ipi_broadcast(uint8_t vector);
/* fixed IPI to one APIC id */
void apic_ipi_single(uint8_t dest_apic_id, uint8_t vector);

/* busy-wait microsecond delay via PIT channel 2 (works with IF=0) */
void apic_udelay(uint32_t us);

#endif /* APIC_H */
