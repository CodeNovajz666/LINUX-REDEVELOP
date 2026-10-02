#include "ulib.h"

/* sigtest: a caught signal must not kill the process -- the handler runs
 * and execution continues to a normal exit (the acceptance test for the
 * v0.5.0 delivery machinery: frame rewrite -> handler -> sigreturn
 * trampoline -> restored context).
 *
 * round 1: signal delivered on the syscall return path (ukill returns
 *          straight into the handler)
 * round 2: signal delivered after a sleep, i.e. through the scheduler's
 *          delivery point (schedule -> signal_deliver_pending)
 *
 * exit code 77 = both rounds caught, program survived. */

volatile int caught = 0;

static void onterm(int sig) {
    (void)sig;
    caught = 1;
}

int main(void) {
    if (usignal(U_SIGTERM, onterm) != 0) {
        uprintf("[sigtest] sigaction failed\n");
        return 1;
    }

    /* round 1: immediate delivery on the int 0x80 return path */
    if (ukill(ugetpid(), U_SIGTERM) != 0) {
        uprintf("[sigtest] kill(self) failed\n");
        return 2;
    }
    if (!caught) {
        uprintf("[sigtest] handler did not run before ukill returned\n");
        return 3;
    }

    /* round 2: delivery through a sleep/yield cycle */
    caught = 0;
    if (ukill(ugetpid(), U_SIGTERM) != 0) {
        uprintf("[sigtest] second kill(self) failed\n");
        return 4;
    }
    usleep(30);
    uyield();
    if (!caught) {
        uprintf("[sigtest] handler did not run across sleep\n");
        return 5;
    }

    uprintf("[sigtest] SIGTERM caught twice and survived both, SIGTEST-OK\n");
    return 77;
}
