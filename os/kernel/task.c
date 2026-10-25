#include "novix.h"
#include "task.h"
#include "sched.h"
#include "heap.h"
#include "paging.h"
#include "mm.h"
#include "elf.h"
#include "vfs.h"
#include "isr.h"
#include "console.h"
#include "shell.h"
#include "timer.h"
#include "percpu.h"
#include "smp.h"
#include "spinlock.h"
#include "lib.h"

static struct task tasks[TASK_MAX];
static struct task *task_list;               /* creation-ordered */

/* v0.4.0 pid model: pid == slot index, bound forever. A reaped task frees
 * its slot (and thus its pid) for reuse; an unreaped zombie holds both, so
 * waitpid can never confuse an old child with a new one. */

/* v0.7.0: current is per-CPU (this_cpu()->current), read through %gs. */
struct task *current_task(void) { return this_cpu()->current; }
void task_set_current(struct task *t) { this_cpu()->current = t; }

bool task_wake(struct task *t) {
    if (!t) return false;
    irq_flags_t f;
    spin_lock_irqsave(&sched_lock, &f);
    bool woke = t->state == TASK_SLEEPING || t->state == TASK_WAITING;
    if (woke) t->state = TASK_RUNNABLE;
    spin_unlock_irqrestore(&sched_lock, f);
    return woke;
}

bool task_executing_anywhere(const struct task *t) {
    for (int i = 0; i < SMP_MAX; i++)
        if (cpus[i].online && cpus[i].current == t) return true;
    return false;
}

const char *task_state_name(task_state_t s) {
    switch (s) {
    case TASK_UNUSED:   return "UNUSED";
    case TASK_RUNNABLE: return "READY";
    case TASK_RUNNING:  return "RUN";
    case TASK_SLEEPING: return "SLEEP";
    case TASK_WAITING:  return "WAIT";
    case TASK_ZOMBIE:   return "ZOMBIE";
    }
    return "?";
}

static struct task *task_slot(void) {
    for (int i = 0; i < TASK_MAX; i++)
        if (tasks[i].state == TASK_UNUSED)
            return &tasks[i];
    return NULL;
}

static void task_list_add(struct task *t) {
    t->next = NULL;
    if (!task_list) { task_list = t; return; }
    struct task *cur = task_list;
    while (cur->next) cur = cur->next;
    cur->next = t;
}

static void task_list_remove(struct task *t) {
    if (task_list == t) { task_list = t->next; return; }
    for (struct task *c = task_list; c; c = c->next) {
        if (c->next == t) { c->next = t->next; return; }
    }
}

struct task *task_find(int pid) {
    for (struct task *t = task_list; t; t = t->next)
        if (t->pid == pid) return t;
    return NULL;
}

struct task *task_first(void) { return task_list; }
struct task *task_next(struct task *t) { return t ? t->next : NULL; }
int task_count(void) {
    int n = 0;
    for (struct task *t = task_list; t; t = t->next) n++;
    return n;
}

/* Build the fake interrupt frame that a brand-new task "returns" from. */
static void build_first_frame(struct task *t, uint64_t rip, uint64_t rsp, bool user) {
    struct pt_regs *f = (struct pt_regs *)(t->kstack_top - sizeof(struct pt_regs));
    memset(f, 0, sizeof(*f));
    f->rip = rip;
    f->rflags = EFL_IF;
    if (user) {
        f->cs = SEL_UCODE; f->ss = SEL_UDATA; f->rsp = rsp;
    } else {
        f->cs = SEL_KCODE; f->ss = SEL_KDATA; f->rsp = t->kstack_top;
    }
    t->kesp = (uint64_t)(uintptr_t)f;
}

