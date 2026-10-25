#include "ulib.h"

/* tail (v1.1.6): -n N (default 10) last lines. A ring of the newest N
 * lines (each capped 256 bytes) is filled by one pass, then replayed. */

#define T_MAX_LINES 64
#define T_LINE_MAX  256

static char ring[T_MAX_LINES][T_LINE_MAX];
static int ring_len[T_MAX_LINES];
static int r_head, r_count;

static void feed(const char *b, int n) {
    static int cur_len;
    for (int i = 0; i < n; i++) {
        if (b[i] == '\n') {
            ring_len[r_head] = cur_len;
            r_head = (r_head + 1) % T_MAX_LINES;
            if (r_count < T_MAX_LINES) r_count++;
            cur_len = 0;
        } else if (cur_len < T_LINE_MAX - 1) {
            ring[r_head][cur_len++] = b[i];
        }
    }
    ring_len[r_head] = cur_len;                 /* partial last line */
}

int main(int argc, char **argv) {
    long n = 10;
    int first = 1;
    if (first < argc && ustrcmp(argv[first], "-n") == 0 && first + 1 < argc) {
        n = 0;
        for (const char *p = argv[first + 1]; *p; p++) {
            if (*p < '0' || *p > '9') { uputs("tail: bad -n\n"); return 2; }
            n = n * 10 + (*p - '0');
        }
        first += 2;
    }
    if (n > T_MAX_LINES) n = T_MAX_LINES;

    char buf[512];
    int got;
    if (first < argc) {
        for (int i = first; i < argc; i++) {
            int fd = uopen(argv[i], U_O_RDONLY);
            if (fd < 0) { uprintf("tail: %s: no such file\n", argv[i]); return 1; }
            while ((got = uread(fd, buf, sizeof(buf))) > 0) feed(buf, got);
            uclose(fd);
        }
    } else {
        while ((got = uread(0, buf, sizeof(buf))) > 0) feed(buf, got);
    }

    long skip = r_count - n;
    if (skip < 0) skip = 0;
    int idx = (r_head - r_count + T_MAX_LINES) % T_MAX_LINES;
    for (long i = 0; i < r_count; i++) {
        if (i >= skip) {
            uwrite_fd(1, ring[idx], ring_len[idx]);
            uwrite_fd(1, "\n", 1);
        }
        idx = (idx + 1) % T_MAX_LINES;
    }
    return 0;
}
