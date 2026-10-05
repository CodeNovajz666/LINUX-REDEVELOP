#include "ulib.h"
#include "etc.h"

/* login <username>: the credential gate. Verifies the password against
 * /etc/shadow (salted SHA-256), then drops to the account's uid/gid and
 * execs its shell. Runs as root (spawned by init via getty), so the
 * setuid pair below is the privilege-dropping point of the whole system.
 *
 * Unix convention: a wrong username and a wrong password are reported
 * identically ("Login incorrect") so an attacker cannot enumerate users.
 * Three failures cost a 3-second pause before getty comes back. */

int main(int argc, char **argv) {
    const char *user = argc > 1 ? argv[1] : "";
    if (!user[0]) {
        uprintf("login: no username\n");
        return 1;
    }

    struct pwent pw;
    int have_pw = etc_getpwnam(user, &pw) == 0;

    for (int attempt = 0; attempt < 3; attempt++) {
        char pass[40];
        uputs("Password: ");
        uecho_set(0);
        ureadline(pass, sizeof(pass));
        uecho_set(1);
        uputs("\n");

        if (have_pw && etc_verify(user, pass) == 0) {
            etc_utmp_login(user);               /* `who` reads this */
            usetgid(pw.gid);
            usetuid(pw.uid);
            if (uchdir(pw.home) != 0)
                uchdir("/");
            /* v1.0.0: /etc/motd after a successful login (world-readable,
             * so this runs after the privilege drop like real login(1)) */
            int mfd = uopen("/etc/motd", U_O_RDONLY);
            if (mfd >= 0) {
                char mbuf[256];
                int mn;
                while ((mn = uread(mfd, mbuf, sizeof(mbuf))) > 0)
                    uwrite_fd(1, mbuf, mn);
                uclose(mfd);
            }
            /* argv[0] "-sh": the login-shell convention */
            const char *shargv[1] = { "-sh" };
            uexec_argv(pw.shell, shargv, 1);
            uprintf("login: cannot exec %s\n", pw.shell);
            return 1;
        }
        uprintf("Login incorrect\n");
    }
    usleep(3000);                               /* brute-force brake */
    return 1;
}