static void task_fill(struct task *t, const char *name, int prio) {
    strncpy(t->name, name, TASK_NAME_LEN - 1);
    t->name[TASK_NAME_LEN - 1] = 0;
    t->priority = t->base_priority = prio;
    t->quantum = 0;
    t->wait_ticks = 0;
    t->wake_tick = 0;
    t->deadline = 0;
    t->period = 0;
    t->exit_status = 0;
    t->kill_signo = 0;
    t->wait_pid = -1;
    t->is_user = false;
    t->is_idle = false;
    t->cpu = -1;
    t->on_cpu = -1;
    t->ticks_run = 0;
    /* v0.9.0: Unix identity + working directory are inherited from the
     * spawner (kernel-thread spawners run as root, cwd "/") */
    {
        struct task *cur = current_task();
        t->uid = cur ? cur->uid : 0;
        t->gid = cur ? cur->gid : 0;
        t->ruid = cur ? cur->ruid : 0;
        t->rgid = cur ? cur->rgid : 0;   /* v1.1.7: real gid follows the same rule */
        strcpy(t->cwd, cur ? cur->cwd : "/");
    }
    memset(t->fd, 0, sizeof(t->fd));
    task_open_std_fds(t);                   /* fd 0/1/2 -> /dev/console */
}

struct task *task_create_idle(const char *name) {
    irq_flags_t f;
    spin_lock_irqsave(&sched_lock, &f);
    struct task *t = task_slot();
    if (!t) { spin_unlock_irqrestore(&sched_lock, f); return NULL; }
    memset(t, 0, sizeof(*t));
    task_fill(t, name, 0);
    t->pid = 0;                               /* slot 0, forever the idle */
    t->ppid = -1;
    t->state = TASK_RUNNING;
    t->is_idle = true;
    t->on_cpu = 0;                        /* BSP owns the boot stack from creation */
    t->kesp = 0;                            /* adopted at first preemption */
    extern char boot_stack_bottom[], boot_stack_top[];  /* boot.S */
    t->kstack_base = (uint64_t)(uintptr_t)boot_stack_bottom;
    t->kstack_top = (uint64_t)(uintptr_t)boot_stack_top;
    t->page_dir = paging_kernel_dir();
    task_list_add(t);
    cpus[0].idle = t;                        /* CPU0 falls back to us */
    task_set_current(t);
    spin_unlock_irqrestore(&sched_lock, f);
    return t;
}

static void kthread_trampoline(void) {
    struct task *t = current_task();
    t->kfn(t->karg);
    task_exit(0);                        /* entry returned: implicit exit(0) */
}

struct task *task_create_kernel(const char *name, void (*entry)(void *), void *arg, int priority) {
    irq_flags_t f;
    spin_lock_irqsave(&sched_lock, &f);
    struct task *t = task_slot();
    if (!t) { spin_unlock_irqrestore(&sched_lock, f); return NULL; }
    memset(t, 0, sizeof(*t));
    task_fill(t, name, priority);
    t->pid = (int)(t - tasks);
    {
        struct task *cur = current_task();
        t->ppid = cur ? cur->pid : -1;
    }
    t->kstack_base = (uint64_t)(uintptr_t)kmalloc(KSTACK_SIZE);
    if (!t->kstack_base) { spin_unlock_irqrestore(&sched_lock, f); return NULL; }
    t->kstack_top = t->kstack_base + KSTACK_SIZE;
    t->page_dir = paging_kernel_dir();
    t->kfn = entry;
    t->karg = arg;
    build_first_frame(t, (uint64_t)(uintptr_t)kthread_trampoline, 0, false);
    /* v1.0.1 (F7): publish the task only once every allocation succeeded.
     * RUNNABLE before the kstack kmalloc turned an OOM into a leaked slot:
     * it was on no list (invisible to the scheduler and task_find) yet
     * never TASK_UNUSED again, so task_slot could not reclaim it. */
    t->state = TASK_RUNNABLE;
    task_list_add(t);
    spin_unlock_irqrestore(&sched_lock, f);
    sched_notify_new_task(t);
    return t;
}

