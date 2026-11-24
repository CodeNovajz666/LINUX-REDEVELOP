#include "ulib.h"
#include "etc.h"

/* osh: the user-space scripting shell (v1.0.0). Replaces the v0.9.0 sh and
 * the kernel debug shell's user-facing role: pipelines, redirection,
 * background jobs, shell variables, $?, comments -- everything the ROADMAP
 * scoped for this milestone, built on the 0.2-0.9 syscall base.
 *
 * Explicit teaching scope (v1.0.0 honesty):
 *   - no quoting/escaping: words split on blanks, operators always split
 *   - variables are shell-local; they are NOT exported to children
 *   - no Ctrl-D/EOF
 *   - globs, here-docs, conditionals are 1.x territory
 *
 * v1.2.0: job control lands -- osh leads its own process group, every job
 * (foreground or background) runs in one group of its own, and the
 * console's foreground group is osh's IOCtl(TCSETPGRP) register: Ctrl-C
 * becomes SIGINT for exactly the running foreground job.
 *
 * v1.1.1: interactive recall -- Up/Down walk the history ring through
 * ureadline_ex's editor (Left/Right/Home/End/Ctrl-U/Ctrl-W included); the
 * escape-sequence grammar itself is parsed once in the kernel input layer.
 *
 * v1.0.0: the kernel debug shell's read-only reports (ps/cpus/mem) are
 * merged in as built-ins over syscalls 39-41; the kernel shell itself
 * stays as the console=debug rescue path. */

#define OSH_LINE_MAX   128
#define OSH_EXP_MAX    256
#define OSH_TOK_MAX    32
#define OSH_STAGE_MAX  5
#define OSH_ARGV_MAX   8
#define OSH_JOB_MAX    8
#define OSH_VAR_MAX    16

struct osh_tok {
    const char *p;
    int len;
};

struct osh_redir {
    const char *in_file;                        /* < file */
    const char *out_file;                       /* > / >> file */
    int append;
};

struct osh_stage {
    /* +1: parse_stages NUL-terminates argv[argc] -- argc may legally be
     * OSH_ARGV_MAX, so the sentinel needs its own slot (writing argv[8]
     * of an 8-slot array clobbered the adjacent argc field) */
    const char *argv[OSH_ARGV_MAX + 1];
    int argc;
    struct osh_redir r;
};

struct osh_job {
    /* every pipeline stage's pid, not just the last: middle stages are
     * osh's children too and need reaping, or each background pipeline
     * leaks zombie slots until the shell exits */
    int pid[OSH_STAGE_MAX];
    int npids;
    char cmd[48];
    bool used;
    bool done;
};

struct osh_var {
    char name[16];
    char value[48];
    bool exported;                          /* v1.1.5: rides child envp */
};

static struct osh_job jobs[OSH_JOB_MAX];
static struct osh_var vars[OSH_VAR_MAX];
static int nvars;
static int last_status;                         /* $? */

/* v1.2.0 job groups: the pgid the NEXT fork's child should adopt -- 0 = "no
 * group yet, you are the leader candidate" (stage 0), otherwise "join the
 * job's existing group" (stages 1+). Parent AND child both call setpgid
 * with the same target (the classic double-call): whichever runs first
 * wins, the loser's call re-affirms, and no stage can exec outside its
 * job's group. */
static int child_pgid;

/* v1.1.0: command history -- raw (pre-expansion) lines in a true ring.
 * v1.1.1: fixed the "pseudo ring" (a full table used to overwrite only the
 * last slot forever) and wired it to the editor's Up/Down browsing: hist(i)
 * is chronological entry i; entries older than the ring holds are gone. */
#define OSH_HIST_MAX 32
static char history[OSH_HIST_MAX][OSH_LINE_MAX];
static int nhist;                     /* total lines ever entered */

static void hist_add(const char *line) {
    if (!line[0]) return;                       /* blank lines not recorded */
    int last = nhist - 1;
    if (nhist > 0 && ustrcmp(history[last % OSH_HIST_MAX], line) == 0)
        return;                                 /* consecutive repeat: skip */
    ustrncpy(history[nhist % OSH_HIST_MAX], line, OSH_LINE_MAX - 1);
    history[nhist % OSH_HIST_MAX][OSH_LINE_MAX - 1] = 0;
    nhist++;
}

