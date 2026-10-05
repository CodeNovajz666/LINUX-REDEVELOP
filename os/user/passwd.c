#include "ulib.h"
#include "etc.h"
#include "sha256.h"

/* passwd [USER]: change a password. /bin/passwd is the system's one SUID
 * binary (see fs_perm_fixup): a mortal execs it with effective uid 0,
 * real uid still theirs -- exactly the split this program needs. Root
 * may change anyone's password (skipping the old one); everyone else
 * must prove they know their current password first. */

static void read_password(const char *prompt, char *buf, int max) {
    uwrite(prompt, ustrlen(prompt));
    uecho_set(0);
    ureadline(buf, max);
    uecho_set(1);
    uwrite("\n", 1);
}

int main(int argc, char **argv) {
    int euid = ugetuid();                     /* 0 via SUID for mortals */
    int ruid = ugetruid();                    /* who really invoked us */
    struct pwent self;
    if (etc_getpwuid(ruid, &self) != 0) {
        uprintf("passwd: no passwd entry for uid %d\n", ruid);
        return 1;
    }
    const char *target = argc > 1 ? argv[1] : self.name;
    if (etc_getpwnam(target, 0) != 0 && ustrcmp(target, self.name) != 0) {
        uprintf("passwd: unknown user %s\n", target);
        return 1;
    }
    int changing_other = ustrcmp(target, self.name) != 0;
    if (changing_other && euid != 0) {
        uprintf("passwd: only root may change another user's password\n");
        return 1;
    }

    /* root changing someone else skips the old password */
    if (!(euid == 0 && changing_other)) {
        char cur[40];
        read_password("Current password: ", cur, sizeof(cur));
        if (etc_verify(self.name, cur) != 0) {
            uprintf("passwd: authentication failure\n");
            return 1;
        }
    }

    char pw1[40], pw2[40];
    read_password("New password: ", pw1, sizeof(pw1));
    read_password("Retype new password: ", pw2, sizeof(pw2));
    if (!pw1[0]) {
        uprintf("passwd: empty password not allowed\n");
        return 1;
    }
    if (ustrcmp(pw1, pw2) != 0) {
        uprintf("passwd: passwords do not match\n");
        return 1;
    }

    char salt[9], msg[72], hash[65];
    etc_gen_token(salt, 8);
    unsigned long pl = ustrlen(pw1);
    if (pl > ETC_PW_MAX) pl = ETC_PW_MAX;       /* same clamp as etc_verify */
    umemcpy(msg, salt, 8);
    umemcpy(msg + 8, pw1, pl);
    sha256_hex(msg, 8 + pl, hash);

    if (etc_shadow_set(target, salt, hash) != 0) {
        uprintf("passwd: cannot write /etc/shadow\n");
        return 1;
    }
    uprintf("passwd: password updated for %s\n", target);
    return 0;
}
