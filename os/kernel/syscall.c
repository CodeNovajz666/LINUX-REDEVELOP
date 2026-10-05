#include "novix.h"
#include "isr.h"
#include "syscall.h"
#include "task.h"
#include "sched.h"
#include "timer.h"
#include "keyboard.h"
#include "console.h"
#include "klog.h"
#include "elf.h"
#include "vfs.h"
#include "pmm.h"
#include "paging.h"
#include "mm.h"
#include "signal.h"
#include "pic.h"
#include "lib.h"

/* Syscall ABI (v0.3.0, x86_64): int 0x80 with rax = syscall number,
 * rdi/rsi/rdx = up to three arguments, result in rax. The kernel side
 * runs on the caller's page tables (the kernel low map is shared into
 * every user directory), so user pointers are dereferenced directly. */

/* ---- syscall helpers ---- */

static int copy_str_from_user(char *dst, uint64_t usrc, size_t max) {
    if (usrc < USER_CODE_BASE || usrc >= USER_STACK_TOP) return -1;
    for (size_t i = 0; i < max - 1; i++) {
        dst[i] = *(volatile char *)(uintptr_t)(usrc + i);
        if (!dst[i]) return 0;
    }
    dst[max - 1] = 0;
    return 0;
}

/* user buffer sanity: must stay inside the user range */
static int user_buf_ok(uint64_t buf, uint64_t len) {
    if (len > 65536) return 0;
    if (buf < USER_CODE_BASE || buf + len > USER_STACK_TOP) return 0;
    return 1;
}

/* v0.9.0: copy an argv pointer array out of user memory. The strings stay
 * put -- elf_load copies them onto the new stack synchronously while the
 * caller's CR3 is still loaded, so plain user pointers are safe here. */
static int copy_argv_from_user(uint64_t uargv, int argc, const char *kargv[TASK_MAX_ARGV]) {
    if (argc <= 0 || argc > TASK_MAX_ARGV) return -1;
    if (uargv < USER_CODE_BASE || uargv + 8u * (uint64_t)argc > USER_STACK_TOP) return -1;
    for (int i = 0; i < argc; i++) {
        uint64_t p = *(volatile uint64_t *)(uintptr_t)(uargv + 8u * (uint64_t)i);
        if (p < USER_CODE_BASE || p >= USER_STACK_TOP) return -1;
        kargv[i] = (const char *)(uintptr_t)p;
    }
    return 0;
}

static long sys_write(uint64_t fd, uint64_t buf, uint64_t len) {
    if (!user_buf_ok(buf, len)) return -1;
    struct file *f = fd_get((int)fd);
    if (!f) return -1;
    return vfs_write(f, (const void *)(uintptr_t)buf, (uint32_t)len);
}

static long sys_read(uint64_t fd, uint64_t buf, uint64_t len) {
    if (!user_buf_ok(buf, len)) return -1;
    struct file *f = fd_get((int)fd);
    if (!f) return -1;
    return vfs_read(f, (void *)(uintptr_t)buf, (uint32_t)len);
}

static long sys_open(uint64_t upath, uint64_t flags) {
    char path[80];
    if (copy_str_from_user(path, upath, sizeof(path)) != 0) return -1;
    struct file *f = vfs_open(path, (uint16_t)flags);
    if (!f) return -1;
    int fd = fd_alloc();
    if (fd < 0) { vfs_close(f); return -1; }
    fd_install(fd, f);
    return fd;
}

static long sys_close(uint64_t fd) {
    struct file *f = fd_get((int)fd);
    if (!f) return -1;
    fd_close((int)fd);
    return 0;
}

static long sys_lseek(uint64_t fd, uint64_t off, uint64_t whence) {
    struct file *f = fd_get((int)fd);
    if (!f) return -1;
    return vfs_lseek(f, (int32_t)off, (int32_t)whence);
}

static long sys_stat(uint64_t upath, uint64_t ustat) {
    char path[80];
    if (copy_str_from_user(path, upath, sizeof(path)) != 0) return -1;
    if (ustat < USER_CODE_BASE || ustat + sizeof(struct stat) > USER_STACK_TOP) return -1;
    struct stat st;
    if (vfs_stat(path, &st) != 0) return -1;
    memcpy((void *)(uintptr_t)ustat, &st, sizeof(st));
    return 0;
}

