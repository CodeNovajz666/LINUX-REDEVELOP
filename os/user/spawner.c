#include "ulib.h"

/* spawner: the classic Unix recipe, done with fork+exec (v0.4.0 rewrite --
 * it used spawn in earlier versions). Two fork/exec/waitpid rounds, each
 * child becomes /bin/hello and exits 42; the spawner itself exits 7. */

int main(void) {
    uprintf("[spawner] pid=%d, fork+exec two children\n", ugetpid());

    int pids[2] = { -1, -1 };
    for (int i = 0; i < 2; i++) {
        int pid = ufork();
        if (pid < 0) {
            uprintf("[spawner] fork failed (%d)\n", pid);
            return 1;
        }
        if (pid == 0) {
            int r = uexec("hello");             /* /bin/hello */
            uprintf("[spawner] child exec failed (%d)\n", r);
            return 98;
        }
        pids[i] = pid;
    }

    uprintf("[spawner] children: %d, %d; waiting...\n", pids[0], pids[1]);
    int sa = uwaitpid(pids[0]);
    int sb = uwaitpid(pids[1]);
    uprintf("[spawner] child %d -> %d, child %d -> %d\n", pids[0], sa, pids[1], sb);
    if (sa != 42 || sb != 42) {
        uprintf("[spawner] unexpected child status!\n");
        return 2;
    }
    uprintf("[spawner] all children reaped, exiting with 7\n");
    return 7;
}
