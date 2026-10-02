#include "ulib.h"

/* memhog: the v0.6.0 demand-paging / swap proof.
 *
 * 1. mmap 1 MiB anonymous, write a per-page pattern (first + last word)
 * 2. sleep 600 ms -- with the ktest watermark forced high, kswapd evicts
 *    the pages while we sleep; with the default watermark nothing moves
 * 3. verify every page: evicted ones fault back in through the swap-in
 *    path, and the pattern proves the bytes survived the round trip
 * 4. munmap and exit
 *
 * Deliberately no globals and no umalloc: the ELF's own file pages stay
 * clean, so under pressure they are dropped and re-faulted from the
 * inode, never written back into it.
 *
 * exit code 88 = everything held. */

#define PAGES 256                     /* 1 MiB of 4 KiB pages */
#define KPAGE  4096UL

int main(void) {
    unsigned long *mem = ummap(PAGES * KPAGE);
    if (!mem) { uprintf("[memhog] mmap failed\n"); return 1; }
    uprintf("[memhog] %d pages at %lx, filling\n", PAGES, (unsigned long)mem);

    for (int i = 0; i < PAGES; i++) {
        unsigned long *p = mem + i * (KPAGE / sizeof(unsigned long));
        p[0] = 0xC0DE0000UL + (unsigned long)i;
        p[KPAGE / sizeof(unsigned long) - 1] = ~(0xC0DE0000UL + (unsigned long)i);
    }

    usleep(600);                      /* kswapd's window */

    uprintf("[memhog] verifying\n");
    for (int i = 0; i < PAGES; i++) {
        unsigned long *p = mem + i * (KPAGE / sizeof(unsigned long));
        unsigned long v = 0xC0DE0000UL + (unsigned long)i;
        if (p[0] != v || p[KPAGE / sizeof(unsigned long) - 1] != ~v) {
            uprintf("[memhog] page %d corrupted (%lx)\n", i, p[0]);
            return 2;
        }
    }

    if (umunmap(mem, PAGES * KPAGE) != 0) {
        uprintf("[memhog] munmap failed\n");
        return 3;
    }

    uprintf("[memhog] 1 MiB mmap/verify/munmap round trip ok\n");
    uprintf("[memhog] MEMHOG-OK\n");
    return 88;
}
