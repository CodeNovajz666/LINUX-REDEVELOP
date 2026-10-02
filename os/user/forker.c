#include "ulib.h"

/* forker: the direct COW proof. Parent and child share the same global
 * variable through identical COW pages; the child's write must trigger a
 * copy and leave the parent's value untouched. Exits 0 when isolation
 * holds (test 19 in the kernel suite greps the exit status). */

static volatile int g = 111;

int main(void) {
    uprintf("[forker] pid=%d, g=%d before fork\n", ugetpid(), g);

    int pid = ufork();
    if (pid < 0) {
        uprintf("[forker] fork failed (%d)\n", pid);
        return 1;
    }
    if (pid == 0) {
        g = 222;                        /* write fault -> COW copy */
        uprintf("[forker] child pid=%d sees g=%d\n", ugetpid(), g);
        uexit(g == 222 ? 33 : 34);
    }

    int st = uwaitpid(pid);
    uprintf("[forker] parent sees g=%d (must be 111), child status %d\n", g, st);
    if (g == 111 && st == 33) {
        uprintf("[forker] COW-ISOLATION-OK\n");
        return 0;
    }
    uprintf("[forker] COW isolation FAILED\n");
    return 2;
}
