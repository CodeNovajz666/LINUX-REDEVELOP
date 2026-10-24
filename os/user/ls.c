#include "ulib.h"

/* ls [PATH]: print a directory listing. v1.1.0: the listing itself is
 * kernel-formatted (shell_report_dir via SYS_GETDENTS) -- byte-identical
 * to the kernel debug shell's `ls`, including the "N entries" tail.
 * Listing a directory needs its read permission; default PATH is cwd. */

int main(int argc, char **argv) {
    char path[80];
    if (argc < 2) {
        if (ugetcwd(path, sizeof(path)) < 0) ustrcpy(path, "/");
    } else {
        ustrncpy(path, argv[1], sizeof(path) - 1);
        path[sizeof(path) - 1] = 0;
    }
    /* 4 KiB: a two-digit entry count with lines <= 64B fits ~60 entries */
    static char buf[4096];
    int n = ugetdents(path, buf, (int)sizeof(buf) - 1);
    if (n < 0) {
        uprintf("ls: %s: no such directory (or permission denied)\n", path);
        return 1;
    }
    buf[n] = 0;
    uputs(buf);
    return 0;
}