struct task *task_create_user(const char *name, struct inode *elf_ino,
                               const char *const argv[], int argc) {
    struct mm *mm = mm_create();
    if (!mm) return NULL;

    uint64_t pd, entry, uesp, brk;
    if (elf_load(elf_ino, mm, argv, argc, NULL, 0, &pd, &entry, &uesp, &brk) != 0) {
        mm_free_vmas(mm);
        return NULL;
    }

    irq_flags_t f;
    spin_lock_irqsave(&sched_lock, &f);
    struct task *t = task_slot();
    if (!t) {
        spin_unlock_irqrestore(&sched_lock, f);
        paging_destroy_user_dir(pd);
        mm_free_vmas(mm);
        return NULL;
    }
    memset(t, 0, sizeof(*t));
    task_fill(t, name, 5);
    /* v0.9.0: same SUID rule as task_exec -- spawn *is* "run this binary",
     * so a SUID file adopts its owner's uid here too (ruid keeps recording
     * the invoker; without this, sh's spawn-launched /bin/passwd would run
     * as the mortal and could never write /etc/shadow) */
    if ((elf_ino->mode & S_ISUID) && elf_ino->uid != t->uid)
        t->uid = elf_ino->uid;
    t->pid = (int)(t - tasks);
    {
        struct task *cur = current_task();
        t->ppid = cur ? cur->pid : -1;
    }
    t->is_user = true;
    t->brk = brk;
    t->kstack_base = (uint64_t)(uintptr_t)kmalloc(KSTACK_SIZE);
    if (!t->kstack_base) {
        spin_unlock_irqrestore(&sched_lock, f);
        paging_destroy_user_dir(pd);
        mm_free_vmas(mm);
        return NULL;
    }
    t->kstack_top = t->kstack_base + KSTACK_SIZE;
    t->page_dir = pd;
    t->mm = mm;
    build_first_frame(t, entry, uesp, true);
    t->state = TASK_RUNNABLE;        /* F7: after all allocations */
    task_list_add(t);
    spin_unlock_irqrestore(&sched_lock, f);
    sched_notify_new_task(t);
    return t;
}

/* Spawn a user program from a VFS path (e.g. "/bin/hello"): resolve the
 * inode once and hand it to the loader; FILE VMAs page straight from it.
 * v0.9.0: argv rides along onto the fresh stack (argc/argv for main). */
struct task *task_spawn_argv(const char *path, const char *const argv[], int argc) {
    struct inode *ino = namei(path);
    if (!ino || (ino->mode & S_IFMT) != S_IFREG || ino->size == 0) return NULL;
    /* v0.9.0: running a binary is X permission on it (root bypasses) */
    if (!vfs_permitted(ino, VFS_PERM_X)) return NULL;

    const char *slash = path;
    for (const char *p = path; *p; p++)
        if (*p == '/') slash = p + 1;
    char name[TASK_NAME_LEN];
    strncpy(name, *slash ? slash : path, TASK_NAME_LEN - 1);
    name[TASK_NAME_LEN - 1] = 0;

    return task_create_user(name, ino, argv, argc);
}

struct task *task_spawn_path(const char *path) {
    return task_spawn_argv(path, NULL, 0);       /* argv[0] defaults to the path */
}

/* ---- fork (v0.4.0): dup_mm + dup_files + copy_thread ----
 *
 * Called from the SYS_FORK dispatcher with the parent's live interrupt
 * frame (int 0x80 is an interrupt gate, so we run with interrupts off and
 * the parent cannot be preempted mid-fork). The child's first frame is a
 * deep copy with rax = 0; the parent's rax is set by the dispatcher to the
 * child's pid, exactly like the real fork(2). */
struct task *task_fork(struct pt_regs *parent_regs) {
    struct task *parent = current_task();
    if (!parent || !parent->is_user) {
        kprintf("fork: %s (pid %d) is not a user process -- denied\n",
                parent ? parent->name : "?", parent ? parent->pid : -1);
        return NULL;
    }

    irq_flags_t f;
    spin_lock_irqsave(&sched_lock, &f);
    struct task *t = task_slot();
    if (!t) { spin_unlock_irqrestore(&sched_lock, f); return NULL; }

    /* v0.6.0: bring swapped pages home first -- the COW dup below clones
     * present PTEs only, so it must not see swap markers. Slots released;
     * the fresh mappings become ordinary COW twins downstream. */
    swap_prefault(parent->page_dir, parent->mm);

    uint64_t pd = paging_dup_user_dir(parent->page_dir);
    if (!pd) { spin_unlock_irqrestore(&sched_lock, f); return NULL; }

    struct mm *mm = mm_dup(parent->mm);
    if (!mm) {
        spin_unlock_irqrestore(&sched_lock, f);
        paging_destroy_user_dir(pd);
        return NULL;
    }