/* editor callback: the line `back` entries before the newest, or NULL when
 * browsing past the oldest entry the ring still holds */
static const char *hist_back(int back, void *ctx) {
    (void)ctx;
    if (back < 1 || back > OSH_HIST_MAX || back > nhist) return 0;
    return history[(nhist - back) % OSH_HIST_MAX];
}

/* operator token? ('|' '<' '>' '&' or the two-char '>>') */
static bool tok_is_op(const struct osh_tok *t) {
    if (t->len == 2) return t->p[0] == '>' && t->p[1] == '>';
    if (t->len != 1) return false;
    return t->p[0] == '|' || t->p[0] == '<' || t->p[0] == '>' || t->p[0] == '&';
}

/* ---- variable table ---- */

static struct osh_var *var_find(const char *name, int len) {
    for (int i = 0; i < nvars; i++)
        if ((int)ustrlen(vars[i].name) == len && ustrncmp(vars[i].name, name, len) == 0)
            return &vars[i];
    return 0;
}

static void var_set(const char *name, int nlen, const char *value) {
    if (nlen <= 0 || nlen >= (int)sizeof(vars[0].name)) return;
    struct osh_var *v = var_find(name, nlen);
    if (!v) {
        if (nvars >= OSH_VAR_MAX) return;       /* table full: silently drop */
        v = &vars[nvars++];
        for (int i = 0; i < nlen; i++) v->name[i] = name[i];
        v->name[nlen] = 0;
        v->exported = false;                    /* NAME=value stays local
                                                 * until `export` says so */
    }
    ustrncpy(v->value, value, sizeof(v->value) - 1);
    v->value[sizeof(v->value) - 1] = 0;
}

/* is this word NAME=VALUE (assignable)? */
static bool word_is_assign(const char *w, int len) {
    if (len <= 1 || w[0] == '$') return false;
    if (!((w[0] >= 'A' && w[0] <= 'Z') || (w[0] >= 'a' && w[0] <= 'z') || w[0] == '_'))
        return false;
    for (int i = 1; i < len; i++) {
        char c = w[i];
        if (c == '=') return i > 0;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_'))
            return false;
    }
    return false;
}

/* ---- $VAR expansion (pre-tokenization, so values may contain blanks) -- */

static int var_value(const char *name, int len, char *out, int cap) {
    if (len == 1 && name[0] == '?') {           /* $? */
        char num[12];
        char *p = num + sizeof(num);
        unsigned u = (unsigned)last_status;
        do { *--p = '0' + u % 10; u /= 10; } while (u);
        int n = 0;
        while (p < num + sizeof(num) && n < cap) out[n++] = *p++;
        return n;
    }
    if (len == 3 && ustrncmp(name, "pid", 3) == 0) {
        char num[12];
        char *p = num + sizeof(num);
        unsigned u = (unsigned)ugetpid();
        do { *--p = '0' + u % 10; u /= 10; } while (u);
        int n = 0;
        while (p < num + sizeof(num) && n < cap) out[n++] = *p++;
        return n;
    }
    struct osh_var *v = var_find(name, len);
    if (!v) return 0;                           /* unknown name -> empty */
    int n = 0;
    while (v->value[n] && n < cap) { out[n] = v->value[n]; n++; }
    return n;
}

/* v1.0.1: strict bounds -- every write checks o < cap - 1 first, so the
 * final out[o] = 0 always lands on a free byte (the old loop could run
 * one to two bytes past the buffer on a maximally expanding line). A
 * lone '$' at end-of-line breaks instead of advancing past the NUL. */
