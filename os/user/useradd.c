#include "ulib.h"
#include "etc.h"
#include "sha256.h"

/* useradd NAME: root-only account creation. Appends to /etc/passwd and
 * /etc/shadow, creates and chowns /home/NAME, and prints a generated
 * password (the account is immediately usable; `passwd` changes it).
 * Not SUID: a mortal execs it with their own uid and is refused. */

int main(int argc, char **argv) {
    if (ugetuid() != 0) {
        uputs("useradd: only root can add users\n");
        return 1;
    }
    if (argc != 2 || !argv[1][0]) {
        uputs("usage: useradd NAME\n");
        return 1;
    }
    const char *name = argv[1];
    /* the name must survive every consumer intact: pwent.name is 24 bytes,
     * and a ':' would split the passwd/shadow line early (getpwnam would
     * then disagree with shadow_lookup and the account could never log in) */
    unsigned long nlen = ustrlen(name);
    if (nlen > sizeof(((struct pwent *)0)->name) - 1 || ustrchr(name, ':')) {
        uputs("useradd: name must be 1-23 characters without ':'\n");
        return 1;
    }
    if (etc_getpwnam(name, 0) == 0) {
        uprintf("useradd: user %s already exists\n", name);
        return 1;
    }

    struct pwent e;
    ustrncpy(e.name, name, sizeof(e.name) - 1);
    e.uid = etc_max_uid() + 1;
    if (e.uid < 100) e.uid = 100;
    e.gid = e.uid;
    ustrcpy(e.home, "/home/");
    ustrncpy(e.home + 6, name, sizeof(e.home) - 7);
    ustrcpy(e.shell, "/bin/osh");

    if (umkdir(e.home) != 0) {
        uprintf("useradd: warning: cannot create %s\n", e.home);
    }
    uchown(e.home, e.uid, e.gid);

    if (etc_passwd_append(&e) != 0) {
        uprintf("useradd: cannot write /etc/passwd\n");
        return 1;
    }

    char salt[9], pw[9], msg[24], hash[65];
    etc_gen_token(salt, 8);
    etc_gen_token(pw, 8);
    umemcpy(msg, salt, 8);
    umemcpy(msg + 8, pw, 8);
    sha256_hex(msg, 16, hash);
    if (etc_shadow_set(name, salt, hash) != 0) {
        uprintf("useradd: cannot write /etc/shadow\n");
        return 1;
    }

    uprintf("useradd: added %s (uid %d)\n", name, e.uid);
    uprintf("useradd: password for %s: %s\n", name, pw);
    return 0;
}