    memset(t, 0, sizeof(*t));
    strncpy(t->name, parent->name, TASK_NAME_LEN - 1);
    t->name[TASK_NAME_LEN - 1] = 0;
    t->priority = t->base_priority = parent->base_priority;
    t->pid = (int)(t - tasks);
    t->ppid = parent->pid;
    t->on_cpu = -1;                /* parked from birth (memset skipped task_fill) */
    t->is_user = true;
    /* v0.9.0: identity and working directory are inherited across fork
     * (POSIX: the child keeps the parent's uid/gid/cwd) */
    t->uid = parent->uid;
    t->gid = parent->gid;
    t->ruid = parent->ruid;
    t->rgid = parent->rgid;              /* v1.1.7: identity inherits whole */
    strcpy(t->cwd, parent->cwd);
    t->kstack_base = (uint64_t)(uintptr_t)kmalloc(KSTACK_SIZE);
    if (!t->kstack_base) {
        spin_unlock_irqrestore(&sched_lock, f);
        paging_destroy_user_dir(pd);
        mm_free_vmas(mm);
        return NULL;
    }
    t->kstack_top = t->kstack_base + KSTACK_SIZE;
    t->page_dir = pd;
    t->mm = mm;

    /* copy_thread: the child "returns from fork" through this frame */
    struct pt_regs *cf = (struct pt_regs *)(t->kstack_top - sizeof(struct pt_regs));
    *cf = *parent_regs;
    cf->rax = 0;                                /* child sees fork() == 0 */
    t->kesp = (uint64_t)(uintptr_t)cf;

    /* dup_files: share open-file descriptions (Unix semantics) */
    vfs_dup_files(t, parent);

    /* POSIX: the child inherits handlers and the break (the address space
     * is a COW twin, so brk is already true); pending signals are NOT
     * inherited. memset above zeroed sigpending for us. */
    memcpy(t->sighandlers, parent->sighandlers, sizeof(t->sighandlers));
    t->sigrestorer = parent->sigrestorer;
    t->sigblocked = parent->sigblocked;
    t->brk = parent->brk;

    task_list_add(t);
    t->state = TASK_RUNNABLE;        /* F7: after all allocations */
    spin_unlock_irqrestore(&sched_lock, f);
    sched_notify_new_task(t);
    return t;
}

/* ---- exec (v0.4.0): replace the current image, keep pid/ppid/fds ----
 *
 * Loads the new ELF into a fresh directory first, so a bad binary leaves
 * the old image untouched. On success the old directory is destroyed (its
 * COW-shared frames just drop a ref), CR3 moves, and the *live* interrupt
 * frame is overwritten with a fresh first-frame -- the iretq at the end of
 * the syscall path enters the new program without any scheduler tricks.
 *
 * v0.9.0: argv[] is pushed onto the new stack (main(argc, argv)); identity
 * and cwd are NOT reset (POSIX exec keeps uid/gid/cwd) -- except when the
 * binary carries S_ISUID, in which case the caller adopts the file owner's
 * uid while ruid keeps recording who invoked it. Teaching simplification:
 * a single uid, no euid split, so /bin/passwd really becomes root rather
 * than "effectively root". */
int task_exec(struct pt_regs *r, const char *path,
              const char *const argv[], int argc,
              const char *const envp[], int envc) {
    struct task *cur = current_task();
    if (!cur || !cur->is_user) return -1;

    struct inode *ino = namei(path);
    if (!ino || (ino->mode & S_IFMT) != S_IFREG || ino->size == 0) return -1;
    /* v0.9.0: executing a binary is X permission on it */
    if (!vfs_permitted(ino, VFS_PERM_X)) return -1;

    struct mm *mm = mm_create();
    if (!mm) return -1;
    uint64_t pd, entry, uesp, brk;
    if (elf_load(ino, mm, argv, argc, envp, envc,
                 &pd, &entry, &uesp, &brk) != 0) {
        mm_free_vmas(mm);
        return -1;                              /* old image still intact */
    }

    /* swap address spaces */
    mm_release(cur);                            /* old vmas, slots, page dir */
    cur->page_dir = pd;
    cur->mm = mm;
    paging_load_cr3(pd);

    /* the new image's addresses invalidate the old handlers and heap;
     * reset everything to defaults (POSIX exec semantics) */
    cur->brk = brk;
    cur->sigpending = 0;
    cur->sigblocked = 0;
    memset(cur->sighandlers, 0, sizeof(cur->sighandlers));
    cur->sigrestorer = 0;

    /* v0.9.0: setuid binaries adopt their owner's identity (see above) */
    if ((ino->mode & S_ISUID) && ino->uid != cur->uid)
        cur->uid = ino->uid;                     /* ruid stays: who invoked us */

    /* rename to the new basename */
    const char *slash = path;
    for (const char *p = path; *p; p++)
        if (*p == '/') slash = p + 1;
    strncpy(cur->name, *slash ? slash : path, TASK_NAME_LEN - 1);
    cur->name[TASK_NAME_LEN - 1] = 0;

    /* overwrite the live frame: the pending iretq enters the new image */
    memset(r, 0, sizeof(*r));
    r->rip = entry;
    r->rflags = EFL_IF;
    r->cs = SEL_UCODE;
    r->ss = SEL_UDATA;
    r->rsp = uesp;
    return 0;
}

