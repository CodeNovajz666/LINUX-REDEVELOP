#include "ulib.h"

/* rm FILE...: unlink files (SYS_UNLINK). The VFS refuses directories and
 * requires write permission on the parent -- the Unix rm(1) core, nothing
 * more (no -r, no -f; teaching scope). */

int main(int argc, char **argv) {
    if (argc < 2) {
        uputs("usage: rm FILE...\n");
        return 1;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (uunlink(argv[i]) != 0) {
            uprintf("rm: %s: failed (missing, is a directory, or parent not writable)\n",
                    argv[i]);
            rc = 1;
        }
    }
    return rc;
}