static int expand_line(const char *in, char *out, int cap) {
    int o = 0;
    for (const char *p = in; *p; p++) {
        if (*p != '$') {
            if (o < cap - 1) out[o++] = *p;
            continue;
        }
        p++;                                    /* past '$' */
        if (*p == '{') {                        /* ${NAME} */
            const char *e = ustrchr(p + 1, '}');
            if (!e) {
                if (o < cap - 1) out[o++] = '$';
                if (o < cap - 1) out[o++] = '{';
                continue;
            }
            int n = var_value(p + 1, (int)(e - (p + 1)), out + o, cap - 1 - o);
            o += n;
            p = e;
            continue;
        }
        /* $NAME / $? / $pid: longest legal name */
        const char *s = p;
        if (*p == '?' || *p == '_') p++;
        else while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                    (*p >= '0' && *p <= '9') || *p == '_') p++;
        if (p == s) {                           /* lone '$' or '$<other>' */
            if (o < cap - 1) out[o++] = '$';
            if (*s && o < cap - 1) out[o++] = *s;
            if (!*s) break;                     /* '$' ended the line */
            continue;
        }
        o += var_value(s, (int)(p - s), out + o, cap - 1 - o);
        p--;                                    /* the for-loop re-advances */
    }
    out[o] = 0;
    return o;
}

/* ---- tokenizer: words + single-char operators ('>>' pairs) ---- */

/* words are copied here as proper C strings: downstream (exec, file
 * opens, comparisons) all read argv as NUL-terminated, and the raw line
 * must keep its delimiters for the operator scan */
static char tok_word[OSH_TOK_MAX][64];

static int tokenize(char *line, struct osh_tok *toks, int max) {
    int n = 0;
    char *p = line;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (*p == '#') break;                   /* comment: rest of line */
        if (n >= max) return -1;
        if (*p == '|' || *p == '<' || *p == '>' || *p == '&') {
            if (*p == '>' && p[1] == '>') {     /* append operator */
                toks[n].p = p; toks[n].len = 2;
                p += 2;
            } else {
                toks[n].p = p; toks[n].len = 1;
                p += 1;
            }
            n++;
            continue;
        }
        int len = 0;
        char *w = tok_word[n];
        while (*p && *p != ' ' && *p != '\t' &&
               *p != '|' && *p != '<' && *p != '>' && *p != '&' && *p != '#') {
            if (len < (int)sizeof(tok_word[0]) - 1) w[len++] = *p;
            p++;
        }
        w[len] = 0;
        toks[n].p = w;
        toks[n].len = len;
        n++;
    }
    return n;
}

/* ---- execution ---- */

static void wire_child_fds(const struct osh_stage *st, int pipe_in, int pipe_out) {
    if (pipe_in >= 0) udup2(pipe_in, 0);
    if (pipe_out >= 0) udup2(pipe_out, 1);
    if (st->r.in_file) {
        int fd = uopen(st->r.in_file, U_O_RDONLY);
        if (fd < 0) { uprintf("osh: cannot open %s\n", st->r.in_file); uexit(1); }
        udup2(fd, 0);
    }
    if (st->r.out_file) {
        int flags = U_O_WRONLY | U_O_CREAT | (st->r.append ? U_O_APPEND : U_O_TRUNC);
        int fd = uopen(st->r.out_file, flags);
        if (fd < 0) { uprintf("osh: cannot open %s\n", st->r.out_file); uexit(1); }
        udup2(fd, 1);
    }
    /* everything the child needs is now on 0/1/2; close inherited pipe/file
     * descriptors so a stage never keeps an unrelated pipe end alive */
    for (int fd = 3; fd < 32; fd++) uclose(fd);
}

/* v1.1.5: the exported-variable environment children exec with --
 * "NAME=value" rows rebuilt before every exec (a static pool: the child
 * copies nothing, the kernel reads the strings before the space switch) */
static char env_pool[OSH_VAR_MAX][72];
static const char *env_vec[OSH_VAR_MAX + 1];

static const char *const *osh_envp(void) {
    int n = 0;
    for (int i = 0; i < nvars && n < OSH_VAR_MAX; i++) {
        if (!vars[i].exported) continue;
        char *o = env_pool[n];
        char *end = o + sizeof(env_pool[0]) - 1;
        const char *p = vars[i].name;
        while (*p && o < end) *o++ = *p++;
        if (o < end) *o++ = '=';
        p = vars[i].value;
        while (*p && o < end) *o++ = *p++;
        *o = 0;
        env_vec[n] = env_pool[n];
        n++;
    }
    env_vec[n] = 0;
    return env_vec;
}