void task_sleep_until(struct task *t, uint64_t tick) {
    irq_flags_t f;
    spin_lock_irqsave(&sched_lock, &f);
    if (t->state == TASK_RUNNING || t->state == TASK_RUNNABLE) {
        t->wake_tick = tick;
        t->state = TASK_SLEEPING;
    }
    spin_unlock_irqrestore(&sched_lock, f);
    sched_kick();
}

void task_exit(int status) {
    struct task *t = current_task();
    /* release fds at exit, not at reap: a zombie keeps only its status.
     * (kill_task skips this path, so waitpid closes again -- idempotent) */
    task_close_files(t);

    irq_flags_t f;
    spin_lock_irqsave(&sched_lock, &f);
    t->exit_status = status;
    t->state = TASK_ZOMBIE;

    /* wake a parent blocked in waitpid on us */
    struct task *p = task_find(t->ppid);
    bool woke_parent = p && p->state == TASK_WAITING &&
                       (p->wait_pid == -1 || p->wait_pid == t->pid);
    if (woke_parent) p->state = TASK_RUNNABLE;

    /* orphaned children are re-parented to init (pid 1) */
    for (struct task *c = task_list; c; c = c->next)
        if (c->ppid == t->pid && c->state != TASK_UNUSED)
            c->ppid = 1;
    spin_unlock_irqrestore(&sched_lock, f);

    if (woke_parent) smp_resched_others();   /* parent may sleep on another CPU */
    for (;;) kyield();          /* switch away forever; we are a zombie */
}

/* Kill the *current* task (called from the exception path): exit status
 * follows the Unix 128+signo convention. The abandoned exception frame
 * stays on this task's kernel stack until waitpid reclaims it. */
void task_kill(int signo) {
    current_task()->kill_signo = signo;
    task_exit(SIG_ERR + signo);
}

/* Kill another task. Under SMP the victim may still be executing on
 * another CPU: it keeps running until its next preemption (<= 10 ms) and
 * the ZOMBIE state guarantees no CPU ever picks it again. Its reaper is
 * held off by the executing-anywhere check in waitpid until then. */
void task_kill_task(struct task *t, int signo) {
    if (!t) return;
    if (t->is_idle) return;      /* a CPU without its idle would hang */
    if (t == current_task()) { task_kill(signo); }
    irq_flags_t f;
    spin_lock_irqsave(&sched_lock, &f);
    if (t->state == TASK_UNUSED || t->state == TASK_ZOMBIE) {
        spin_unlock_irqrestore(&sched_lock, f);
        return;
    }
    t->exit_status = SIG_ERR + signo;
    t->kill_signo = signo;
    t->state = TASK_ZOMBIE;

    struct task *p = task_find(t->ppid);
    bool woke_parent = p && p->state == TASK_WAITING &&
                       (p->wait_pid == -1 || p->wait_pid == t->pid);
    if (woke_parent) p->state = TASK_RUNNABLE;

    for (struct task *c = task_list; c; c = c->next)
        if (c->ppid == t->pid && c->state != TASK_UNUSED)
            c->ppid = 1;
    spin_unlock_irqrestore(&sched_lock, f);

    smp_resched_others();        /* victim's CPU drops it at the next tick */
}

