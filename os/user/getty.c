#include "ulib.h"

/* getty: the terminal's front door. Prints /etc/issue, prompts for a
 * username, then EXECs /bin/login <name> -- no fork, the chain
 * init -> getty -> login -> sh is one process re-using one pid, so the
 * shell exiting unwinds straight back to init, which respawns us.
 * Everything login rejects also lands here again through that respawn. */

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
