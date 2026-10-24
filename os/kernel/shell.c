#include "novix.h"
#include "shell.h"
#include "console.h"
#include "keyboard.h"
#include "task.h"
#include "sched.h"
#include "smp.h"
#include "timer.h"
#include "pmm.h"
#include "heap.h"
#include "vfs.h"
#include "signal.h"
#include "mm.h"
#include "rtc.h"
#include "lib.h"

/* v0.5.0: the most recent spawned task is the "foreground" one -- Ctrl+C
 * (0x03) from the keyboard sends it SIGINT, like a single-job tty line */
static int fg_pid = -1;

static void cmd_help(void) {
    kprintf("commands:\n");
    kprintf("  help              this text\n");
    kprintf("  ps                task table\n");
    kprintf("  cpus              per-CPU status (v0.7.0 SMP)\n");
    kprintf("  spawn NAME        run /bin/NAME as a user process\n");
    kprintf("                    (hello / spawner / filetest / malloctest /\n");
    kprintf("                     sigtest / masktest / pipetest / memhog /\n");
    kprintf("                     forker / fault_*)  Ctrl+C sends SIGINT to it\n");
    kprintf("  kill PID [SIGNO]  terminate a task (default 15=SIGTERM)\n");
    kprintf("  policy N          0=PRIORITY 1=RR 2=FCFS 3=EDF 4=RMS\n");
    kprintf("  uptime            ms since boot\n");
    kprintf("  date              hardware wall clock (v0.8.0 RTC)\n");
    kprintf("  mem [-v]           memory usage (-v: paging/swap counters)\n");
    kprintf("  ls [PATH]         list directory (default /)\n");
    kprintf("  cat FILE          print file contents\n");
    kprintf("  mkdir PATH        create directory\n");
    kprintf("  touch FILE        create empty file\n");
    kprintf("  rm FILE           remove file\n");
    kprintf("  echo TEXT         print TEXT; 'echo T > F' writes, '>>' appends\n");
    kprintf("  clear             clear screen\n");
    kprintf("  reboot            reset machine\n");
    kprintf("  poweroff          exit QEMU\n");
    kprintf("line editing: Backspace, Ctrl+C, Tab complete, Up/Down history\n");
}

static void cmd_spawn(const char *name) {
    if (!name || !name[0]) { kprintf("usage: spawn NAME\n"); return; }
    char path[80];
    if (name[0] == '/') {
        strncpy(path, name, sizeof(path) - 1);
        path[sizeof(path) - 1] = 0;
    } else {
        strcpy(path, "/bin/");
        strncpy(path + 5, name, sizeof(path) - 6);
        path[sizeof(path) - 1] = 0;
    }
    struct task *t = task_spawn_path(path);
    if (!t) {
        kprintf("spawn: '%s' not found (try: hello, spawner, filetest, fault_div, fault_segv)\n", name);
        return;
    }
    fg_pid = t->pid;
    kprintf("spawned pid %d (%s)\n", t->pid, t->name);
}

/* v1.0.0: the streaming sink for the kernel debug shell -- reports print
 * straight to the console. sys_ps/sys_cpus/sys_meminfo use their own
 * user-buffer sink with the exact same report builders. */
void shell_emit_console(const char *s, int len, void *ctx) {
    (void)ctx;
    console_write(s, len);
}

/* v1.0.0: the `mem` report shared by the kernel shell and sys_meminfo
 * (all counters are globals, so the builder lives here beside the shell) */
void shell_report_mem(shell_emit_t emit, void *ctx, int verbose) {
    char row[128];
    int n = ksnprintf(row, sizeof(row),
            "physical: %u/%u pages free (%u MiB total managed)\n",
            pmm_free_pages(), pmm_total_pages(),
            pmm_total_pages() * PAGE_SIZE >> 20);
    emit(row, n, ctx);
    n = ksnprintf(row, sizeof(row),
            "kernel heap: %u used, %u free of %u KiB\n",
            (unsigned)(heap_used_bytes() >> 10),
            (unsigned)(heap_free_bytes() >> 10),
            (unsigned)((HEAP_END - HEAP_START) >> 10));
    emit(row, n, ctx);
    if (verbose) {                              /* v0.6.0: mm accounting */
        n = ksnprintf(row, sizeof(row),
                "mm: file-in %u anon %u swap-in %u cow %u\n",
                g_mmstat.fault_file, g_mmstat.fault_anon,
                g_mmstat.fault_swapin, g_mmstat.fault_cow);
        emit(row, n, ctx);
        n = ksnprintf(row, sizeof(row),
                "    swap-out %u file-drop %u, %u/%u swap slots free\n",
                g_mmstat.swap_out, g_mmstat.file_drop,
                swap_free_slots(), swap_total_slots());
        emit(row, n, ctx);
    }
}

