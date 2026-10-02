#include "ulib.h"

/* fault_segv: dereferences the near-null pointer 0x1. The low 4 MiB is
 * identity-mapped supervisor-only, so ring 3 access page-faults; the
 * kernel must kill this process with SIGSEGV (exit status 128+11). */
int main(void) {
    uprintf("[fault_segv] pid=%d, dereferencing 0x1...\n", ugetpid());
    volatile int *p = (volatile int *)0x1;
    volatile int v = *p;
    uprintf("[fault_segv] unreachable: %d\n", v);
    return 0;
}
