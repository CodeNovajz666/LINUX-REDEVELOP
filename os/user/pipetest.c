#include "ulib.h"

/* pipetest: fork + two pipes = bidirectional parent/child channel.
 *
 *   parent --(p2c: 1 KiB pattern)--> child verifies
 *   child  --(c2p: "ok")-----------> parent verifies
 *   parent closes p2c write end -> child reads EOF (0)
 *   child exits 33 -> parent reaps it
 *
 * exit code 66 = data integrity + EOF + status all correct. */

int main(void) {
    int p2c[2], c2p[2];
    if (upipe(p2c) != 0 || upipe(c2p) != 0) {
        uprintf("[pipetest] pipe creation failed\n");
        return 1;
    }

    int pid = ufork();
    if (pid < 0) {
        uprintf("[pipetest] fork failed\n");
        return 2;
    }

    if (pid == 0) {                         /* ---- child ---- */
        uclose(p2c[1]);                     /* not writing to parent */
        uclose(c2p[0]);                     /* not reading own ack */

        unsigned char buf[1024];
        int n = 0, r;
        while (n < 1024 && (r = uread(p2c[0], buf + n, 1024 - n)) > 0)
            n += r;
        if (n != 1024) {
            uprintf("[pipetest] child read %d bytes, want 1024\n", n);
            uexit(10);
        }
        for (int i = 0; i < 1024; i++) {
            if (buf[i] != (unsigned char)i) {
                uprintf("[pipetest] data mismatch at byte %d\n", i);
                uexit(11);
            }
        }

        /* parent closed its write end: the next read must report EOF */
        r = uread(p2c[0], buf, 1);
        if (r != 0) {
            uprintf("[pipetest] expected EOF, got %d\n", r);
            uexit(12);
        }

        uwrite_fd(c2p[1], "ok", 2);
        uexit(33);
    }

    /* ---- parent ---- */
    uclose(p2c[0]);                         /* not reading what we send */
    uclose(c2p[1]);                         /* not acking ourselves */

    unsigned char out[1024];
    for (int i = 0; i < 1024; i++)
        out[i] = (unsigned char)i;

    int done = 0, r;
    while (done < 1024 && (r = uwrite_fd(p2c[1], out + done, 1024 - done)) > 0)
        done += r;
    if (done != 1024) {
        uprintf("[pipetest] parent wrote %d bytes, want 1024\n", done);
        return 3;
    }
    uclose(p2c[1]);                         /* child sees EOF after draining */

    char ack[2] = {0, 0};
    int got = 0;
    while (got < 2 && (r = uread(c2p[0], ack + got, 2 - got)) > 0)
        got += r;
    if (got != 2 || ack[0] != 'o' || ack[1] != 'k') {
        uprintf("[pipetest] bad ack (%d bytes: %c%c)\n", got, ack[0], ack[1]);
        return 4;
    }

    int st = uwaitpid(pid);
    if (st != 33) {
        uprintf("[pipetest] child status %d, want 33\n", st);
        return 5;
    }

    uprintf("[pipetest] 1KiB verified both ways + EOF, PIPETEST-OK\n");
    return 66;
}
