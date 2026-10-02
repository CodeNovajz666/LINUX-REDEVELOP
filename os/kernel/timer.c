#include "novix.h"
#include "timer.h"
#include "task.h"
#include "sched.h"
#include "percpu.h"

static volatile uint64_t ticks;

void timer_init(uint32_t hz) {
    /* v0.7.0: the PIT still runs (kept as the calibration/uptime base), but
     * IRQ0 is masked at the IOAPIC -- every CPU's LAPIC timer is the tick
     * source now, so the 8259 line fires into nothing. */
    uint32_t divisor = 1193182 / hz;
    outb(0x43, 0x36);              /* ch0, lo/hi, mode 3 */
    outb(0x40, divisor & 0xFF);
    outb(0x40, (divisor >> 8) & 0xFF);
}

uint64_t timer_ticks(void) { return ticks; }
uint64_t timer_uptime_ms(void) { return ticks * (1000 / TICK_HZ); }

void timer_tick(void) {
    /* every CPU calls this from its LAPIC timer; only the BSP advances the
     * global count (uptime, sleep deadlines), while each CPU still runs its
     * own preemption/aging pass below */
    if (this_cpu()->id == 0)
        ticks++;
    sched_on_tick(ticks);
}

void timer_sleep_ms(uint32_t ms) {
    struct task *cur = current_task();
    if (!cur) return;
    task_sleep_until(cur, timer_ticks() + (uint64_t)ms * TICK_HZ / 1000 + 1);
    kyield();
}
