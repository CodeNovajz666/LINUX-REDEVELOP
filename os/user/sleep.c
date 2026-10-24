#include "ulib.h"

/* sleep N: pause for N seconds (SYS_SLEEP, which ticks in milliseconds).
 * Strict decimal parse -- garbage is refused, not silently zeroed. */

static int parse_seconds(const char *s) {
    if (!*s) return -1;
    long v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return -1;
        v = v * 10 + (*s - '0');
        if (v > 60) return -1;            /* cap: a teaching tool, not a timer daemon */
    }
    return (int)v;
}

int main(int argc, char **argv) {
    int n = 1;
    if (argc > 1) {
        n = parse_seconds(argv[1]);
        if (n < 0) {
            uprintf("sleep: %s: invalid number (0-60)\n", argv[1]);
            return 1;
        }
    }
    usleep(n * 1000);
    return 0;
}
