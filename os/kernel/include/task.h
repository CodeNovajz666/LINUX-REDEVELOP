#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "novix.h"
#include "isr.h"
#include "vfs.h"

struct mm;                                     /* mm.h: address-space VMAs */

typedef enum {
    TASK_UNUSED = 0,
    TASK_RUNNABLE,
    TASK_RUNNING,
    TASK_SLEEPING,
    TASK_WAITING,       /* blocked in waitpid */
    TASK_ZOMBIE,
} task_state_t;

struct task {
    int pid;
    int ppid;
    char name[TASK_NAME_LEN];
    task_state_t state;
    int priority;           /* effective (aging may boost) */
    int base_priority;
    int quantum;            /* ticks left before forced preemption */
    uint32_t wait_ticks;    /* ticks spent runnable-but-not-run (aging) */
    uint64_t wake_tick;     /* sleep until (absolute ticks) */
    uint32_t deadline;      /* absolute tick, EDF */
    uint32_t period;        /* ticks, RMS */
    int exit_status;
    int kill_signo;        /* 0 = exited normally; else signal that killed us */
    int wait_pid;          /* target of waitpid, -1 = any */
    bool is_user;
    bool is_idle;           /* v0.7.0: per-CPU idle tasks -- never picked,
                             * never killable; each CPU falls back to its own */
    uint16_t uid;           /* v0.9.0: user id (0 = root, bypasses VFS perms) */
    uint16_t gid;           /* v0.9.0: group id */
    uint16_t ruid;          /* v0.9.0: real uid -- survives SUID exec, so
                             * /bin/passwd still knows who invoked it */
    char cwd[TASK_CWD_LEN]; /* v0.9.0: working directory ("/" until chdir) */
    int cpu;                /* v0.7.0: CPU that last ran us (ps column) */
    int on_cpu;             /* v0.7.0 ownership: -1 = parked, else the CPU
                             * executing us right now. Pickable only when -1;
                             * only the owner may write kesp/state (see
                             * schedule()'s park/adopt protocol). */
    uint64_t brk;          /* sbrk break (v0.5.0); 0 until an image is loaded */
    uint32_t sigpending;   /* bit (n-1) = signal n pending delivery */
    uint32_t sigblocked;   /* reserved: mask checked at delivery, always 0 now */
    uint64_t sighandlers[SIG_MAX + 1];  /* 0 = default action (terminate) */
    uint64_t sigrestorer;  /* user trampoline pushed as the handler's return addr */
    uint64_t kesp;         /* saved frame rsp (context!) */
    uint64_t kstack_base;  /* kmalloc payload (kfree target) */
    uint64_t kstack_top;   /* base + KSTACK_SIZE; frame sits just below */
    uint64_t page_dir;     /* physical CR3 (PML4) */
    struct mm *mm;         /* VMA list of the address space (v0.6.0) */
    uint64_t ticks_run;
    void (*kfn)(void *);     /* kernel-thread entry + arg via trampoline */
    void *karg;
    struct file *fd[TASK_MAX_FD];   /* open-file table (fd 0/1/2 = console) */
    struct task *next;
};

struct task *current_task(void);
void task_set_current(struct task *t);

struct task *task_create_kernel(const char *name, void (*entry)(void *), void *arg, int priority);
struct task *task_create_idle(const char *name);       /* adopts boot stack */
/* v0.9.0: argv rides onto the fresh user stack (crt0 -> main(argc, argv)) */
struct task *task_create_user(const char *name, struct inode *elf_ino,
                              const char *const argv[], int argc);
/* spawn a user program from a VFS path (e.g. "/bin/hello"); resolves the
 * ELF through ramfs. returns the new task or NULL. argv may be NULL. */
struct task *task_spawn_path(const char *path);
struct task *task_spawn_argv(const char *path, const char *const argv[], int argc);

/* fork/exec (v0.4.0) */
struct task *task_fork(struct pt_regs *parent_regs); /* child of current, or NULL */
/* v0.9.0: exec gains argv -- the strings are pushed onto the fresh user stack
 * so main(argc, argv) works (crt0 reads them off the initial rsp). */
int task_exec(struct pt_regs *r, const char *path,
              const char *const argv[], int argc);    /* replace current image */

void task_exit(int status) __attribute__((noreturn));
void task_kill(int signo) __attribute__((noreturn));  /* kill current task */
void task_kill_task(struct task *t, int signo);       /* kill another task */
int task_waitpid(int pid, int flags);                   /* blocking; status or -1 */
void task_sleep_until(struct task *t, uint64_t tick);

/* v0.7.0: wake a sleeping/waiting task under sched_lock; returns true when
 * the state actually flipped to RUNNABLE (caller may then IPI other CPUs) */
bool task_wake(struct task *t);
/* v0.7.0: true while some online CPU still executes t (zombie reaping guard) */
bool task_executing_anywhere(const struct task *t);

struct task *task_find(int pid);
struct task *task_first(void);                          /* head of all-task list */
struct task *task_next(struct task *t);

const char *task_state_name(task_state_t s);
int task_count(void);
void task_dump_table(void);

#endif /* TASK_H */
