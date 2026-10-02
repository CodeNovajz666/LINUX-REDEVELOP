#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

void timer_init(uint32_t hz);
uint64_t timer_ticks(void);
uint64_t timer_uptime_ms(void);
void timer_tick(void);          /* called from IRQ0 */
void timer_sleep_ms(uint32_t ms);   /* blocks the current task */

#endif /* TIMER_H */