/* v1.1.5: resolve a command word through $PATH (default /bin): every
 * ':'-separated entry gets a /NAME stat until a regular file answers.
 * Words containing '/' or '.' pass through untouched (paths are exact). */
static int resolve_cmd(const char *word, char *path, int cap) {
    if (word[0] == '/' || word[0] == '.') {
        ustrncpy(path, word, cap - 1);
        path[cap - 1] = 0;
        return 0;
    }
    struct osh_var *pv = var_find("PATH", 4);
    const char *p = pv ? pv->value : "/bin";
    for (;;) {
        const char *e = p;
        while (*e && *e != ':') e++;
        int n = 0;
        while (p < e && n < cap - 2) path[n++] = *p++;
        if (n > 0 && path[n - 1] != '/' && n < cap - 2) path[n++] = '/';
        const char *w = word;
        while (*w && n < cap - 1) path[n++] = *w++;
        path[n] = 0;
        struct ustat st;
        if (ustat(path, &st) == 0 && (st.st_mode & 0xF000) == 0x8000)
            return 0;                           /* a regular file answers */
        if (!*e) break;
        p = e + 1;
        if (!*p) break;                         /* trailing ':' -- done */
    }
    return -1;
}

/* spawn one stage; returns child pid or -1 (message already printed) */
static int run_stage(const struct osh_stage *st, int pipe_in, int pipe_out) {
    char path[64];
    if (resolve_cmd(st->argv[0], path, sizeof(path)) != 0) {
        uprintf("osh: %s: not found\n", st->argv[0]);
        return -1;
    }
    const char *const *envp = osh_envp();
    int pid = ufork();
    if (pid < 0) {
        uputs("osh: fork failed\n");
        return -1;
    }
    if (pid == 0) {                             /* child: group, wire, exec */
        if (child_pgid) usetpgid(0, child_pgid);
        wire_child_fds(st, pipe_in, pipe_out);
        uexecve(path, st->argv, envp);
        uprintf("osh: %s: not found\n", st->argv[0]);
        uexit(127);
    }
    return pid;
}

static void kill_stage_pids(const int *pids, int n) {
    for (int i = 0; i < n; i++) {
        if (!pids[i]) continue;
        ukill(pids[i], U_SIGKILL);
        uwaitpid(pids[i]);
    }
}

/* strict decimal parse: every char a digit, value capped -- a long digit
 * run must not wrap into a valid pid. -1 on any malformed input. */
static int parse_pid(const char *s) {
    if (!*s) return -1;
    long v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return -1;
        v = v * 10 + (*s - '0');
        if (v > 1000000) return -1;
    }
    return (int)v;
}