static void cmd_mem(const char *arg) {
    shell_report_mem(shell_emit_console, 0, arg[0] == '-' && arg[1] == 'v');
}

static void cmd_reboot(void) {
    irq_flags_t f = irq_save();
    for (int i = 0; i < 100; i++) { outb(0x64, 0xFE); io_wait(); }
    irq_restore(f);
    kprintf("reboot failed\n");
}

/* v0.8.0: the hardware wall clock, verbatim (QEMU runs -rtc base=localtime
 * so the CMOS holds local time; no timezone machinery, honestly).
 * v1.1.0: extracted as the shared builder behind cmd_date and sys_datetime. */
int shell_report_date(shell_emit_t emit, void *ctx) {
    struct rtc_time t;
    if (rtc_read(&t) != 0) return -1;
    char row[96];
    int n = ksnprintf(row, sizeof(row),
            "%04u-%02u-%02u %02u:%02u:%02u local (unix %llu, up %u ms)\n",
            t.year, t.mon, t.day, t.hour, t.min, t.sec,
            (unsigned long long)rtc_unix_seconds(),
            (unsigned)timer_uptime_ms());
    emit(row, n, ctx);
    return 0;
}

static void cmd_date(void) {
    if (shell_report_date(shell_emit_console, NULL) != 0)
        kprintf("date: RTC not sane\n");
}

static void cmd_kill(const char *arg) {
    int pid = 0;
    const char *p = arg;
    for (; *p >= '0' && *p <= '9'; p++)
        pid = pid * 10 + (*p - '0');
    if (!arg[0]) { kprintf("usage: kill PID [SIGNO]\n"); return; }
    int sig = SIGTERM;                       /* v0.5.0: optional signal arg */
    if (*p == ' ') {
        sig = 0;
        for (const char *q = p + 1; *q >= '0' && *q <= '9'; q++)
            sig = sig * 10 + (*q - '0');
        if (sig < 1 || sig > 31) { kprintf("kill: bad signal %d\n", sig); return; }
    }
    if (pid == 0) { kprintf("kill: cannot kill the idle task\n"); return; }
    struct task *t = task_find(pid);
    if (!t) { kprintf("kill: no task with pid %d\n", pid); return; }
    if (t->is_idle) { kprintf("kill: cannot kill the idle task\n"); return; }
    if (pid == current_task()->pid) { kprintf("kill: cannot kill the shell\n"); return; }
    task_kill_task(t, sig);
    kprintf("killed pid %d (%s) with sig%d\n", pid, t->name, sig);
}

/* ---- filesystem commands ---- */

/* v1.1.0: the `ls` listing as a streaming report -- the same bytes go to
 * the kernel debug shell's console and to the /bin/ls program's buffer via
 * sys_getdents (which also checks the caller's read permission on the
 * directory; the kernel shell runs as root and does not). */
