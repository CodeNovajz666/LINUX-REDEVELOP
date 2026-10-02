#include "ulib.h"

/* malloctest: the v0.5.0 heap proof.
 *
 * 1. 256 chunks of 4 KiB payload, each written and read back
 * 2. all freed -> the free list coalesces into one big run
 * 3. a 1 MiB malloc then comes entirely from the coalesced list: the
 *    kernel free-page count must NOT move (no new sbrk)
 * 4. the 1 MiB block is fully written and verified
 *
 * exit code 55 = everything held. */
#define NCHUNK 256
#define PAYLOAD 4096

static unsigned char *chunks[NCHUNK];

int main(void) {
    long pages_before = ufreepages();

    for (int i = 0; i < NCHUNK; i++) {
        chunks[i] = umalloc(PAYLOAD);
        if (!chunks[i]) { uprintf("[malloctest] malloc(%d) failed at %d\n", PAYLOAD, i); return 1; }
        for (int j = 0; j < PAYLOAD; j++)
            chunks[i][j] = (unsigned char)(i + j);
    }
    for (int i = 0; i < NCHUNK; i++)
        for (int j = 0; j < PAYLOAD; j++)
            if (chunks[i][j] != (unsigned char)(i + j)) {
                uprintf("[malloctest] chunk %d byte %d corrupted\n", i, j);
                return 2;
            }

    for (int i = 0; i < NCHUNK; i++)
        ufree(chunks[i]);

    /* the coalescing proof: 1 MiB without touching the kernel's pages */
    long pages_mid = ufreepages();
    unsigned char *big = umalloc(1UL << 20);
    if (!big) { uprintf("[malloctest] 1MiB malloc failed\n"); return 3; }
    long pages_after = ufreepages();
    if (pages_after != pages_mid) {
        uprintf("[malloctest] no coalescing: %ld -> %ld pages (sbrk happened)\n",
                pages_mid, pages_after);
        return 4;
    }

    for (unsigned long i = 0; i < (1UL << 20); i++)
        big[i] = (unsigned char)(i * 7);
    for (unsigned long i = 0; i < (1UL << 20); i++)
        if (big[i] != (unsigned char)(i * 7)) {
            uprintf("[malloctest] big block corrupted at %lu\n", i);
            return 5;
        }
    ufree(big);

    /* a tiny allocation must still work after all that */
    char *tiny = umalloc(1);
    if (!tiny) { uprintf("[malloctest] tiny malloc failed\n"); return 6; }
    *tiny = 42;
    if (*tiny != 42) { uprintf("[malloctest] tiny block corrupted\n"); return 7; }
    ufree(tiny);

    uprintf("[malloctest] 256x4K + 1MiB round-trip, coalescing ok (heap pages: %ld)\n",
            pages_before - pages_mid);
    uprintf("[malloctest] MALLOCTEST-OK\n");
    return 55;
}
