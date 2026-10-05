#include "ulib.h"

/* grep PATTERN [FILE...]: print every line containing PATTERN (plain
 * substring -- regexes are 1.x territory). With no FILE, or FILE "-", the
 * standard input is searched: the pipeline half of osh's acceptance line
 * `cat /etc/motd | grep Novix > /tmp/x`.
 * Exit codes: 0 = something matched, 1 = no match, 2 = trouble. */

#define LINE_MAX 256

static const char *pattern;
static bool matched;

static void emit(const char *line, int len) {
    uwrite(line, len);
    uwrite("\n", 1);
}

/* substring match over one stream; '\r' is ignored (guest serial artifacts) */
static void scan_stream(int fd) {
    char buf[512];
    char line[LINE_MAX];
    int fill = 0;
    int n;
    while ((n = uread(fd, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\r') continue;
            if (c == '\n') {
                line[fill] = 0;
                if (ustrstr(line, pattern)) {
                    emit(line, fill);
                    matched = true;
                }
                fill = 0;
                continue;
            }
            if (fill < LINE_MAX - 1) line[fill++] = c;
        }
    }
    if (fill > 0) {                             /* trailing line, no newline */
        line[fill] = 0;
        if (ustrstr(line, pattern)) {
            emit(line, fill);
            matched = true;
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        uputs("usage: grep PATTERN [FILE...]\n");
        return 2;
    }
    pattern = argv[1];
    matched = false;

    if (argc == 2) {
        scan_stream(0);
    } else {
        for (int a = 2; a < argc; a++) {
            if (ustrcmp(argv[a], "-") == 0) {
                scan_stream(0);
                continue;
            }
            int fd = uopen(argv[a], U_O_RDONLY);
            if (fd < 0) {
                uprintf("grep: cannot open %s\n", argv[a]);
                return 2;
            }
            scan_stream(fd);
            uclose(fd);
        }
    }
    return matched ? 0 : 1;
}
