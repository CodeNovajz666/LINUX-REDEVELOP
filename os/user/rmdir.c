#include "ulib.h"

/* rmdir DIR...: remove empty directories (v1.1.2, SYS_RMDIR). The kernel
 * refuses anything that is not a directory or not empty -- the two error
 * paths print differently so a failed cleanup says why. */

int main(int argc, char **argv) {
    if (argc < 2) {
        uputs("usage: rmdir DIR...\n");
        return 1;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (urmdir(argv[i]) == 0) continue;
        struct ustat st;
        if (ustat(argv[i], &st) != 0)
            uprintf("rmdir: %s: no such directory\n", argv[i]);
        else if ((st.st_mode & 0xF000) != 0x4000)
            uprintf("rmdir: %s: not a directory\n", argv[i]);
        else
            uprintf("rmdir: %s: directory not empty\n", argv[i]);
        rc = 1;
    }
    return rc;
}
