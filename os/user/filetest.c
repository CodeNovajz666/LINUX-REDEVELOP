/* filetest: user-space exercise of the v0.2.0 file syscalls against
 * /tmp/filetest.txt -- create, write, read, lseek, stat, append, close.
 * Each check prints on failure; exit code 0 = all passed. */
#include "ulib.h"

static int fails;

static void check(const char *what, long got, long want) {
    if (got != want) {
        uprintf("filetest: %s: got %d want %d\n", what, got, want);
        fails++;
    }
}

static int str_eq(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}

int main(void) {
    const char *path = "/tmp/filetest.txt";
    char buf[32];

    /* create + write + close */
    int fd = uopen(path, U_O_WRONLY | U_O_CREAT | U_O_TRUNC);
    check("open create", fd, 3);
    check("write", uwrite_fd(fd, "novix files", 11), 11);
    check("close", uclose(fd), 0);

    /* read back, EOF, lseek, re-read */
    fd = uopen(path, U_O_RDONLY);
    check("open rdonly", fd, 3);
    check("read", uread(fd, buf, sizeof(buf)), 11);
    check("content", str_eq(buf, "novix files", 11), 1);
    check("eof", uread(fd, buf, sizeof(buf)), 0);
    check("lseek set", ulseek(fd, 0, U_SEEK_SET), 0);
    check("read after seek", uread(fd, buf, 4), 4);
    check("seek content", str_eq(buf, "novi", 4), 1);
    check("lseek past eof", ulseek(fd, 999, U_SEEK_SET), -1);
    check("close again", uclose(fd), 0);

    /* stat */
    struct ustat st;
    check("stat", ustat(path, &st), 0);
    check("stat size", st.st_size, 11);
    check("stat missing", ustat("/tmp/definitely-missing", &st), -1);

    /* bad fd handling */
    check("read bad fd", uread(99, buf, 4), -1);
    check("close bad fd", uclose(99), -1);
    check("open missing w/o creat", uopen("/tmp/definitely-missing", U_O_RDONLY), -1);

    /* append writes land at the end */
    fd = uopen(path, U_O_WRONLY | U_O_APPEND);
    check("open append", fd, 3);
    check("append write", uwrite_fd(fd, "!", 1), 1);
    uclose(fd);
    fd = uopen(path, U_O_RDONLY);
    check("append size", uread(fd, buf, sizeof(buf)), 12);
    check("append content", str_eq(buf, "novix files!", 12), 1);
    uclose(fd);

    if (fails == 0) {
        uprintf("filetest: all checks passed\n");
        uexit(0);
    }
    uprintf("filetest: %d check(s) failed\n", fails);
    uexit(1);
}
