#include "ulib.h"

/* iotest: the ktest dup2_roundtrip carrier (spawned by the kernel suite as
 * root). Proves the plumbing osh is built on:
 *   1. pipe + fork + dup2: a child's stdout lands in the parent's pipe
 *   2. dup2 edge cases: invalid oldfd, same-fd no-op, high newfd
 *   3. file redirection: dup2'd fd writes reach the file on disk
 * Exit status 0 = all good; anything else is a failed assert. */

static const char marker[] = "DUP2-CHILD-OK\n";

static int test_pipe_child(void) {
    int fds[2];
    if (upipe(fds) != 0) return 1;
    int pid = ufork();
    if (pid < 0) return 1;
    if (pid == 0) {
        if (udup2(fds[1], 1) != 1) uexit(2);
        uclose(fds[0]);
        uclose(fds[1]);
        uwrite(marker, (int)sizeof(marker) - 1);
        uexit(0);
    }
    uclose(fds[1]);                             /* parent is reader-only */
    char buf[64];
    int total = 0, n;
    while (total < (int)sizeof(marker) - 1 &&
           (n = uread(fds[0], buf + total, sizeof(buf) - 1 - total)) > 0)
        total += n;
    uclose(fds[0]);
    if (uwaitpid(pid) != 0) return 1;
    if (total != (int)sizeof(marker) - 1) return 1;
    if (ustrncmp(buf, marker, (int)sizeof(marker) - 1) != 0) return 1;
    return 0;
}

static int test_dup2_edges(void) {
    if (udup2(-1, 5) >= 0) return 1;            /* invalid oldfd */
    if (udup2(99, 5) >= 0) return 1;            /* beyond the table */
    int fd = uopen("/dev/null", U_O_WRONLY);
    if (fd < 0) return 1;
    if (udup2(fd, fd) != fd) return 1;          /* same-fd no-op */
    if (udup2(fd, 5) != 5) return 1;            /* high newfd joins the file */
    uwrite_fd(5, "x", 1);
    uclose(5);
    uclose(fd);
    return 0;
}

static int test_file_redirect(void) {
    const char *path = "/tmp/dup2-file.txt";
    int fd = uopen(path, U_O_WRONLY | U_O_CREAT | U_O_TRUNC);
    if (fd < 0) return 1;
    int wfd = udup2(fd, 7);
    if (wfd != 7) return 1;
    uclose(fd);                                 /* the dup keeps the file open */
    const char *msg = "redirected through dup2\n";
    if (uwrite_fd(7, msg, (int)ustrlen(msg)) != (int)ustrlen(msg)) return 1;
    uclose(7);

    int r = uopen(path, U_O_RDONLY);
    if (r < 0) return 1;
    char buf[64];
    int n = uread(r, buf, sizeof(buf) - 1);
    uclose(r);
    if (n != (int)ustrlen(msg) || ustrncmp(buf, msg, (unsigned long)n) != 0)
        return 1;
    return 0;
}

int main(void) {
    if (test_pipe_child() != 0) {
        uputs("[iotest] pipe+fork+dup2 FAILED\n");
        return 1;
    }
    uputs("[iotest] pipe+fork+dup2 ok (child stdout -> parent pipe)\n");

    if (test_dup2_edges() != 0) {
        uputs("[iotest] dup2 edges FAILED\n");
        return 1;
    }
    uputs("[iotest] dup2 edges ok (bad oldfd, same-fd, high newfd)\n");

    if (test_file_redirect() != 0) {
        uputs("[iotest] file redirect FAILED\n");
        return 1;
    }
    uputs("[iotest] file redirect ok (dup'd fd reaches the disk)\n");
    uputs("[iotest] IOTEST-OK\n");
    return 0;
}
