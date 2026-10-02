#ifndef GDT_H
#define GDT_H

#include <stdint.h>
#include <stdbool.h>
#include "percpu.h"

/* v0.7.0: every CPU owns its GDT + TSS (RSP0 must follow the task that
 * CPU is about to run). gdt_init() builds cpus[0]; APs call
 * gdt_init_cpu() from ap_entry. */
void gdt_init(void);
void gdt_init_cpu(struct cpu *c);
/* RSP0 of the *current* CPU's TSS (called from schedule()) */
void tss_set_kstack(uint64_t rsp0);

#endif /* GDT_H */
