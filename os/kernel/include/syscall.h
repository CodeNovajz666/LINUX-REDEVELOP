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

void syscall_dispatch(struct pt_regs *r);

#endif /* SYSCALL_H */
