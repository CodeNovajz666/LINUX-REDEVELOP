#include "novix.h"
#include "percpu.h"
#include "lib.h"

/*
 * v0.7.0 per-CPU anchors. One struct cpu per possible CPU; %gs-relative
 * access through this_cpu(). The GS base MSR is written exactly once per
 * CPU at bringup (percpu_init for the BSP, ap_entry for APs) and never
 * changes after -- user space never loads GS, so no swapgs dance exists.
 */

#define MSR_IA32_GS_BASE 0xC0000101u

struct cpu cpus[SMP_MAX];
int ncpu = 1;

void percpu_set_gs(struct cpu *c) {
    wrmsr(MSR_IA32_GS_BASE, (uint64_t)(uintptr_t)c);
}

void percpu_init(void) {
    memset(cpus, 0, sizeof(cpus));
    for (int i = 0; i < SMP_MAX; i++) {
        cpus[i].id = i;
        cpus[i].apic_id = -1;
        cpus[i].self = &cpus[i];    /* %gs:0 must work on APs too, not just slot 0 */
    }
    cpus[0].online = true;        /* the BSP is up by definition */
    ncpu = 1;
    percpu_set_gs(&cpus[0]);
}
