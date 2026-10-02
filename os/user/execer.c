#include "ulib.h"

/* execer: prints one line, then replaces itself with /bin/hello. If the
 * kernel ever returns from uexec, the replacement failed; otherwise the
 * next output belongs to hello and the process exits with 42 (test 21
 * verifies the status; the pid must not change). */

int main(void) {
    int pid = ugetpid();
    uprintf("[execer] pid=%d, replacing image with /bin/hello...\n", pid);

    int r = uexec("/bin/hello");

    /* not reached on success */
    uprintf("[execer] exec FAILED with %d\n", r);
    return 99;
}
