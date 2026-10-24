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
    /* the name rule (length, no ':') lives in etc_valid_name, shared with
     * the register wizard -- two copies here would drift apart */
    if (!etc_valid_name(name)) {
        uputs("useradd: name must be 1-23 characters without ':'\n");
        return 1;
    }
    if (etc_getpwnam(name, 0) == 0) {
        uprintf("useradd: user %s already exists\n", name);
        return 1;
    }

    struct pwent e;
    ustrncpy(e.name, name, sizeof(e.name) - 1);
    e.name[sizeof(e.name) - 1] = 0;
    e.uid = etc_max_uid() + 1;
    if (e.uid < 100) e.uid = 100;
    e.gid = e.uid;
    ustrcpy(e.home, "/home/");
    ustrncpy(e.home + 6, name, sizeof(e.home) - 7);
    e.home[sizeof(e.home) - 1] = 0;
    ustrcpy(e.shell, "/bin/osh");

    /* shadow before passwd (register's rule, v1.0.1): a crash between
     * the two writes leaves a shadow-only row, which shadow_set replaces
     * by name on the retry -- a passwd row without a shadow row could
     * never be logged into and would block the retry ("already exists") */
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

    if (umkdir(e.home) != 0) {
        uprintf("useradd: warning: cannot create %s\n", e.home);
    }
    uchown(e.home, e.uid, e.gid);

    if (etc_passwd_append(&e) != 0) {
        uprintf("useradd: cannot write /etc/passwd\n");
        return 1;      /* the shadow row stays: harmless, replaced on retry */
    }

    uprintf("useradd: added %s (uid %d)\n", name, e.uid);
    uprintf("useradd: password for %s: %s\n", name, pw);
    return 0;
}
