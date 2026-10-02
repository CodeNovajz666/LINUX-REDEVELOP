#include "ulib.h"

/* fault_div: deliberately divides by zero in ring 3. The kernel must kill
 * this process with SIGFPE (exit status 128+8) instead of panicking --
 * the v0.1.1 robustness test. */
int main(void) {
    uprintf("[fault_div] pid=%d, dividing by zero...\n", ugetpid());
    volatile int zero = 0;
    volatile int r = 42 / zero;
    uprintf("[fault_div] unreachable: %d\n", r);
    return 0;
}