static long sys_spawn(uint64_t uname, uint64_t uargv, uint64_t uargc) {
    char name[64];
    if (copy_str_from_user(name, uname, sizeof(name)) != 0) return -1;
    if (!name[0]) return -1;
    /* bare names resolve under /bin -- "hello" -> "/bin/hello" */
    char path[80];
    if (name[0] == '/') {
        strncpy(path, name, sizeof(path) - 1);
        path[sizeof(path) - 1] = 0;
    } else {
        strcpy(path, "/bin/");
        strncpy(path + 5, name, sizeof(path) - 6);
        path[sizeof(path) - 1] = 0;
    }
    /* v0.9.0: argv rides along (rsi = char** argv, rdx = argc; the pair
     * (0, 0) means "no arguments" and argv[0] defaults to the path) */
    struct task *t;
    if (uargv == 0) {
        if (uargc != 0) return -1;
        t = task_spawn_path(path);
    } else {
        const char *kargv[TASK_MAX_ARGV];
        if (copy_argv_from_user(uargv, (int)(int32_t)uargc, kargv) != 0) return -1;
        t = task_spawn_argv(path, kargv, (int)(int32_t)uargc);
    }
    return t ? t->pid : -1;
}

static long sys_exec(struct pt_regs *r, uint64_t upath, uint64_t uargv, uint64_t uargc) {
    char path[80];
    if (copy_str_from_user(path, upath, sizeof(path)) != 0) return -1;
    if (!path[0]) return -1;
    /* bare names resolve under /bin, same rule as spawn */
    char full[96];
    if (path[0] == '/') {
        strncpy(full, path, sizeof(full) - 1);
        full[sizeof(full) - 1] = 0;
    } else {
        strcpy(full, "/bin/");
        strncpy(full + 5, path, sizeof(full) - 6);
        full[sizeof(full) - 1] = 0;
    }
    /* v0.9.0: argv from rsi/rdx, same convention as spawn */
    if (uargv == 0) {
        if (uargc != 0) return -1;
        return task_exec(r, full, NULL, 0);
    }
    const char *kargv[TASK_MAX_ARGV];
    if (copy_argv_from_user(uargv, (int)(int32_t)uargc, kargv) != 0) return -1;
    /* on success this never "returns": the frame is replaced and the
     * pending iretq enters the new image (rax ends up 0, harmless) */
    return task_exec(r, full, kargv, (int)(int32_t)uargc);
}

/* ---- v0.5.0: sbrk / signals / pipes ---- */

static long sys_sbrk(struct pt_regs *r) {
    (void)r;
    struct task *cur = current_task();
    if (!cur || !cur->is_user || !cur->brk || !cur->mm) return -1;
    long inc = (long)(int64_t)r->rdi;

    uint64_t old = cur->brk;
    uint64_t want = inc >= 0 ? old + (uint64_t)inc : old - (uint64_t)(-inc);
    if (want > USER_HEAP_LIMIT) return -1;
    if (want < USER_CODE_BASE) return -1;

    /* v0.6.0: brk moves only register/trim an anon VMA; the pages
     * themselves fault in on first touch (mm_brk_grow returns -1 as
     * uint64_t on failure -- checked via the cast) */
    if (want > old) {
        if (mm_brk_grow(cur, old, want) == (uint64_t)-1) return -1;
    } else if (want < old) {
        mm_brk_shrink(cur, old, want);
    }
    cur->brk = want;
    return (long)old;
}

/* anonymous mmap: the kernel picks the address (always page-aligned,
 * grown up from USER_HEAP_LIMIT); pages fault in zeroed. */
static long sys_mmap(struct pt_regs *r) {
    struct task *cur = current_task();
    if (!cur || !cur->is_user) return -1;
    uint64_t va = mm_mmap(cur, r->rdi);
    return va ? (long)va : -1;
}

static long sys_munmap(struct pt_regs *r) {
    struct task *cur = current_task();
    if (!cur || !cur->is_user) return -1;
    return mm_munmap(cur, r->rdi, r->rsi);
}

static long sys_kill(int pid, int sig) {
    return signal_send(task_find(pid), sig);
}