static int run_pipeline(struct osh_stage *stages, int nstages, bool background) {
    int pipe_r = -1, pipe_w = -1;
    int pids[OSH_STAGE_MAX] = { 0 };

    for (int i = 0; i < nstages; i++) {
        int pin = -1, pout = -1;
        if (i > 0) pin = pipe_r;
        if (i < nstages - 1) {
            int pfds[2];
            if (upipe(pfds) != 0) {
                uputs("osh: pipe failed\n");
                kill_stage_pids(pids, i);
                return 1;
            }
            pipe_r = pfds[0];
            pipe_w = pfds[1];
            pout = pipe_w;
        }
        /* v1.2.0: every stage of the job lands in ONE process group led by
         * stage 0 -- that group is what Ctrl-C hits when the job holds the
         * console (parent side of the double-call below). */
        child_pgid = (i == 0) ? 0 : pids[0];
        pids[i] = run_stage(&stages[i], pin, pout);
        if (pin >= 0) uclose(pin);              /* parent drops stage ends */
        if (pout >= 0) uclose(pout);
        if (pids[i] < 0) {
            kill_stage_pids(pids, i);
            return 1;
        }
        usetpgid(pids[i], pids[0]);
    }

    if (background) {
        for (int j = 0; j < OSH_JOB_MAX; j++) {
            if (jobs[j].used && jobs[j].done) jobs[j].used = false;
            if (!jobs[j].used) {
                /* every stage joins the job: middle stages are osh's
                 * children and would zombie forever otherwise */
                jobs[j].npids = 0;
                for (int i = 0; i < nstages; i++)
                    jobs[j].pid[jobs[j].npids++] = pids[i];
                jobs[j].used = true;
                jobs[j].done = false;
                char *o = jobs[j].cmd;
                for (int a = 0; a < stages[0].argc &&
                                o < jobs[j].cmd + (int)sizeof(jobs[j].cmd) - 1; a++) {
                    if (a) *o++ = ' ';
                    const char *w = stages[0].argv[a];
                    while (*w && o < jobs[j].cmd + (int)sizeof(jobs[j].cmd) - 1)
                        *o++ = *w++;
                }
                *o = 0;
                uprintf("[%d] pid %d\n", j + 1, jobs[j].pid[jobs[j].npids - 1]);
                return 0;
            }
        }
        uputs("osh: too many background jobs\n");
        kill_stage_pids(pids, nstages);
        return 1;
    }

    /* foreground: the job's group owns the console while it runs -- Ctrl-C
     * becomes SIGINT for exactly that group (job dies, background jobs and
     * osh itself are untouched). Stages fork-inherit osh's SIGINT handler,
     * but every stage execs and exec resets caught signals to the default,
     * so SIGINT kills them (status 130) unless the program catches it. */
    if (pids[0] > 0) utcsetpgrp(pids[0]);
    /* every stage is reaped; the last stage's status becomes $? */
    for (int i = 0; i < nstages; i++)
        last_status = uwaitpid(pids[i]);
    utcsetpgrp(ugetpgrp());     /* prompt again: osh's own group holds it */
    return last_status;
}

/* ---- builtins (single stage, no redirection) ---- */

static bool stage_is_builtin(const struct osh_stage *st) {
    if (!st->argc || st->r.in_file || st->r.out_file) return false;
    static const char *names[] =
        { "cd", "exit", "logout", "help", "history", "poweroff", "wait",
          "jobs", "ps", "cpus", "mem", "kill", "upt", "uptime", "export", 0 };
    for (int i = 0; names[i]; i++)
        if (ustrcmp(st->argv[0], names[i]) == 0) return true;
    return false;
}

static void reap_finished_jobs(void) {
    /* poll every stage pid of every live job. WNOHANG's 0 is ambiguous
     * ("alive" vs "reaped, exited 0"), so an exit-0 stage resolves one
     * poll later: the next nohang returns -1 (no such child) and the
     * job flips to done. Blocking `wait` is exact; this is best effort. */
    for (int j = 0; j < OSH_JOB_MAX; j++) {
        if (!jobs[j].used || jobs[j].done) continue;
        bool all_gone = true;
        for (int i = 0; i < jobs[j].npids; i++) {
            if (jobs[j].pid[i] <= 0) continue;
            int r = uwaitpid_nohang(jobs[j].pid[i]);
            if (r != 0) jobs[j].pid[i] = 0;     /* reaped (r > 0) or gone */
            else all_gone = false;              /* still (or just) alive */
        }
        jobs[j].done = all_gone;
    }
}

static void kill_all_jobs(void) {
    for (int j = 0; j < OSH_JOB_MAX; j++) {
        if (!jobs[j].used) continue;
        for (int i = 0; i < jobs[j].npids; i++) {
            if (jobs[j].pid[i] <= 0) continue;
            ukill(jobs[j].pid[i], U_SIGKILL);
            uwaitpid(jobs[j].pid[i]);           /* reap so no zombie leaks */
        }
        jobs[j].used = false;
    }
}

