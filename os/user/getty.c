#include "ulib.h"
#include "etc.h"

/* getty: the terminal's front door. Prints /etc/issue, prompts for a
 * username, then EXECs /bin/login <name> -- no fork, the chain
 * init -> getty -> login -> sh is one process re-using one pid, so the
 * shell exiting unwinds straight back to init, which respawns us.
 * Everything login rejects also lands here again through that respawn.
 * v1.0.0: while /etc/passwd holds no accounts (a fresh disk image ships
 * zero), the front door is the registration wizard instead -- it execs
 * /bin/register, which exits back into the respawn, and the next pass
 * around this loop sees the accounts and shows the login prompt. */

static void print_issue(void) {
    int fd = uopen("/etc/issue", U_O_RDONLY);
    if (fd < 0) return;
    char buf[128];
    int n;
    while ((n = uread(fd, buf, sizeof(buf))) > 0)
        uwrite_fd(1, buf, n);
    uclose(fd);
}

int main(void) {
    char user[32];
    for (;;) {
        if (etc_user_count() == 0) {
            const char *argv[1] = { "register" };
            uexec_argv("/bin/register", argv, 1);
            uprintf("getty: cannot exec /bin/register\n");
            usleep(1000);
            continue;
        }
        print_issue();
        uputs("login: ");
        ureadline(user, sizeof(user));
        if (!user[0]) continue;              /* empty line: re-prompt */
        /* one argv slot beyond argv[0]: the username */
        const char *argv[2] = { "login", user };
        uexec_argv("/bin/login", argv, 2);
        uprintf("getty: cannot exec /bin/login\n");
        usleep(1000);
    }
}
