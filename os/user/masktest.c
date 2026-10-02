#include "ulib.h"

/* Signal masking end to end: prove SIGKILL cannot be blocked, block
 * SIGTERM, kill ourselves, prove the handler does NOT run while blocked,
 * then unblock and prove delivery happens on the way out of the very
 * sigprocmask syscall that unblocked it (the interrupt return path
 * re-checks sigpending & ~sigblocked).
 *
 * exit 44 = all stages passed. */

static volatile int got = 0;

static void handler(int sig) {
    (void)sig;
    got++;
}

int main(void) {
    if (usignal(U_SIGTERM, handler) != 0) {
        uprintf("sigaction failed\n");
        uexit(1);
    }

    /* SIGKILL must stay unblockable even if the caller asks for it */
    unsigned int km = 1u << (U_SIGKILL - 1);
    unsigned int after = 0;
    if (usigprocmask(U_SIG_SETMASK, &km, 0) != 0) {
        uprintf("sigprocmask SETMASK failed\n");
        uexit(2);
    }
    if (usigprocmask(U_SIG_SETMASK, 0, &after) != 0) {   /* set = NULL: read */
        uprintf("sigprocmask readback failed\n");
        uexit(3);
    }
    if (after != 0) {
        uprintf("SIGKILL was blockable (mask=%x)\n", after);
        uexit(4);
    }

    /* block SIGTERM, then make one pending on ourselves */
    unsigned int term = 1u << (U_SIGTERM - 1);
    unsigned int old = 0;
    if (usigprocmask(U_SIG_BLOCK, &term, &old) != 0) {
        uprintf("sigprocmask BLOCK failed\n");
        uexit(5);
    }
    if (old & term) {
        uprintf("old mask unexpectedly has SIGTERM set\n");
        uexit(6);
    }

    if (ukill(ugetpid(), U_SIGTERM) != 0) {
        uprintf("kill failed\n");
        uexit(7);
    }

    /* SIGTERM is pending but blocked: hand the CPU away a few times -- if
     * the mask were ignored, delivery would run the handler right now */
    for (int i = 0; i < 5; i++) uyield();
    if (got != 0) {
        uprintf("handler ran while signal was blocked!\n");
        uexit(8);
    }
    uprintf("alive: SIGTERM pending, blocked\n");

    /* unblock: the handler fires before the next user instruction runs */
    if (usigprocmask(U_SIG_UNBLOCK, &term, &old) != 0) {
        uprintf("sigprocmask UNBLOCK failed\n");
        uexit(9);
    }
    if (!(old & term)) {
        uprintf("mask before unblock lacked SIGTERM\n");
        uexit(10);
    }
    if (got != 1) {
        uprintf("handler not delivered after unblock (got=%d)\n", got);
        uexit(11);
    }

    uprintf("MASKTEST-OK\n");
    uexit(44);
}
