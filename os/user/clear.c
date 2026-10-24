#include "ulib.h"

/* clear: wipe the screen and home the cursor. v1.1.0: ioctl(TCCLEAR) on
 * stdout -- the console device executes it; the escape sequence never
 * travels through user space (the VGA console is not ANSI-driven). */

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    if (uclear() != 0) {
        uputs("clear: no console\n");
        return 1;
    }
    return 0;
}
