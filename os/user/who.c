#include "ulib.h"
#include "etc.h"

/* who: list login sessions from /var/run/utmp ("user:tty:pid" lines that
 * login writes). Sessions stay listed until the next login rewrites the
 * tty -- there is no logout hook yet (v1.0.0 territory). */

int main(void) {
    static char buf[512];
    int n = etc_read_file("/var/run/utmp", buf, sizeof(buf));
    if (n <= 0) {
        uputs("who: no sessions\n");
        return 1;
    }
    for (char *line = buf; line && *line; ) {
        char *nl = ustrchr(line, '\n');
        if (nl) *nl = 0;
        char *c1 = ustrchr(line, ':');
        char *c2 = c1 ? ustrchr(c1 + 1, ':') : 0;
        if (c1 && c2) {
            *c1 = 0;
            *c2 = 0;
            uprintf("%s   %s   pid %s\n", line, c1 + 1, c2 + 1);
        }
        line = nl ? nl + 1 : 0;
    }
    return 0;
}
