#include "ulib.h"

/* cat FILE...: dump files to stdout. stat-then-open so "no such file"
 * and "permission denied" are distinguishable without errno plumbing. */

#define U_S_IFMT  0xF000
#define U_S_IFDIR 0x4000

int main(int argc, char **argv) {
    if (argc < 2) {
        uputs("usage: cat FILE...\n");
        return 1;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        struct ustat st;
        if (ustat(argv[i], &st) != 0) {
            uprintf("cat: %s: no such file\n", argv[i]);
            rc = 1;
            continue;
        }
        if ((st.st_mode & U_S_IFMT) == U_S_IFDIR) {
            uprintf("cat: %s: is a directory\n", argv[i]);
            rc = 1;
            continue;
        }
        int fd = uopen(argv[i], U_O_RDONLY);
        if (fd < 0) {
            uprintf("cat: %s: permission denied\n", argv[i]);
            rc = 1;
            continue;
        }
        char buf[128];
        int n;
        while ((n = uread(fd, buf, sizeof(buf))) > 0)
            uwrite_fd(1, buf, n);
        uclose(fd);
    }
    return rc;
}
