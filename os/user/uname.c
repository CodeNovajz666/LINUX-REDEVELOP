#include "ulib.h"

/* uname: name and version of the running system (a static fact here --
 * this kernel has no utsname syscall; the banner string is the source). */

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    uputs("Novix 1.1.0 x86_64 -- a real OS, grown out of a kernel module\n");
    return 0;
}
