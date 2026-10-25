#include "ulib.h"

/* mkdir DIR...: create directories (SYS_MKDIR, in the kernel since v0.9.0
 * -- v1.1.2 finally gives osh the /bin tool that calls it, alongside
 * rmdir; the parent's W+X is checked inside the VFS). */

int main(int argc, char **argv) {
    if (argc < 2) {
        uputs("usage: mkdir DIR...\n");
        return 1;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (umkdir(argv[i]) == 0) continue;
        struct ustat st;
        if (ustat(argv[i], &st) == 0)
            uprintf("mkdir: %s: already exists\n", argv[i]);
        else
            uprintf("mkdir: %s: failed (parent missing or not writable)\n", argv[i]);
        rc = 1;
    }
    return rc;
}
