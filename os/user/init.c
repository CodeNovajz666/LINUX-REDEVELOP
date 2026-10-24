#include "ulib.h"
#include "etc.h"

/* init: the user-space pid 1 (v1.0.0). Reads /etc/init and runs it:
 *   - plain lines are one-shot: spawned and waited on, in file order
 *   - a line starting with "respawn" is the supervised service: when it
 *     exits it is spawned again (the SysV rule that makes logout return
 *     to a fresh getty). The first respawn line wins; extras are ignored.
 *   - '#' lines and blanks are comments
 * With no /etc/init the boot falls back to the v0.9.0 contract: read
 * /etc/inittab ("tty1:getty"), supervise that program.
 * Words split on blanks; no quoting, no expansion at this level. */

#define INIT_FILE_MAX  512
#define INIT_ARGV_MAX  8

struct init_entry {
    /* +1: parse_line may legally fill INIT_ARGV_MAX words and then
     * NUL-terminates argv[argc] -- the sentinel needs its own slot
     * (argv[8] of an 8-slot array would clobber the adjacent argc) */
    const char *argv[INIT_ARGV_MAX + 1];
    int argc;
    bool respawn;
};

static int spawn_entry(const struct init_entry *e) {
    int pid = uspawn_argv(e->argv[0], e->argv, e->argc);
    if (pid < 0)
        uprintf("init: cannot run %s\n", e->argv[0]);
    return pid;
}

/* parse "respawn /bin/getty"-style words from one line (in place) */
static bool parse_line(char *line, struct init_entry *e) {
    while (*line == ' ' || *line == '\t') line++;
    if (!*line || *line == '#') return false;
    e->respawn = false;
    e->argc = 0;
    if (ustrncmp(line, "respawn", 7) == 0 &&
        (line[7] == ' ' || line[7] == '\t')) {
        e->respawn = true;
        line += 7;
    }
    char *p = line;
    while (*p && e->argc < INIT_ARGV_MAX) {
        while (*p == ' ' || *p == '\t') *p++ = 0;
        if (!*p) break;
        e->argv[e->argc++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
    }
    e->argv[e->argc] = 0;
    return e->argc > 0;
}

/* v0.9.0 fallback: /etc/inittab's "tty:prog" -> supervise /bin/prog */
static bool inittab_service(char *prog, int cap) {
    static char buf[256];
    if (etc_read_file("/etc/inittab", buf, sizeof(buf)) < 0) return false;
    for (char *line = buf; *line; ) {
        char *nl = ustrchr(line, '\n');
        if (nl) *nl = 0;
        if (line[0] && line[0] != '#') {
            char *colon = ustrchr(line, ':');
            if (colon && colon[1]) {
                if (colon[1] == '/') {
                    ustrncpy(prog, colon + 1, cap - 1);
                    prog[cap - 1] = 0;
                } else {
                    ustrcpy(prog, "/bin/");
                    ustrncpy(prog + 5, colon + 1, cap - 6);
                    prog[cap - 1] = 0;
                }
                return true;
            }
        }
        if (!nl) break;
        line = nl + 1;
    }
    return false;
}

static void supervise(const struct init_entry *e) {
    for (;;) {
        int pid = spawn_entry(e);
        if (pid < 0) {
            usleep(1000);
            continue;
        }
        uwaitpid(pid);
        while (uwaitpid_nohang(-1) > 0) { }     /* reap orphans, best effort */
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    static char buf[INIT_FILE_MAX];
    struct init_entry oneshots[8];
    struct init_entry service;
    bool have_service = false;
    int noneshots = 0;

    if (etc_read_file("/etc/init", buf, sizeof(buf)) >= 0) {
        for (char *line = buf; *line; ) {
            char *nl = ustrchr(line, '\n');
            if (nl) *nl = 0;
            struct init_entry e;
            if (parse_line(line, &e)) {
                if (e.respawn && !have_service) {
                    service = e;
                    have_service = true;
                } else if (!e.respawn && noneshots < 8) {
                    oneshots[noneshots++] = e;
                }
                /* extras: extra respawn lines and >8 one-shots are ignored */
            }
            if (!nl) break;
            line = nl + 1;
        }
    }

    for (int i = 0; i < noneshots; i++) {
        int pid = spawn_entry(&oneshots[i]);
        if (pid >= 0) uwaitpid(pid);
    }

    if (have_service) {
        supervise(&service);
    } else {
        /* no respawn line: fall back to the inittab contract, else getty */
        char prog[64];
        if (inittab_service(prog, sizeof(prog))) {
            struct init_entry e;
            e.argv[0] = prog;
            e.argc = 1;
            e.respawn = true;
            supervise(&e);
        } else {
            struct init_entry e;
            e.argv[0] = "/bin/getty";
            e.argc = 1;
            e.respawn = true;
            supervise(&e);
        }
    }
    return 0;
}
