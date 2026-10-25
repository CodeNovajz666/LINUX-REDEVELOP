#include "ulib.h"
#include "etc.h"

/* register: the first-boot account wizard (v1.0.0). getty execs this
 * instead of login while /etc/passwd holds no accounts -- the out-of-box
 * disk ships zero of them, so the first human to boot the machine creates
 * its credentials instead of being asked for some:
 *   step 1 -- root's password (a system without an administrator could
 *             never run useradd, which is root-only)
 *   step 2 -- the first mortal account; an empty username skips it and
 *             leaves a valid root-only system
 * Runs as root (child of the init -> getty chain). Writing is shadow-first:
 * an interrupted wizard leaves shadow-only lines (harmless -- shadow_set
 * replaces by name on the retry) rather than a passwd entry nobody can
 * log in with. When it exits, init respawns getty, which now sees the
 * accounts and shows the normal login prompt.
 * A mortal execing it later is stopped twice: the guard below and the
 * 0600 /etc/shadow VFS check. */

static void read_password(const char *prompt, char *buf, int max) {
    uwrite(prompt, ustrlen(prompt));
    uecho_set(0);
    ureadline(buf, max);
    uecho_set(1);
    uwrite("\n", 1);
}

/* prompt + retype, non-empty and matching; three rounds. 0 = got it */
static int ask_password(const char *prompt, char *out, int max) {
    for (int attempt = 0; attempt < 3; attempt++) {
        char again[40];
        read_password(prompt, out, max);
        read_password("Retype new password: ", again, sizeof(again));
        if (out[0] && ustrcmp(out, again) == 0) return 0;
        uputs("Passwords are empty or do not match -- try again\n");
    }
    return -1;
}

/* v1.1.3: the one shared hasher (etc.c) -- fresh salt + PBKDF2 digest;
 * register/useradd/passwd used to carry three drifting copies of this */
static int set_password(const char *name, const char *pw) {
    char salt[9], hash[65];
    if (etc_hash_password(pw, salt, hash) != 0) return -1;
    return etc_shadow_set(name, salt, ETC_PBKDF2_ITERS, hash);
}

int main(void) {
    if (ugetuid() != 0) {
        uputs("register: accounts are created on the first boot, by root\n");
        return 1;
    }
    uputs("\nThis is a fresh Novix system -- no user accounts exist yet.\n");
    uputs("Registration sets the administrator (root) password, then\n");
    uputs("creates your first user account.\n\n");

    char rpw[40];
    if (ask_password("New password for root: ", rpw, sizeof(rpw)) != 0) {
        uputs("register: too many failures\n");
        return 1;
    }
    if (set_password("root", rpw) != 0) {
        uputs("register: cannot write /etc/shadow\n");
        return 1;
    }
    /* a retry after an interrupted run already has the line: appending
     * again would give root two passwd entries */
    if (etc_getpwnam("root", 0) != 0) {
        struct pwent root;
        ustrcpy(root.name, "root");
        root.uid = 0;
        root.gid = 0;
        ustrcpy(root.home, "/root");
        ustrcpy(root.shell, "/bin/osh");
        if (etc_passwd_append(&root) != 0) {
            uputs("register: cannot write /etc/passwd\n");
            return 1;
        }
    }
    uputs("register: root password set\n\n");

    char name[24];
    bool skipped = false, have_user = false;
    for (int attempt = 0; attempt < 3; attempt++) {
        uputs("Choose a username (empty for a root-only system): ");
        ureadline(name, sizeof(name));
        if (!name[0]) {
            skipped = true;
            break;
        }
        if (!etc_valid_name(name)) {
            uputs("register: name must be 1-23 characters without ':'\n");
            continue;
        }
        if (etc_getpwnam(name, 0) == 0) {
            uprintf("register: user %s already exists\n", name);
            continue;
        }
        have_user = true;
        break;
    }
    if (skipped) {
        uputs("register: no user account created (root only)\n");
        uputs("Registration complete. Please log in.\n");
        return 0;
    }
    if (!have_user) {
        uputs("register: too many failures\n");
        return 1;
    }

    char upw[40];
    if (ask_password("New password: ", upw, sizeof(upw)) != 0) {
        uputs("register: too many failures\n");
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

    /* shadow before passwd (see the header comment); a home left over on
     * the disk is adopted and chowned instead of failing the run */
    if (set_password(name, upw) != 0) {
        uputs("register: cannot write /etc/shadow\n");
        return 1;
    }
    if (umkdir(e.home) != 0)
        uprintf("register: warning: cannot create %s\n", e.home);
    uchown(e.home, e.uid, e.gid);
    if (etc_passwd_append(&e) != 0) {
        uputs("register: cannot write /etc/passwd\n");
        return 1;
    }

    uprintf("register: account '%s' created (uid %d, home %s)\n",
            name, e.uid, e.home);
    uputs("Registration complete. Please log in.\n");
    return 0;
}
