#include "ulib.h"

/* mv SRC DST: move a regular file. Teaching simplification, honestly: the
 * VFS has no rename(2), so mv is copy + unlink here (same content, new
 * inode, no atomicity across directories). Cross-device works for the
 * same reason -- ramfs /tmp <-> FAT root just copies. */

#define U_S_IFMT  0xF000
#define U_S_IFDIR 0x4000

int main(int argc, char **argv) {
    if (argc != 3) {
        uputs("usage: mv SRC DST\n");
        return 1;
    }
    struct ustat st;
    if (ustat(argv[1], &st) != 0) {
        uprintf("mv: %s: no such file\n", argv[1]);
        return 1;
    }
    if ((st.st_mode & U_S_IFMT) == U_S_IFDIR) {
        uprintf("mv: %s: is a directory (not supported)\n", argv[1]);
        return 1;
    }
    /* cp SRC DST (same open/read/write loop as /bin/cp) */
    int in = uopen(argv[1], U_O_RDONLY);
    if (in < 0) {
        uprintf("mv: %s: permission denied\n", argv[1]);
        return 1;
    }
    int out = uopen(argv[2], U_O_WRONLY | U_O_CREAT | U_O_TRUNC);
    if (out < 0) {
        uprintf("mv: %s: cannot create\n", argv[2]);
        uclose(in);
        return 1;
    }
    char buf[128];
    int n, rc = 0;
    while ((n = uread(in, buf, sizeof(buf))) > 0) {
        int off = 0;
        while (off < n) {
            int w = uwrite_fd(out, buf + off, n - off);
            if (w <= 0) { uputs("mv: write failed\n"); rc = 1; break; }
            off += w;
        }
        if (rc) break;
    }
    uclose(in);
    uclose(out);
    if (rc) return 1;
    /* only unlink the source after the copy fully succeeded */
    if (uunlink(argv[1]) != 0) {
        uprintf("mv: %s: cannot remove source\n", argv[1]);
        return 1;
    }
    return 0;
}
