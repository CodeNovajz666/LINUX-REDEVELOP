#include "ulib.h"

/* wc (v1.1.6): -l -w -c over files or stdin (default: all three).
 * Teaching scope: fixed 4 KiB chunks, counts split on whitespace. */

static unsigned long nl, nw, nc;

static void count_buf(const char *b, int n) {
    bool in_word = false;
    for (int i = 0; i < n; i++) {
        nc++;
        if (b[i] == '\n') nl++;
        if (b[i] == ' ' || b[i] == '\t' || b[i] == '\n') in_word = false;
        else if (!in_word) { in_word = true; nw++; }
    }
}

static int count_fd(int fd) {
    char buf[512];
    int n;
    while ((n = uread(fd, buf, sizeof(buf))) > 0) count_buf(buf, n);
    return n < 0 ? -1 : 0;
}

static bool sel_l, sel_w, sel_c;                /* which counts to print */

static void put_num(unsigned long v) {
    char tmp[24];
    int i = 0;
    if (v == 0) tmp[i++] = '0';
    while (v) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
    while (i > 0) uwrite_fd(1, &tmp[--i], 1);
}

static void report(const char *name) {
    int shown = 0;
    if (sel_l) { if (shown++) uwrite_fd(1, " ", 1); put_num(nl); }
    if (sel_w) { if (shown++) uwrite_fd(1, " ", 1); put_num(nw); }
    if (sel_c) { if (shown++) uwrite_fd(1, " ", 1); put_num(nc); }
    if (name) { uwrite_fd(1, " ", 1); uputs(name); }
    uwrite_fd(1, "\n", 1);
}

int main(int argc, char **argv) {
    int first = 1;
    while (first < argc && argv[first][0] == '-') {
        for (const char *p = argv[first] + 1; *p; p++) {
            if (*p == 'l') sel_l = true;
            else if (*p == 'w') sel_w = true;
            else if (*p == 'c') sel_c = true;
            else { uprintf("wc: unknown flag -%c\n", *p); return 2; }
        }
        first++;
    }
    if (!sel_l && !sel_w && !sel_c) sel_l = sel_w = sel_c = true;

    int rc = 0;
    if (first >= argc) {
        if (count_fd(0) != 0) { uputs("wc: read error\n"); return 1; }
        report(0);
        return rc;
    }
    for (int i = first; i < argc; i++) {
        nl = nw = nc = 0;
        int fd = uopen(argv[i], U_O_RDONLY);
        if (fd < 0) { uprintf("wc: %s: no such file\n", argv[i]); rc = 1; continue; }
        if (count_fd(fd) != 0) { uprintf("wc: %s: read error\n", argv[i]); rc = 1; }
        uclose(fd);
        report(argv[i]);
    }
    return rc;
}