static long sys_sigaction(int sig, uint64_t handler, uint64_t restorer) {
    struct task *cur = current_task();
    if (!cur || !cur->is_user) return -1;
    if (sig < SIG_MIN || sig > SIG_MAX || sig == SIGKILL) return -1;
    if (handler && (handler < USER_CODE_BASE || handler >= USER_STACK_TOP))
        return -1;
    if (restorer < USER_CODE_BASE || restorer >= USER_STACK_TOP)
        return -1;
    cur->sighandlers[sig] = handler;         /* 0 restores the default */
    if (handler) cur->sigrestorer = restorer;
    return 0;
}

/* sigprocmask(2) teaching scale: a u32 bitmask, SIGKILL forced off.
 * Either pointer may be null. Unblocking a pending signal delivers it on
 * the way out of this very syscall (the interrupt return path checks
 * sigpending & ~sigblocked). */
static long sys_sigprocmask(uint64_t how, uint64_t uset, uint64_t uold) {
    struct task *cur = current_task();
    if (!cur || !cur->is_user) return -1;
    if (how != SIG_BLOCK && how != SIG_UNBLOCK && how != SIG_SETMASK) return -1;

    uint32_t old = cur->sigblocked;
    if (uset) {
        if (uset < USER_CODE_BASE || uset + 4 > USER_STACK_TOP) return -1;
        uint32_t set = *(volatile uint32_t *)(uintptr_t)uset;
        if (how == SIG_BLOCK)      cur->sigblocked |= set;
        else if (how == SIG_UNBLOCK) cur->sigblocked &= ~set;
        else                       cur->sigblocked = set;
        cur->sigblocked &= ~(1u << (SIGKILL - 1));   /* SIGKILL: unblockable */
    }
    if (uold) {
        if (uold < USER_CODE_BASE || uold + 4 > USER_STACK_TOP) return -1;
        *(volatile uint32_t *)(uintptr_t)uold = old;
    }
    return 0;
}

/* the handler returned into the user-space trampoline, which invoked this
 * syscall; the saved frame starts exactly at the trampoline's rsp (the
 * handler's `ret` already consumed the 8-byte return address) */
static long sys_sigreturn(struct pt_regs *r) {
    struct task *cur = current_task();
    if (!cur || !cur->is_user) return -1;
    uint64_t uframe = r->rsp;
    uint64_t stack_bot = USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE;
    if (uframe < stack_bot || uframe + sizeof(struct pt_regs) > USER_STACK_TOP)
        return -1;

    struct pt_regs saved;
    memcpy(&saved, (const void *)(uintptr_t)uframe, sizeof(saved));
    memcpy(r, &saved, sizeof(*r));
    /* dispatch overwrites rax with our return value: hand back the saved
     * rax so the interrupted syscall's result survives the signal */
    return (long)saved.rax;
}

static long sys_pipe(uint64_t ufd) {
    if (!user_buf_ok(ufd, sizeof(int) * 2)) return -1;
    int fds[2];
    if (pipe_create(&fds[0], &fds[1]) != 0) return -1;
    memcpy((void *)(uintptr_t)ufd, fds, sizeof(fds));
    return 0;
}

/* ---- v0.9.0: users, working directory, terminal, power ---- */

static long sys_getuid(void) {
    struct task *cur = current_task();
    return cur ? cur->uid : 0;
}

static long sys_getruid(void) {
    struct task *cur = current_task();
    return cur ? cur->ruid : 0;
}

static long sys_getgid(void) {
    struct task *cur = current_task();
    return cur ? cur->gid : 0;
}

/* setuid(2) teaching rule: root becomes anyone; a mortal may only restate
 * its own uid. login drops privilege through this call, never raises it. */
static long sys_setuid(uint64_t uid) {
    struct task *cur = current_task();
    if (!cur || !cur->is_user) return -1;
    if (cur->uid == 0) {
        cur->uid = cur->ruid = (uint16_t)uid;
        return 0;
    }
    return (uint16_t)uid == cur->uid ? 0 : -1;
}

static long sys_setgid(uint64_t gid) {
    struct task *cur = current_task();
    if (!cur || !cur->is_user) return -1;
    if (cur->uid == 0) {
        cur->gid = (uint16_t)gid;
        return 0;
    }
    return (uint16_t)gid == cur->gid ? 0 : -1;
}

