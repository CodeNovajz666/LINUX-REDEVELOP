#include "ulib.h"

/* mv SRC DST: move a regular file, exactly like every real mv:
 * rename(2) first -- same inode, same content bytes, no data copy, works
 * inside one filesystem instance (v1.1.2 gave the kernel RENAME);
 * fall back to copy + unlink when rename refuses (cross-instance move,
 * e.g. ramfs /tmp <-> the FAT root -- the EXDEV case GNU mv handles the
 * same way). Directory sources stay refused: the FAT rename path moves
 * them only within their parent, and mv's honest scope is files. */

#define U_S_IFMT  0xF000
#define U_S_IFDIR 0x4000

static int copy_file(const char *src, const char *dst) {
    int in = uopen(src, U_O_RDONLY);
    if (in < 0) {
        uprintf("mv: %s: permission denied\n", src);
        return -1;
    }
    int out = uopen(dst, U_O_WRONLY | U_O_CREAT | U_O_TRUNC);
    if (out < 0) {
        uprintf("mv: %s: cannot create\n", dst);
        uclose(in);
        return -1;
    }
    char buf[128];
    int n, rc = 0;
    while ((n = uread(in, buf, sizeof(buf))) > 0) {
        int off = 0;
        while (off < n) {
            int w = uwrite_fd(out, buf + off, n - off);
            if (w <= 0) { uputs("mv: write failed\n"); rc = -1; break; }
            off += w;
        }
        if (rc) break;
    }
    uclose(in);
    uclose(out);
    return rc;
}

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
    /* fast path: same instance -> the inode itself moves (stat-provable) */
    if (urename(argv[1], argv[2]) == 0)
        return 0;
    /* rename refused (cross-instance or target exists): copy + unlink,
     * source unlinked only after the copy fully succeeded */
    if (copy_file(argv[1], argv[2]) != 0)
        return 1;
    if (uunlink(argv[1]) != 0) {
        uprintf("mv: %s: cannot remove source\n", argv[1]);
        return 1;
    }
    return 0;
}
