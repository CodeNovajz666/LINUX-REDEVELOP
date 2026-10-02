#ifndef _SIGNAL_H
#define _SIGNAL_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/spinlock.h>

/* 信号类型 */
#define SIGHUP      1       /* 挂断信号 */
#define SIGINT      2       /* 中断信号 */
#define SIGQUIT     3       /* 退出信号 */
#define SIGILL      4       /* 非法指令 */
#define SIGTRAP     5       /* 陷阱信号 */
#define SIGABRT     6       /* 异常终止 */
#define SIGBUS      7       /* 总线错误 */
#define SIGFPE      8       /* 浮点异常 */
#define SIGKILL     9       /* 终止信号（不可捕获） */
#define SIGUSR1     10      /* 用户定义信号1 */
#define SIGSEGV     11      /* 段错误 */
#define SIGUSR2     12      /* 用户定义信号2 */
#define SIGPIPE     13      /* 管道破裂 */
#define SIGALRM     14      /* 闹钟信号 */
#define SIGTERM     15      /* 终止信号 */
#define SIGSTKFLT   16      /* 堆栈故障 */
#define SIGCHLD     17      /* 子进程状态变化 */
#define SIGCONT     18      /* 继续执行 */
#define SIGSTOP     19      /* 停止执行（不可捕获） */
#define SIGTSTP     20      /* 终端停止 */
#define SIGTTIN     21      /* 终端输入 */
#define SIGTTOU     22      /* 终端输出 */
#define SIGURG      23      /* 紧急数据 */
#define SIGXCPU     24      /* CPU时间超出 */
#define SIGXFSZ     25      /* 文件大小超出 */
#define SIGVTALRM   26      /* 虚拟闹钟 */
#define SIGPROF     27      /* 性能分析 */
#define SIGWINCH    28      /* 窗口大小变化 */
#define SIGIO       29      /* I/O就绪 */
#define SIGPWR      30      /* 电源故障 */
#define SIGSYS      31      /* 系统调用错误 */

/* 信号处理动作 */
typedef void (*sighandler_t)(int);

/* 信号处理动作类型 */
typedef enum {
    SIG_DFL = 0,    /* 默认动作 */
    SIG_IGN,        /* 忽略信号 */
    SIG_ERR         /* 错误 */
} sigaction_type_t;

/* 信号状态 */
typedef enum {
    SIG_STATUS_PENDING = 0,    /* 待处理 */
    SIG_STATUS_HANDLED,        /* 已处理 */
    SIG_STATUS_IGNORED         /* 已忽略 */
} sig_status_t;

/* 信号结构 */
struct siginfo {
    int si_signo;        /* 信号编号 */
    int si_code;         /* 信号代码 */
    pid_t si_pid;        /* 发送者PID */
    uid_t si_uid;        /* 发送者UID */
    void *si_addr;       /* 错误地址 */
    int si_status;       /* 退出状态 */
    union sigval si_value; /* 信号值 */
};

/* 信号值联合 */
union sigval {
    int sival_int;        /* 整数信号值 */
    void *sival_ptr;      /* 指针信号值 */
};

/* 信号处理结构 */
struct sigaction {
    sighandler_t sa_handler;    /* 信号处理函数 */
    unsigned long sa_flags;     /* 标志位 */
    void (*sa_restorer)(void);  /* 恢复函数 */
    sigset_t sa_mask;           /* 信号掩码 */
};

/* 信号集合 */
#define _SIGSET_NWORDS (1024 / (8 * sizeof(unsigned long)))
typedef struct {
    unsigned long sig[_SIGSET_NWORDS];
} sigset_t;

/* 信号队列项 */
struct sigqueue_item {
    int signo;                 /* 信号编号 */
    struct siginfo info;       /* 信号信息 */
    sig_status_t status;       /* 信号状态 */
    struct list_head list;     /* 链表节点 */
};

/* 进程信号结构 */
struct process_signal {
    pid_t pid;                 /* 进程PID */
    struct sigaction actions[32]; /* 信号处理动作 */
    sigset_t mask;             /* 信号掩码 */
    struct list_head pending;   /* 待处理信号队列 */
    spinlock_t lock;           /* 锁 */
};

/* 信号处理API */
int signal_init(void);
void signal_cleanup(void);
int signal_send(pid_t pid, int signo, const union sigval *value);
int signal_handler(int signo, struct siginfo *info, void *context);
int signal_register_handler(pid_t pid, int signo, sighandler_t handler);
int signal_unregister_handler(pid_t pid, int signo);
int signal_block(pid_t pid, const sigset_t *set);
int signal_unblock(pid_t pid, const sigset_t *set);
int signal_pending(pid_t pid, sigset_t *set);

/* 进程信号API */
int process_signal_init(struct process_signal *psig, pid_t pid);
void process_signal_cleanup(struct process_signal *psig);
int process_signal_send(struct process_signal *psig, int signo, const struct siginfo *info);
int process_signal_handle(struct process_signal *psig);

/* 信号集合操作 */
void sigemptyset(sigset_t *set);
void sigfillset(sigset_t *set);
void sigaddset(sigset_t *set, int signo);
void sigdelset(sigset_t *set, int signo);
bool sigismember(const sigset_t *set, int signo);

#endif /* _SIGNAL_H */
