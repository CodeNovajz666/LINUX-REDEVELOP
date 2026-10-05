#include "ulib.h"

/* uidtest: the setuid boundary walk, driven by the kernel ktest case
 * "setuid_boundary" (spawned as root with argv "magic42"). Proves three
 * things in one exit code: argv reaches main, root can step down to uid
 * 100, and a mortal can never climb back to 0. */

int main(int argc, char **argv) {
    if (argc != 2 || ustrcmp(argv[1], "magic42") != 0) {
        uprintf("uidtest: argv plumbing broken (argc=%d)\n", argc);
        return 1;
    }
    uprintf("uidtest: argv ok ('%s'), uid=%d ruid=%d\n",
            argv[1], ugetuid(), ugetruid());

    if (ugetuid() != 0) return 1;          /* spawned by the boot task */
    if (usetuid(100) != 0) return 1;       /* root may step down */
    if (ugetuid() != 100) return 1;
    if (usetuid(0) == 0) {
        uprintf("uidtest: mortal climbed back to uid 0!\n");
        return 1;
    }
    if (usetuid(100) != 0) return 1;       /* own uid stays allowed */
    /* root's setuid is the POSIX login drop: ruid moves with uid (that is
     * why `id` inside a login shell never shows a stray ruid=0). The other
     * half of ruid's job -- surviving SUID exec as the invoker's record --
     * is /bin/passwd's story, not this walk's. */
    if (ugetruid() != 100) return 1;

    uprintf("uidtest: boundary walk ok (uid 100, no way back to 0)\n");
    return 0;
}
