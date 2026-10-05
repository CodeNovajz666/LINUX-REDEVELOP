#include "ulib.h"

/* touch FILE...: create files that don't exist yet (an existing file is
 * left alone -- no timestamps to update in this filesystem). Creating
 * requires write permission on the parent directory, which is exactly
 * the permission boundary the v0.9.0 tests probe. */

int main(int argc, char **argv) {
    if (argc < 2) {
        uputs("usage: touch FILE...\n");
        return 1;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        struct ustat st;
        int exists = ustat(argv[i], &st) == 0;
        int fd = uopen(argv[i], exists ? U_O_WRONLY : (U_O_CREAT | U_O_WRONLY));
        if (fd < 0) {
            if (exists)
                uprintf("touch: %s: permission denied\n", argv[i]);
            else
                uprintf("touch: %s: cannot create\n", argv[i]);
            rc = 1;
            continue;
        }
        uclose(fd);
    }
    return rc;
}