void shell_report_dir(shell_emit_t emit, void *ctx, const char *path) {
    if (!path[0]) path = "/";
    struct stat st;
    if (vfs_stat(path, &st) != 0) {
        char row[96];
        int n = ksnprintf(row, sizeof(row),
                          "ls: %s: no such file or directory\n", path);
        emit(row, n, ctx);
        return;
    }
    if ((st.st_mode & S_IFMT) != S_IFDIR) {
        char row[96];
        int n = ksnprintf(row, sizeof(row), "%s  %u bytes\n",
                          path, st.st_size);
        emit(row, n, ctx);
        return;
    }

    char full[96];
    uint32_t dlen = strlen(path);
    if (dlen >= sizeof(full) - 34) {
        emit("ls: path too long\n", 18, ctx);
        return;
    }
    memcpy(full, path, dlen);
    if (dlen && full[dlen - 1] != '/') full[dlen++] = '/';

    char name[32];
    char row[64];
    int n = 0;
    for (int i = 0; vfs_readdir(path, i, name, sizeof(name)) == 0; i++) {
        strcpy(full + dlen, name);
        struct stat es;
        int w;
        if (vfs_stat(full, &es) != 0)
            w = ksnprintf(row, sizeof(row), "  %-24s ?\n", name);
        else if ((es.st_mode & S_IFMT) == S_IFDIR)
            w = ksnprintf(row, sizeof(row), "  %-24s <dir>\n", name);
        else if ((es.st_mode & S_IFMT) == S_IFCHR)
            w = ksnprintf(row, sizeof(row), "  %-24s <chr>\n", name);
        else if ((es.st_mode & S_IFMT) == S_IFBLK)
            w = ksnprintf(row, sizeof(row), "  %-24s <blk>\n", name);
        else
            w = ksnprintf(row, sizeof(row), "  %-24s %u bytes\n",
                          name, es.st_size);
        emit(row, w, ctx);
        n++;
    }
    int w = ksnprintf(row, sizeof(row), "%d entr%s\n", n, n == 1 ? "y" : "ies");
    emit(row, w, ctx);
}

static void cmd_ls(const char *path) {
    shell_report_dir(shell_emit_console, NULL, path);
}

static void cmd_cat(const char *path) {
    if (!path[0]) { kprintf("usage: cat FILE\n"); return; }
    struct file *f = vfs_open(path, O_RDONLY);
    if (!f) { kprintf("cat: %s: no such file\n", path); return; }
    char buf[64];
    int n;
    while ((n = vfs_read(f, buf, sizeof(buf))) > 0)
        for (int i = 0; i < n; i++) console_putchar(buf[i]);
    vfs_close(f);
}

static void cmd_mkdir(const char *path) {
    if (!path[0]) { kprintf("usage: mkdir PATH\n"); return; }
    if (vfs_mkdir(path) != 0)
        kprintf("mkdir: %s: failed (exists or parent missing)\n", path);
}

static void cmd_touch(const char *path) {
    if (!path[0]) { kprintf("usage: touch FILE\n"); return; }
    struct file *f = vfs_open(path, O_WRONLY | O_CREAT);
    if (!f) { kprintf("touch: %s: cannot create\n", path); return; }
    vfs_close(f);
}

static void cmd_rm(const char *path) {
    if (!path[0]) { kprintf("usage: rm FILE\n"); return; }
    if (vfs_unlink(path) != 0)
        kprintf("rm: %s: failed (missing or is a directory)\n", path);
}

/* echo TEXT, echo TEXT > FILE (overwrite), echo TEXT >> FILE (append) */
static void cmd_echo(char *arg) {
    char *redir = strstr(arg, " >> ");
    int append = redir != NULL;
    if (!redir) redir = strstr(arg, " > ");
    if (!redir) { kprintf("%s\n", arg); return; }

    *redir = 0;
    char *file = redir + (append ? 4 : 3);
    while (*file == ' ') file++;
    if (!file[0]) { kprintf("usage: echo TEXT > FILE\n"); return; }

    struct file *f = vfs_open(file, O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC));
    if (!f) { kprintf("echo: %s: cannot open\n", file); return; }
    vfs_write(f, arg, strlen(arg));
    vfs_write(f, "\n", 1);
    vfs_close(f);
}