static int run_builtin(struct osh_stage *st) {
    const char *cmd = st->argv[0];
    if (ustrcmp(cmd, "exit") == 0 || ustrcmp(cmd, "logout") == 0) {
        kill_all_jobs();
        int code = last_status;
        if (st->argc > 1) {                     /* exit N */
            code = 0;
            for (const char *p = st->argv[1]; *p >= '0' && *p <= '9'; p++)
                code = code * 10 + (*p - '0');
        }
        uexit(code);
    }
    if (ustrcmp(cmd, "cd") == 0) {
        const char *dir = st->argc > 1 ? st->argv[1] : "/";
        if (uchdir(dir) != 0)
            uprintf("cd: %s: no such directory\n", dir);
        last_status = 0;
        return 0;
    }
    /* v1.1.5: export NAME / export NAME=value -- marks a variable for the
     * child environment (plain NAME=value stays shell-local, bash-style) */
    if (ustrcmp(cmd, "export") == 0) {
        if (st->argc < 2) {                     /* bare: list the exported set */
            for (int i = 0; i < nvars; i++)
                if (vars[i].exported)
                    uprintf("export %s=%s\n", vars[i].name, vars[i].value);
            last_status = 0;
            return 0;
        }
        for (int a = 1; a < st->argc; a++) {
            const char *w = st->argv[a];
            const char *eq = ustrchr(w, '=');
            if (eq) {
                var_set(w, (int)(eq - w), eq + 1);
                struct osh_var *v = var_find(w, (int)(eq - w));
                if (v) v->exported = true;
            } else {
                struct osh_var *v = var_find(w, (int)ustrlen(w));
                if (v) v->exported = true;
                else { uprintf("export: %s: not set\n", w); last_status = 1; }
            }
        }
        last_status = last_status ? 1 : 0;
        return 0;
    }
    if (ustrcmp(cmd, "help") == 0) {
        uputs("osh built-ins: cd DIR, exit, logout, help, history, poweroff,\n"
              "               wait, jobs, ps, cpus, mem [-v], upt, kill PID [SIGNO],\n"
              "               export NAME[=value] (children see it; $PATH lookup)\n"
              "scripting: a|b, a>b a>>b a<b, a&, $VAR ${VAR} $? $pid,\n"
              "           NAME=value assignments, # comments\n"
              "jobs: Ctrl-C stops the foreground job (background jobs and the\n"
              "      prompt itself survive); each job runs in its own group\n"
              "editing: Up/Down history, Left/Right/Home/End cursor,\n"
              "         Backspace, Ctrl-U kill line, Ctrl-W kill word\n"
              "/bin tools: ls pwd cat cp mv rm mkdir rmdir touch echo grep env wc head tail\n"
              "            date sleep uname clear id whoami who passwd useradd ...\n"
              "            thread_hello (threads across cpus) ...\n"
              "limits: no quoting, unexported vars stay local, built-ins are\n"
              "        single-stage (no pipe/redirect)\n");
        last_status = 0;
        return 0;
    }
    if (ustrcmp(cmd, "history") == 0) {
        /* the ring holds the newest OSH_HIST_MAX entries; numbering stays
         * chronological across wraparound (entry k ever-entered = k+1) */
        int first = nhist > OSH_HIST_MAX ? nhist - OSH_HIST_MAX : 0;
        for (int i = first; i < nhist; i++)
            uprintf("  %d  %s\n", i + 1, history[i % OSH_HIST_MAX]);
        last_status = 0;
        return 0;
    }
    if (ustrcmp(cmd, "poweroff") == 0) {
        kill_all_jobs();
        upoweroff();
    }
    if (ustrcmp(cmd, "wait") == 0) {
        for (int j = 0; j < OSH_JOB_MAX; j++) {
            if (!jobs[j].used) continue;
            for (int i = 0; i < jobs[j].npids; i++)
                if (jobs[j].pid[i] > 0) uwaitpid(jobs[j].pid[i]);
            jobs[j].used = false;
        }
        last_status = 0;
        return 0;
    }
    if (ustrcmp(cmd, "jobs") == 0) {
        reap_finished_jobs();
        for (int j = 0; j < OSH_JOB_MAX; j++) {
            if (!jobs[j].used) continue;
            uprintf("[%d] %s pid %d %s\n", j + 1, jobs[j].cmd,
                    jobs[j].npids > 0 ? jobs[j].pid[jobs[j].npids - 1] : 0,
                    jobs[j].done ? "done" : "running");
        }
        last_status = 0;
        return 0;
    }
    /* ---- v1.0.0: debug reports (kernel syscalls 39-41, read-only).
     * Same tables the kernel debug shell prints, streamed into osh's
     * buffers; buffers are static so the stacks stay small. */
    if (ustrcmp(cmd, "ps") == 0) {
        static char ps_buf[8192];               /* 64 tasks x ~80 bytes fit */
        int n = ups(ps_buf, (int)sizeof(ps_buf) - 1);
        if (n < 0) { uputs("ps: report failed\n"); last_status = 1; }
        else { ps_buf[n] = 0; uputs(ps_buf); last_status = 0; }
        return 0;
    }
    if (ustrcmp(cmd, "cpus") == 0) {
        static char cpus_buf[1024];
        int n = ucpus(cpus_buf, (int)sizeof(cpus_buf) - 1);
        if (n < 0) { uputs("cpus: report failed\n"); last_status = 1; }
        else { cpus_buf[n] = 0; uputs(cpus_buf); last_status = 0; }
        return 0;
    }
    if (ustrcmp(cmd, "mem") == 0) {
        static char mem_buf[1024];
        int v = st->argc > 1 && ustrcmp(st->argv[1], "-v") == 0;
        int n = umeminfo(mem_buf, (int)sizeof(mem_buf) - 1, v);
        if (n < 0) { uputs("mem: report failed\n"); last_status = 1; }
        else { mem_buf[n] = 0; uputs(mem_buf); last_status = 0; }
        return 0;
    }
    if (ustrcmp(cmd, "upt") == 0 || ustrcmp(cmd, "uptime") == 0) {
        uprintf("uptime: %u ms\n", (unsigned)uuptime());
        last_status = 0;
        return 0;
    }
    if (ustrcmp(cmd, "kill") == 0) {
        if (st->argc < 2 || st->argc > 3) {
            uputs("usage: kill PID [SIGNO]\n");
            last_status = 2;
            return 0;
        }
        /* v1.0.1: strict numeric parse -- the old loop parsed any prefix
         * and defaulted to pid 0 (the idle task) on garbage input */
        int pid = parse_pid(st->argv[1]);
        if (pid <= 0) {
            uprintf("kill: bad pid '%s'\n", st->argv[1]);
            last_status = 2;
            return 0;
        }
        int sig = U_SIGTERM;                    /* v0.5.0: optional signal */
        if (st->argc > 2) {
            sig = parse_pid(st->argv[2]);
            if (sig < 1 || sig > 31) {
                uprintf("kill: bad signal %d\n", sig);
                last_status = 1;
                return 0;
            }
        }
        if (ukill(pid, sig) != 0) {
            uprintf("kill: no such task (pid %d)\n", pid);
            last_status = 1;
            return 0;
        }
        uprintf("killed pid %d with sig%d\n", pid, sig);
        last_status = 0;
        return 0;
    }
    return -1;
}

