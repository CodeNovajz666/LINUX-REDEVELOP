#include "ulib.h"

/* cp SRC DST: copy one regular file through open/read/write (no sendfile,
 * no holes -- the 0.2 syscall base is the whole story). DST is created
 * with the caller's uid/gid and default 0644 mode. */

#define U_S_IFMT  0xF000
#define U_S_IFDIR 0x4000

int main(int argc, char **argv) {
    if (argc != 3) {
        uputs("usage: cp SRC DST\n");
        return 1;
    }
    struct ustat st;
    if (ustat(argv[1], &st) != 0) {
        uprintf("cp: %s: no such file\n", argv[1]);
        return 1;
    }
    if ((st.st_mode & U_S_IFMT) == U_S_IFDIR) {
        uprintf("cp: %s: is a directory\n", argv[1]);
        return 1;
    }
    if (ustat(argv[2], &st) == 0 && (st.st_mode & U_S_IFMT) == U_S_IFDIR) {
        uprintf("cp: %s: is a directory\n", argv[2]);
        return 1;
    }
    int in = uopen(argv[1], U_O_RDONLY);
    if (in < 0) {
        uprintf("cp: %s: permission denied\n", argv[1]);
        return 1;
    }
    int out = uopen(argv[2], U_O_WRONLY | U_O_CREAT | U_O_TRUNC);
    if (out < 0) {
        uprintf("cp: %s: cannot create\n", argv[2]);
        uclose(in);
        return 1;
    }
    char buf[128];
    int n, rc = 0;
    while ((n = uread(in, buf, sizeof(buf))) > 0) {
        int off = 0;
        while (off < n) {
            int w = uwrite_fd(out, buf + off, n - off);
            if (w <= 0) { uputs("cp: write failed\n"); rc = 1; break; }
            off += w;
        }
        if (rc) break;
    }
    uclose(in);
    uclose(out);
    return rc;
}