static void shell_parse(char *line) {
    while (*line == ' ') line++;
    char *tail = line + strlen(line);      /* trim trailing blanks: Tab
                                              completes with 'name ' */
    while (tail > line && tail[-1] == ' ') tail--;
    *tail = 0;
    char *arg = line;
    while (*arg && *arg != ' ') arg++;
    if (*arg) { *arg = 0; arg++; }
    while (*arg == ' ') arg++;

    if (!strcmp(line, "help")) cmd_help();
    else if (!strcmp(line, "ps") || !strcmp(line, "tasks")) task_dump_table();
    else if (!strcmp(line, "cpus")) smp_dump_cpus();
    else if (!strcmp(line, "spawn")) cmd_spawn(arg);
    else if (!strcmp(line, "kill")) cmd_kill(arg);
    else if (!strcmp(line, "ls") || !strcmp(line, "dir")) cmd_ls(arg);
    else if (!strcmp(line, "cat")) cmd_cat(arg);
    else if (!strcmp(line, "mkdir")) cmd_mkdir(arg);
    else if (!strcmp(line, "touch")) cmd_touch(arg);
    else if (!strcmp(line, "rm")) cmd_rm(arg);
    else if (!strcmp(line, "echo")) cmd_echo(arg);
    else if (!strcmp(line, "policy")) {
        int p = -1;
        if (arg[0] >= '0' && arg[0] <= '9') p = arg[0] - '0';
        if (p >= 0 && p < SCHED_POLICY_COUNT) {
            sched_set_policy((sched_policy_t)p);
            kprintf("policy -> %s\n", sched_policy_name((sched_policy_t)p));
        } else {
            kprintf("usage: policy N (0=PRIORITY 1=RR 2=FCFS 3=EDF 4=RMS), now: %s\n",
                    sched_policy_name(sched_policy()));
        }
    }
    else if (!strcmp(line, "uptime")) kprintf("uptime: %u ms (%u ticks)\n",
                                              (unsigned)timer_uptime_ms(), (unsigned)timer_ticks());
    else if (!strcmp(line, "date")) cmd_date();
    else if (!strcmp(line, "mem")) cmd_mem(arg);
    else if (!strcmp(line, "clear")) console_clear();
    else if (!strcmp(line, "reboot")) cmd_reboot();
    else if (!strcmp(line, "poweroff") || !strcmp(line, "exit")) {
        kprintf("bye.\n");
        qemu_exit(0);
    }
    else if (line[0]) kprintf("unknown command '%s' (try help)\n", line);
}

/* ---- line editing: history + tab completion ----
 * The edited line lives on one screen row (prompt + <=63 chars), so
 * "\b \b" erases and reprints are always column-safe. */

#define HIST_MAX 16
static char hist[HIST_MAX][64];
static int hist_count;      /* commands entered (monotonic) */
static int hist_pos;        /* browsing cursor; == hist_count while typing */

static const char *const cmd_names[] = {
    "help", "ps", "tasks", "spawn", "kill", "ls", "dir", "cat",
    "mkdir", "touch", "rm", "echo", "policy", "uptime", "date", "mem",
    "clear", "reboot", "poweroff", "exit", NULL
};

/* replace the edited line (screen + buffer) with s */
static void line_set(char *line, int *n, int cap, const char *s) {
    while (*n > 0) { (*n)--; kprintf("\b \b"); }
    while (*s && *n < cap - 1) {
        line[(*n)++] = *s;
        console_putchar(*s++);
    }
}

static void hist_prev(char *line, int *n, int cap) {
    int oldest = hist_count > HIST_MAX ? hist_count - HIST_MAX : 0;
    if (hist_pos > oldest) {
        hist_pos--;
        line_set(line, n, cap, hist[hist_pos % HIST_MAX]);
    }
}

static void hist_next(char *line, int *n, int cap) {
    if (hist_pos >= hist_count) return;
    hist_pos++;
    if (hist_pos == hist_count) line_set(line, n, cap, "");
    else line_set(line, n, cap, hist[hist_pos % HIST_MAX]);
}

/* Tab completes the word being typed:
 *  - first word   -> shell command names
 *  - 'spawn ARG'  -> program names enumerated from /bin (no hardcoded list)
 * One match appends the rest plus a space; several list them bash-style
 * and redraw prompt + line. */