static long sys_chdir(uint64_t upath) {
    char path[80];
    if (copy_str_from_user(path, upath, sizeof(path)) != 0) return -1;
    struct task *cur = current_task();
    if (!cur || !cur->is_user) return -1;
    char canon[TASK_CWD_LEN];
    if (vfs_canonicalize(cur->cwd, path, canon, sizeof(canon)) != 0) return -1;
    struct inode *ino = namei(canon);       /* the walk already gates on X */
    if (!ino || (ino->mode & S_IFMT) != S_IFDIR) return -1;
    if (!vfs_permitted(ino, VFS_PERM_X)) return -1;
    strcpy(cur->cwd, canon);
    return 0;
}

static long sys_getcwd(uint64_t ubuf, uint64_t len) {
    struct task *cur = current_task();
    if (!cur || !cur->is_user) return -1;
    if (len < 2 || !user_buf_ok(ubuf, len)) return -1;
    size_t n = strlen(cur->cwd);
    if (n + 1 > (size_t)len) return -1;
    memcpy((void *)(uintptr_t)ubuf, cur->cwd, n + 1);
    return (long)n;
}

/* TCGETS/TCSETS subset over stdio: one flag word, bit TC_ECHO. The echo
 * state lives in the console device, shared by every reader -- a single
 * tty is the documented v0.9.0 scope. */
static long sys_ioctl(uint64_t fd, uint64_t req, uint64_t uflags) {
    struct task *cur = current_task();
    if (!cur || !cur->is_user) return -1;
    if (fd > 2) return -1;
    struct file *f = fd_get((int)fd);
    if (!f || !f->inode || (f->inode->mode & S_IFMT) != S_IFCHR) return -1;
    if (uflags < USER_CODE_BASE || uflags + 2 > USER_STACK_TOP) return -1;
    uint16_t *fl = (uint16_t *)(uintptr_t)uflags;
    if (req == TCGETS) {
        *fl = console_echo_enabled() ? TC_ECHO : 0;
        return 0;
    }
    if (req == TCSETS) {
        console_set_echo((*fl & TC_ECHO) != 0);
        return 0;
    }
    return -1;
}

static long sys_mkdir(uint64_t upath) {
    char path[80];
    if (copy_str_from_user(path, upath, sizeof(path)) != 0) return -1;
    return vfs_mkdir(path);               /* parent W checked inside */
}

/* ---- v1.0.0: osh scripting plumbing ---- */

/* dup2(2): the new fd joins the caller's old open-file description (shared
 * offset, like fork's inherited fds). osh's children get their stdin/stdout
 * wired to pipes and files through exactly this call -- fd_close on the
 * target first is the implicit-close dup2(2) semantics. */
static long sys_dup2(uint64_t oldfd, uint64_t newfd) {
    struct task *cur = current_task();
    if (!cur || !cur->is_user) return -1;
    if (oldfd >= TASK_MAX_FD || newfd >= TASK_MAX_FD) return -1;
    struct file *f = fd_get((int)oldfd);
    if (!f) return -1;
    if (oldfd == newfd) return (long)newfd;
    if (cur->fd[newfd]) fd_close((int)newfd);
    cur->fd[newfd] = f;
    f->f_count++;
    return (long)newfd;
}

/* chown(2): root only. FAT stores no ownership, so this edits the inode
 * cache (what the permission checks read) -- documented teaching scope. */
static long sys_chown(uint64_t upath, uint64_t uid, uint64_t gid) {
    struct task *cur = current_task();
    if (!cur || !cur->is_user || cur->uid != 0) return -1;
    char path[80];
    if (copy_str_from_user(path, upath, sizeof(path)) != 0) return -1;
    struct inode *ino = namei(path);
    if (!ino) return -1;
    ino->uid = (uint16_t)uid;
    ino->gid = (uint16_t)gid;
    return 0;
}