/* ---- prompt and main loop ---- */

/* v1.2.0: Ctrl-C at the prompt is aimed at osh's own group -- catch it and
 * carry on (the real shells' behavior; the byte itself is consumed by the
 * kernel's input layer and never reaches the line editor). A Ctrl-C while
 * a line is half-typed kills the line: the handler latches the flag and
 * the main loop throws the buffer away, like bash does. */
static volatile int got_sigint;
static void on_sigint(int sig) { (void)sig; got_sigint = 1; }

static void prompt(void) {
    char cwd[80];
    char name[24];
    ustrcpy(name, "?");
    struct pwent pw;
    if (etc_getpwuid(ugetuid(), &pw) == 0)
        ustrncpy(name, pw.name, sizeof(name) - 1);
    if (ugetcwd(cwd, sizeof(cwd)) < 0)
        ustrcpy(cwd, "/");
    /* ANSI SGR: bold green user, bold blue cwd, white "novix"/sigil.
     * The host terminal colours the serial stream itself; the VGA console
     * applies the same sequences in kernel/console.c. */
    uprintf("\033[1;32m%s\033[0m@novix:\033[1;34m%s\033[0m%s ",
            name, cwd, ugetuid() == 0 ? "#" : "$");
}

/* one parsed line -> stages; returns nstages or 0 (nothing to run) */
static int parse_stages(struct osh_tok *toks, int ntok,
                        struct osh_stage *stages) {
    int nstages = 0;
    struct osh_stage *st = &stages[0];
    st->argc = 0;
    st->r.in_file = 0;
    st->r.out_file = 0;
    st->r.append = 0;

    for (int i = 0; i < ntok; i++) {
        struct osh_tok *t = &toks[i];
        if (t->len == 1 && t->p[0] == '|') {
            if (st->argc == 0) return -1;       /* empty stage */
            if (++nstages >= OSH_STAGE_MAX) return -1;
            st = &stages[nstages];
            st->argc = 0;
            st->r.in_file = 0;
            st->r.out_file = 0;
            st->r.append = 0;
            continue;
        }
        if (t->len == 1 && t->p[0] == '<' && !st->r.in_file) {
            if (i + 1 >= ntok || tok_is_op(&toks[i + 1])) return -1;
            st->r.in_file = toks[++i].p;
            continue;
        }
        /* '>' or '>>' -- tok_is_op tells operators from two-letter words */
        if (tok_is_op(t) && t->p[0] == '>') {
            if (st->r.out_file) return -1;
            if (i + 1 >= ntok || tok_is_op(&toks[i + 1])) return -1;
            st->r.out_file = toks[++i].p;
            st->r.append = (t->len == 2);
            continue;
        }
        if (t->len == 1 && t->p[0] == '&') {
            if (i != ntok - 1) return -1;       /* '&' only ends the line */
            continue;                           /* caller checks last token */
        }
        if (st->argc >= OSH_ARGV_MAX) return -1;
        st->argv[st->argc++] = t->p;
    }
    if (stages[0].argc == 0 && nstages == 0) return 0;
    if (st->argc == 0) return -1;
    for (int s = 0; s <= nstages; s++)
        stages[s].argv[stages[s].argc] = 0;
    return nstages + 1;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    char line[OSH_LINE_MAX];
    char exp[OSH_EXP_MAX];
    struct osh_tok toks[OSH_TOK_MAX];
    struct osh_stage stages[OSH_STAGE_MAX];
    last_status = 0;

    /* v1.2.0: the job-control shell bootstrap -- osh leads its own process
     * group, catches the Ctrl-C meant for it at the prompt, and starts out
     * owning the console's foreground */
    usetpgid(0, 0);
    usignal(U_SIGINT, on_sigint);
    utcsetpgrp(ugetpgrp());

    for (;;) {
        reap_finished_jobs();
        prompt();
        got_sigint = 0;
        ureadline_ex(line, sizeof(line), hist_back, 0);      /* v1.1.1: ↑/↓ */
        if (got_sigint) {                       /* Ctrl-C: the line is dead */
            uputs("\n");
            continue;
        }
        hist_add(line);                         /* v1.1.0: raw line, pre-expansion */
        expand_line(line, exp, sizeof(exp));

        bool background = false;
        int ntok = tokenize(exp, toks, OSH_TOK_MAX);
        if (ntok < 0) { uputs("osh: too many tokens\n"); last_status = 2; continue; }
        if (ntok > 0 && toks[ntok - 1].len == 1 && toks[ntok - 1].p[0] == '&') {
            background = true;
            ntok--;                             /* '&' is not a stage token */
        }
        int nstages = parse_stages(toks, ntok, stages);
        if (nstages < 0) { uputs("osh: syntax error\n"); last_status = 2; continue; }
        if (nstages == 0) continue;             /* blank / comment line */

        /* assignments: NAME=value at the head of a single command */
        int a = 0;
        while (a < nstages && stages[a].argc == 1 &&
               word_is_assign(stages[a].argv[0], (int)ustrlen(stages[a].argv[0]))) {
            const char *w = stages[a].argv[0];
            const char *eq = ustrchr(w, '=');
            var_set(w, (int)(eq - w), eq + 1);
            a++;
        }
        if (a == nstages) { last_status = 0; continue; }
        if (a > 0) {                            /* shift assignments away */
            for (int s = a; s < nstages; s++) stages[s - a] = stages[s];
            nstages -= a;
        }

        if (nstages == 1 && stage_is_builtin(&stages[0])) {
            run_builtin(&stages[0]);
            continue;
        }
        run_pipeline(stages, nstages, background);
    }
}
