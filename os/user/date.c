#include "ulib.h"

/* date: print the hardware wall clock. v1.1.0: SYS_DATETIME streams the
 * same line the kernel debug shell's `date` prints (local time verbatim;
 * QEMU runs -rtc base=localtime, no timezone machinery). */

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    char buf[96];
    int n = udatetime(buf, (int)sizeof(buf) - 1);
    if (n < 0) {
        uputs("date: RTC not sane\n");
        return 1;
    }
    buf[n] = 0;
    uputs(buf);
    return 0;
}
