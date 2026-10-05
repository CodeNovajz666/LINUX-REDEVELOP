#include "ulib.h"
#include "etc.h"

/* id: who am I? Prints effective uid/gid with names from /etc/passwd,
 * plus the real uid when it differs (i.e. inside a SUID binary). */

static void ids(int n, const char *kind) {
    struct pwent pw;
    const char *name = "unknown";
    if (etc_getpwuid(n, &pw) == 0)
        name = pw.name;
    uprintf("%s=%d(%s)", kind, n, name);
}

int main(void) {
    ids(ugetuid(), "uid");
    uputs(" ");
    ids(ugetgid(), "gid");
    int ruid = ugetruid();
    if (ruid != ugetuid())
        uprintf(" ruid=%d", ruid);
    uputs("\n");
    return 0;
}
