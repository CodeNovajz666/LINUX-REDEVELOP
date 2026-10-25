#include "ulib.h"

/* stackovf (v1.1.4): unbounded recursion, every frame touching a local
 * buffer, walking the stack pointer off the bottom of the stack VMA. The
 * guard region below (never mapped, unreachable by mmap since the
 * allocator is capped below it) turns the fault into a clean SIGSEGV
 * kill -- exit 139 territory, never a panic, never silent corruption. */

static int dive(int d) {
    volatile char pad[256];                 /* volatile: every frame is real */
    for (int i = 0; i < 256; i++) pad[i] = (char)(d + i);
    if (d > 1000000) return pad[d & 255];   /* unreachable at 8 stack pages */
    return dive(d + 1) + pad[(d + 7) & 255];
}

int main(void) {
    uputs("stackovf: diving\n");
    int r = dive(0);
    uprintf("stackovf: returned %d -- the guard did not hold!\n", r);
    return 42;
}