/* v1.0.1: POSIX-exact waitpid. Returns the reaped child's PID and hands
 * its exit status out through *status_out (when non-NULL); 0 means
 * WNOHANG was set and nothing was reapable yet; -1 means the caller has
 * no such child. The old ABI returned the status itself, which made
 * WNOHANG ambiguous: "child alive" and "child exited 0" both read 0, so
 * a poller could never tell a live job from a finished one. WNOHANG also
 * no longer sleeps out the "killed remotely, still draining" window -- a
 * poll reports 0 and the next poll reaps. */
int task_waitpid(int pid, int flags, int *status_out) {
    for (;;) {
        irq_flags_t f;
        bool executing_zombie = false;
        spin_lock_irqsave(&sched_lock, &f);
        bool have_child = false;
        struct task *zombie = NULL;
        for (struct task *t = task_list; t; t = t->next) {
            if (t->ppid != current_task()->pid) continue;
            have_child = true;
            if (t->state != TASK_ZOMBIE) continue;
            if (pid != -1 && t->pid != pid) continue;
            if (task_executing_anywhere(t)) {   /* still on a CPU: not reapable */
                executing_zombie = true;
                continue;
            }
            zombie = t;
            break;
        }
        if (zombie) {
            /* detach from the list under sched_lock so no scheduler can
             * ever pick it again, then reclaim its memory outside the lock
             * (pipe close wakes peers through sched_lock -- no re-take).
             * The slot stays ZOMBIE until every resource is back so a racing
             * task_create can not memset it under our feet. */
            int status = zombie->exit_status;
            int zpid = zombie->pid;
            bool was_user = zombie->is_user;
            uint64_t kstack = zombie->kstack_base;
            task_list_remove(zombie);
            spin_unlock_irqrestore(&sched_lock, f);
            task_close_files(zombie);
            kfree((void *)(uintptr_t)kstack);
            if (was_user) mm_release(zombie);
            zombie->state = TASK_UNUSED;            /* slot recyclable now */
            if (status_out) *status_out = status;
            return zpid;
        }
        spin_unlock_irqrestore(&sched_lock, f);

        if (!have_child) return -1;
        if (flags & WNOHANG) return 0;      /* children alive (or still draining) */
        if (executing_zombie) {
            /* killed remotely, still draining on some CPU: retry after a
             * tick instead of WAITING (nothing will ever wake us). Only
             * blocking waiters sleep here -- WNOHANG bailed out above. */
            task_sleep_until(current_task(), timer_ticks() + 1);
            kyield();
            continue;
        }
        if (flags & WNOHANG) return 0;      /* children alive */
        {
            irq_flags_t f2;
            spin_lock_irqsave(&sched_lock, &f2);
            current_task()->wait_pid = pid;
            current_task()->state = TASK_WAITING;
            spin_unlock_irqrestore(&sched_lock, f2);
        }
        kyield();               /* child exit wakes us */
    }
}

/* v1.0.0: the `ps` table report, streamed through a caller sink. The
 * kernel debug shell prints via shell_emit_console; sys_ps appends the
 * same lines into a user buffer. */
static const char task_report_hdr[] =
    "PID   PPID  STATE     PRI  CPU  UID   TICKS  USER  EXIT      NAME\n";

void task_report_table(shell_emit_t emit, void *ctx) {
    emit(task_report_hdr, (int)sizeof(task_report_hdr) - 1, ctx);
    for (struct task *t = task_list; t; t = t->next) {
        char exit_buf[16];
        const char *exit_str = "-";
        if (t->state == TASK_ZOMBIE) {
            ksnprintf(exit_buf, sizeof(exit_buf),
                      t->kill_signo ? "sig%d" : "%d",
                      t->kill_signo ? t->kill_signo : t->exit_status);
            exit_str = exit_buf;
        }
        char row[96];
        int n = ksnprintf(row, sizeof(row),
                "%-5d %-5d %-9s %-3d %-4d %-5u %-6u %-5s %-9s %s\n",
                t->pid, t->ppid, task_state_name(t->state),
                t->priority, t->cpu, t->uid, (unsigned)t->ticks_run,
                t->is_user ? "yes" : "no", exit_str, t->name);
        emit(row, n, ctx);
    }
}

void task_dump_table(void) {
    task_report_table(shell_emit_console, 0);
}
