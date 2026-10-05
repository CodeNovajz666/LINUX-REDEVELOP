#include "ulib.h"

/* hello: the canonical "user program is alive" demo.
 * prints its pid, sleeps a few times (proving timer+preemption work for
 * ring3), and exits with status 42 which the kernel test suite verifies. */
int main(void) {
    uprintf("[hello] user process running, pid=%d\n", ugetpid());
    for (int i = 3; i > 0; i--) {
        uprintf("[hello] tick %d...\n", i);
        usleep(400);       /* long enough for the shell test's Ctrl+C to land */
    }
    uprintf("[hello] exiting with status 42\n");
    return 42;
}