void syscall_dispatch(struct pt_regs *r) {
    struct task *cur = current_task();
    long ret;
    switch (r->rax) {
    case SYS_EXIT:
        task_exit((int)(int32_t)r->rdi);
        /* task_exit never returns */
        ret = 0;
        break;
    case SYS_WRITE:
        ret = sys_write(r->rdi, r->rsi, r->rdx);
        break;
    case SYS_GETPID:
        ret = cur ? cur->pid : -1;
        break;
    case SYS_YIELD:
        ret = 0;
        sched_kick();
        break;
    case SYS_SLEEP:
        ret = 0;
        if (cur) {
            task_sleep_until(cur, timer_ticks() + r->rdi * TICK_HZ / 1000 + 1);
            sched_kick();
        }
        break;
    case SYS_SPAWN:
        ret = sys_spawn(r->rdi, r->rsi, r->rdx);
        break;
    case SYS_FORK:
        ret = -1;
        {
            struct task *child = task_fork(r);
            if (child) ret = child->pid;        /* parent: child pid */
                                                /* child frame already has rax=0 */
        }
        break;
    case SYS_EXEC:
        ret = sys_exec(r, r->rdi, r->rsi, r->rdx);
        break;
    case SYS_FREEPAGES:
        ret = (long)pmm_free_pages();
        break;
    case SYS_WAITPID:
        ret = task_waitpid((int)(int32_t)r->rdi, (int)(int32_t)r->rsi);
        break;
    case SYS_SBRK:
        ret = sys_sbrk(r);
        break;
    case SYS_KILL:
        ret = sys_kill((int)(int32_t)r->rdi, (int)(int32_t)r->rsi);
        break;
    case SYS_SIGACTION:
        ret = sys_sigaction((int)(int32_t)r->rdi, r->rsi, r->rdx);
        break;
    case SYS_SIGRETURN:
        ret = sys_sigreturn(r);
        break;
    case SYS_PIPE:
        ret = sys_pipe(r->rdi);
        break;
    case SYS_SIGPROCMASK:
        ret = sys_sigprocmask(r->rdi, r->rsi, r->rdx);
        break;
    case SYS_MMAP:                              /* v0.6.0 */
        ret = sys_mmap(r);
        break;
    case SYS_MUNMAP:
        ret = sys_munmap(r);
        break;
    case SYS_GETCHAR:
        ret = keyboard_getchar();
        break;
    case SYS_SET_POLICY:
        if ((int64_t)r->rdi >= 0 && r->rdi < SCHED_POLICY_COUNT) {
            sched_set_policy((sched_policy_t)r->rdi);
            ret = 0;
        } else {
            ret = -1;
        }
        break;
    case SYS_UPTIME:
        ret = (long)timer_uptime_ms();
        break;
    case SYS_OPEN:
        ret = sys_open(r->rdi, r->rsi);
        break;
    case SYS_READ:
        ret = sys_read(r->rdi, r->rsi, r->rdx);
        break;
    case SYS_CLOSE:
        ret = sys_close(r->rdi);
        break;
    case SYS_LSEEK:
        ret = sys_lseek(r->rdi, r->rsi, r->rdx);
        break;
    case SYS_STAT:
        ret = sys_stat(r->rdi, r->rsi);
        break;
    case SYS_GETUID:                             /* v0.9.0 */
        ret = sys_getuid();
        break;
    case SYS_GETRUID:
        ret = sys_getruid();
        break;
    case SYS_GETGID:
        ret = sys_getgid();
        break;
    case SYS_SETUID:
        ret = sys_setuid(r->rdi);
        break;
    case SYS_SETGID:
        ret = sys_setgid(r->rdi);
        break;
    case SYS_CHDIR:
        ret = sys_chdir(r->rdi);
        break;
    case SYS_GETCWD:
        ret = sys_getcwd(r->rdi, r->rsi);
        break;
    case SYS_IOCTL:
        ret = sys_ioctl(r->rdi, r->rsi, r->rdx);
        break;
    case SYS_MKDIR:
        ret = sys_mkdir(r->rdi);
        break;
    case SYS_CHOWN:
        ret = sys_chown(r->rdi, r->rsi, r->rdx);
        break;
    case SYS_DUP2:
        ret = sys_dup2(r->rdi, r->rsi);
        break;
    case SYS_POWEROFF:
        kprintf("poweroff: requested by pid %d (%s)\n",
                cur ? cur->pid : -1, cur ? cur->name : "?");
        qemu_exit(0);
        ret = 0;                                  /* never reached under QEMU */
        break;
    default:
        ret = -1;
        break;
    }
    r->rax = (uint64_t)ret;
}
