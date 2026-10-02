#ifndef SIGNAL_H
#define SIGNAL_H

#include "isr.h"
#include "task.h"

/* v0.5.0 signal delivery -- see kernel/signal.c */
void signal_deliver_pending(struct task *t, struct pt_regs *r);

/* Generate a signal in `target`: registers a handler delivery (pending bit +
 * wake) when the target can catch it, otherwise takes the default action
 * (terminate). Returns 0, or -1 for a bad signal number / dead target. */
int signal_send(struct task *target, int sig);

#endif /* SIGNAL_H */
