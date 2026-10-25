#include "ulib.h"

/* head (v1.1.6): -n N (default 10) first lines of files or stdin. */

static int head_fd(int fd, long n) {
    char buf[512];
    long lines = 0;
    int got;
    while (n > 0 && (got = uread(fd, buf, sizeof(buf))) > 0) {
        int start = 0;
        for (int i = 0; i < got; i++) {
            if (buf[i] == '\n') {
                int len = i + 1 - start;
                if (uwrite_fd(1, buf + start, len) != len) return -1;
                start = i + 1;
                if (++lines >= n) return 0;
            }
        }
        if (start < got) {                     /* partial last line */
            if (uwrite_fd(1, buf + start, got - start) != got - start) return -1;
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    long n = 10;
    int first = 1;
    if (first < argc && ustrcmp(argv[first], "-n") == 0 && first + 1 < argc) {
        n = 0;
        for (const char *p = argv[first + 1]; *p; p++) {
            if (*p < '0' || *p > '9') { uputs("head: bad -n\n"); return 2; }
            n = n * 10 + (*p - '0');
        }
        first += 2;
    }
    int rc = 0;
    if (first >= argc) return head_fd(0, n) == 0 ? 0 : 1;
    for (int i = first; i < argc; i++) {
        int fd = uopen(argv[i], U_O_RDONLY);
        if (fd < 0) { uprintf("head: %s: no such file\n", argv[i]); rc = 1; continue; }
        if (head_fd(fd, n) != 0) rc = 1;
        uclose(fd);
    }
    return rc;
}
