#include "ulib.h"

/* forkacct: the COW page-accounting proof (test 20). Samples the kernel's
 * free-page count through SYS_FREEPAGES while forked children are alive:
 *
 *   - a child that writes nothing costs only page *tables* (a handful)
 *   - a child that writes four distinct data pages costs exactly four
 *     frames more than that
 *   - after both are reaped, every page comes back
 *
 * The children out-sleep the parent's sample point, so the measurement
 * window is deterministic under the 10 ms tick. */

#define NPAGES 4
static volatile char blob[NPAGES * 4096];

int main(void) {
    long base = ufreepages();

    /* child B: pure fork, no deliberate writes */
    int pid = ufork();
    if (pid < 0) { uprintf("[forkacct] fork failed\n"); return 1; }
    if (pid == 0) { usleep(400); uexit(0); }
    usleep(200);
    long fb = ufreepages();
    if (uwaitpid(pid) != 0) { uprintf("[forkacct] child B bad status\n"); return 1; }

    /* child A: fork + write four pages (one byte per page) */
    pid = ufork();
    if (pid < 0) { uprintf("[forkacct] fork failed\n"); return 1; }
    if (pid == 0) {
        for (int i = 0; i < NPAGES; i++)
            blob[i * 4096] = (char)(i + 1);     /* four COW copies */
        usleep(400);
        uexit(0);
    }
    usleep(200);
    long fa = ufreepages();
    if (uwaitpid(pid) != 0) { uprintf("[forkacct] child A bad status\n"); return 1; }

    long after = ufreepages();
    long cost_b = base - fb;                    /* tables + incidental stack */
    long cost_a = base - fa;
    long delta = cost_a - cost_b;               /* exactly the COW copies */

    uprintf("[forkacct] base=%ld quiet=%ld writing=%ld (delta=%ld, want %d)\n",
            base, cost_b, cost_a, delta, NPAGES);
    if (cost_b > 12) { uprintf("[forkacct] quiet fork too expensive\n"); return 2; }
    if (delta < NPAGES - 1 || delta > NPAGES + 1) { uprintf("[forkacct] COW copy count wrong\n"); return 3; }
    if (after != base) { uprintf("[forkacct] pages leaked (%ld != %ld)\n", after, base); return 4; }
    uprintf("[forkacct] FORKACCT-OK\n");
    return 0;
}
