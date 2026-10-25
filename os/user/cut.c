#include "ulib.h"

/* cut (v1.1.6): -c N-M (1-based inclusive chars) or -d SEP -f N (fields,
 * 1-based; a line with fewer fields prints empty). Files or stdin. */

static long c_from = -1, c_to = -1;
static char delim = ':';
static long field = -1;

static void cut_line(const char *s, unsigned long len) {
    if (c_from > 0) {
        for (unsigned long i = c_from - 1; i < len && (long)i < c_to; i++)
            uwrite_fd(1, s + i, 1);
    } else {
        long f = 1;
        unsigned long beg = 0;
        for (unsigned long i = 0; i <= len; i++) {
            if (i == len || s[i] == delim) {
                if (f == field) { uwrite_fd(1, s + beg, (int)(i - beg)); break; }
                f++;
                beg = i + 1;
            }
        }
    }
    uwrite_fd(1, "\n", 1);
}

static void cut_fd(int fd) {
    static char line[256];
    int n = 0, got;
    char ch;
    while ((got = uread(fd, &ch, 1)) == 1) {
        if (ch == '\n') {
            cut_line(line, (unsigned long)n);
            n = 0;
        } else if (n < (int)sizeof(line) - 1) {
            line[n++] = ch;
        }
    }
    if (n > 0) cut_line(line, (unsigned long)n);
}

int main(int argc, char **argv) {
    int first = 1;
    while (first < argc && argv[first][0] == '-') {
        const char *a = argv[first];
        if (a[1] == 'c' && first + 1 < argc) {
            const char *p = argv[++first];
            c_from = 0;
            while (*p >= '0' && *p <= '9') c_from = c_from * 10 + (*p++ - '0');
            if (*p == '-') {
                p++;
                c_to = 0;
                while (*p >= '0' && *p <= '9') c_to = c_to * 10 + (*p++ - '0');
                if (c_to == 0) c_to = 9999;     /* open end: -c N- */
            } else {
                c_to = c_from;                  /* single char */
            }
        } else if (a[1] == 'd' && first + 1 < argc) {
            delim = argv[++first][0];
        } else if (a[1] == 'f' && first + 1 < argc) {
            field = 0;
            for (const char *p = argv[++first]; *p >= '0' && *p <= '9'; p++)
                field = field * 10 + (*p - '0');
        } else {
            uprintf("cut: unknown flag %s\n", a);
            return 2;
        }
        first++;
    }
    if (c_from < 0 && field < 0) { uputs("usage: cut -c N-M | -d SEP -f N [FILE]\n"); return 2; }
    if (first >= argc) { cut_fd(0); return 0; }
    for (int i = first; i < argc; i++) {
        int fd = uopen(argv[i], U_O_RDONLY);
        if (fd < 0) { uprintf("cut: %s: no such file\n", argv[i]); return 1; }
        cut_fd(fd);
        uclose(fd);
    }
    return 0;
}