static void tab_complete(char *line, int *n, int cap) {
    if (*n == 0 || *n >= cap - 1) return;

    int ws = 0;                              /* start of the word */
    for (int i = 0; i < *n; i++)
        if (line[i] == ' ') ws = i + 1;
    int wl = *n - ws;

    const char *matches[16];
    static char pool[16][32];                /* /bin names outlive this call */
    int nmatch = 0;

    if (ws == 0) {
        for (int i = 0; cmd_names[i] && nmatch < 16; i++)
            if (!strncmp(cmd_names[i], line, wl))
                matches[nmatch++] = cmd_names[i];
    } else if (wl > 0 && ws == 6 && !strncmp(line, "spawn ", 6)) {
        int has_slash = 0;                   /* paths are not completed */
        for (const char *q = line + ws; *q; q++)
            if (*q == '/') { has_slash = 1; break; }
        if (!has_slash) {
            char name[32];
            for (int i = 0;
                 vfs_readdir("/bin", i, name, sizeof(name)) == 0 && nmatch < 16;
                 i++) {
                if (!strncmp(name, line + ws, wl)) {
                    strcpy(pool[nmatch], name);
                    matches[nmatch] = pool[nmatch];
                    nmatch++;
                }
            }
        }
    }
    if (nmatch == 0) return;

    if (nmatch == 1) {
        for (int i = wl; matches[0][i] && *n < cap - 1; i++) {
            line[(*n)++] = matches[0][i];
            console_putchar(matches[0][i]);
        }
        if (*n < cap - 1) { line[(*n)++] = ' '; console_putchar(' '); }
        return;
    }
    kprintf("\n");
    for (int i = 0; i < nmatch; i++) kprintf("%s ", matches[i]);
    kprintf("\nnovix> ");
    for (int i = 0; i < *n; i++) console_putchar(line[i]);
}

/* v1.0.1 (F3): reap children the moment they are zombies. `spawn` leaves
 * tasks running while the shell returns to the prompt; without this sweep
 * every exited child stayed a ZOMBIE forever, holding its slot (and pid)
 * until TASK_MAX ran dry. Runs before each prompt and again before each
 * dispatched command, so `ps` never shows the shell's own stale zombies. */
static void shell_reap_children(void) {
    int st;
    int pid;
    while ((pid = task_waitpid(-1, WNOHANG, &st)) > 0)
        if (pid == fg_pid) fg_pid = -1;  /* never Ctrl+C a recycled pid */
}

void shell_run(void) {
    char line[64];
    kprintf("\nnovix shell -- type 'help'\n");
    for (;;) {
        shell_reap_children();
        kprintf("novix> ");
        int n = 0;
        for (;;) {
            int c = keyboard_getchar();
            if (c == '\n') { console_putchar('\n'); break; }
            if (c == 0x03) {                     /* Ctrl+C -> SIGINT to fg */
                console_putchar('^'); console_putchar('C'); console_putchar('\n');
                if (fg_pid > 0) {
                    struct task *t = task_find(fg_pid);
                    if (t && t->state != TASK_UNUSED && t->state != TASK_ZOMBIE) {
                        signal_send(t, SIGINT);
                        kprintf("sent SIGINT to pid %d (%s)\n", t->pid, t->name);
                    }
                }
                n = 0;
                break;                           /* fresh prompt, empty line */
            }
            if (c == '\t') {                    /* Tab: complete current word */
                tab_complete(line, &n, sizeof(line));
                continue;
            }
            if (c == 0x1b) {                     /* ESC [ A/B: up/down history */
                int c1 = keyboard_trychar();
                if (c1 == '[') {
                    int c2 = keyboard_trychar();
                    if (c2 == 'A') hist_prev(line, &n, sizeof(line));
                    else if (c2 == 'B') hist_next(line, &n, sizeof(line));
                }
                continue;
            }
            if (c == '\b' || c == 0x7f) {     /* BS or DEL: erase last char
                                                 (serial terminals send DEL) */
                if (n > 0) { n--; kprintf("\b \b"); }
                continue;
            }
            if (c >= 32 && c < 127 && n < (int)sizeof(line) - 1) {
                line[n++] = (char)c;
                console_putchar((char)c);
            }
        }
        line[n] = 0;
        if (n > 0) {                            /* remember it (skip repeats) */
            int last = hist_count - 1;
            if (hist_count == 0 || strcmp(hist[last % HIST_MAX], line) != 0) {
                strcpy(hist[hist_count % HIST_MAX], line);
                hist_count++;
            }
        }
        hist_pos = hist_count;
        shell_reap_children();   /* v1.0.1 (F3): deaths since the prompt */
        shell_parse(line);
    }
}
