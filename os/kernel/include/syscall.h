#ifndef SYSCALL_H
#define SYSCALL_H

#include "isr.h"

#define SYS_EXIT        1
#define SYS_WRITE       2
#define SYS_GETPID      3
#define SYS_YIELD       4
#define SYS_SLEEP       5
#define SYS_SPAWN       6
#define SYS_WAITPID     7
#define SYS_GETCHAR     8
#define SYS_SET_POLICY  9
#define SYS_UPTIME      10
#define SYS_OPEN        11
#define SYS_READ        12
#define SYS_CLOSE       13
#define SYS_LSEEK       14
#define SYS_STAT        15
#define SYS_FORK        16
#define SYS_EXEC        17
#define SYS_FREEPAGES   18      /* debug: pmm_free_pages() for test programs */
/* v0.5.0 */
#define SYS_SBRK        19      /* rdi = increment -> old break */
#define SYS_KILL        20      /* rdi = pid, rsi = signo */
#define SYS_SIGACTION   21      /* rdi = signo, rsi = handler, rdx = restorer */
#define SYS_SIGRETURN   22      /* no args: frame is on the user stack */
#define SYS_PIPE        23      /* rdi = int fds[2] out */
#define SYS_SIGPROCMASK 24      /* rdi = how, rsi = u32* set, rdx = u32* oldset */
/* v0.6.0 */
#define SYS_MMAP        25      /* rdi = length -> va (anonymous, kernel-picked) */
#define SYS_MUNMAP      26      /* rdi = va, rsi = length */
/* v0.9.0 -- users & login */
#define SYS_GETUID      27      /* -> uid */
#define SYS_SETUID      28      /* rdi = uid (caller must be root) */
#define SYS_GETGID      29      /* -> gid */
#define SYS_SETGID      30      /* rdi = gid (caller must be root) */
#define SYS_CHDIR       31      /* rdi = path (dir must grant X) */
#define SYS_GETCWD      32      /* rdi = buf, rsi = len -> strlen */
#define SYS_IOCTL       33      /* rdi = fd(0-2), rsi = req, rdx = u16* flags */
#define SYS_POWEROFF    34      /* isa-debug-exit(0) */
#define SYS_MKDIR       35      /* rdi = path (parent must grant W) */
#define SYS_CHOWN       36      /* rdi = path, rsi = uid, rdx = gid (root only) */
#define SYS_GETRUID     37      /* -> real uid (survives SUID exec) */
/* v0.9.0: SYS_EXEC grows args -- rsi = char** argv, rdx = argc
 * (both 0 = "no arguments", argv[0] defaults to the path) */
/* v1.0.0 -- osh scripting plumbing */
#define SYS_DUP2        38      /* rdi = oldfd, rsi = newfd -> newfd */
/* v1.0.0 -- osh debug reports: the kernel debug shell's ps/cpus/mem as
 * read-only syscalls; truncating append into buf, return bytes stored */
#define SYS_PS          39      /* rdi = buf, rsi = len -> bytes stored */
#define SYS_CPUS        40      /* rdi = buf, rsi = len -> bytes stored */
#define SYS_MEMINFO     41      /* rdi = buf, rsi = len, rdx = verbose */
/* v1.1.0 -- user-space toolbox: ls/rm/date plumbing so the interactive
 * programs live in /bin instead of the kernel shell */
#define SYS_GETDENTS    42      /* rdi = path, rsi = buf, rdx = len -> bytes stored */
#define SYS_UNLINK      43      /* rdi = path (parent must grant W) */
#define SYS_DATETIME    44      /* rdi = buf, rsi = len -> bytes stored */

/* ioctl requests (TCGETS/TCSETS subset; flags word: bit 0x8 = ECHO) */
#define TCGETS 0
#define TCSETS 1
#define TCCLEAR 2              /* v1.1.0: clear the screen + home cursor */
#define TC_ECHO 0x0008

void syscall_dispatch(struct pt_regs *r);

#endif /* SYSCALL_H */
