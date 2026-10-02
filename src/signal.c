#include "signal.h"
#include <linux/slab.h>
#include <linux/printk.h>
#include <linux/errno.h>
#include <linux/string.h>
#include "pcb.h"

/* 全局信号处理状态 */
static struct signal_system {
    struct list_head processes;    /* 进程信号链表 */
    spinlock_t lock;               /* 锁 */
    bool initialized;              /* 初始化状态 */
} g_signal_system;

/* 初始化信号系统 */
int signal_init(void) {
    /* 初始化链表 */
    INIT_LIST_HEAD(&g_signal_system.processes);
    
    /* 初始化锁 */
    spin_lock_init(&g_signal_system.lock);
    
    g_signal_system.initialized = true;
    printk(KERN_INFO "signal_init: initialized\n");
    return 0;
}

/* 清理信号系统 */
void signal_cleanup(void) {
    if (!g_signal_system.initialized) {
        return;
    }
    
    /* 清理所有进程信号结构 */
    /* 注意：实际应用中需要遍历并清理所有进程的信号结构 */
    
    g_signal_system.initialized = false;
    printk(KERN_INFO "signal_cleanup: cleaned up\n");
}

/* 发送信号 */
int signal_send(pid_t pid, int signo, const union sigval *value) {
    struct pcb *pcb;
    struct process_signal *psig;
    struct siginfo info;
    
    if (!g_signal_system.initialized) {
        return -EINVAL;
    }
    
    /* 查找进程 */
    pcb = pcb_find_by_pid(pid);
    if (!pcb) {
        return -ESRCH;
    }
    
    /* 检查信号编号 */
    if (signo < 1 || signo > 31) {
        return -EINVAL;
    }
    
    /* 初始化信号信息 */
    memset(&info, 0, sizeof(struct siginfo));
    info.si_signo = signo;
    info.si_code = 0;
    info.si_pid = 0; /* 内核发送 */
    info.si_uid = 0;
    
    if (value) {
        info.si_value = *value;
    }
    
    /* 获取进程信号结构 */
    psig = pcb->signal;
    if (!psig) {
        return -EINVAL;
    }
    
    /* 发送信号 */
    return process_signal_send(psig, signo, &info);
}

/* 信号处理函数 */
int signal_handler(int signo, struct siginfo *info, void *context) {
    printk(KERN_INFO "signal_handler: received signal %d\n", signo);
    
    /* 根据信号编号执行相应的处理 */
    switch (signo) {
        case SIGTERM:
        case SIGINT:
        case SIGKILL:
            printk(KERN_INFO "signal_handler: terminating process\n");
            /* 这里应该调用进程终止函数 */
            break;
        case SIGSEGV:
            printk(KERN_INFO "signal_handler: segmentation fault\n");
            /* 这里应该处理段错误 */
            break;
        default:
            printk(KERN_INFO "signal_handler: handling signal %d\n", signo);
            break;
    }
    
    return 0;
}

/* 注册信号处理函数 */
int signal_register_handler(pid_t pid, int signo, sighandler_t handler) {
    struct pcb *pcb;
    struct process_signal *psig;
    
    if (!g_signal_system.initialized) {
        return -EINVAL;
    }
    
    /* 查找进程 */
    pcb = pcb_find_by_pid(pid);
    if (!pcb) {
        return -ESRCH;
    }
    
    /* 检查信号编号 */
    if (signo < 1 || signo > 31) {
        return -EINVAL;
    }
    
    /* 获取进程信号结构 */
    psig = pcb->signal;
    if (!psig) {
        return -EINVAL;
    }
    
    /* 注册处理函数 */
    psig->actions[signo].sa_handler = handler;
    printk(KERN_INFO "signal_register_handler: registered handler for signal %d\n", signo);
    return 0;
}

/* 注销信号处理函数 */
int signal_unregister_handler(pid_t pid, int signo) {
    struct pcb *pcb;
    struct process_signal *psig;
    
    if (!g_signal_system.initialized) {
        return -EINVAL;
    }
    
    /* 查找进程 */
    pcb = pcb_find_by_pid(pid);
    if (!pcb) {
        return -ESRCH;
    }
    
    /* 检查信号编号 */
    if (signo < 1 || signo > 31) {
        return -EINVAL;
    }
    
    /* 获取进程信号结构 */
    psig = pcb->signal;
    if (!psig) {
        return -EINVAL;
    }
    
    /* 注销处理函数 */
    psig->actions[signo].sa_handler = SIG_DFL;
    printk(KERN_INFO "signal_unregister_handler: unregistered handler for signal %d\n", signo);
    return 0;
}

/* 阻塞信号 */
int signal_block(pid_t pid, const sigset_t *set) {
    struct pcb *pcb;
    struct process_signal *psig;
    int i;
    
    if (!g_signal_system.initialized || !set) {
        return -EINVAL;
    }
    
    /* 查找进程 */
    pcb = pcb_find_by_pid(pid);
    if (!pcb) {
        return -ESRCH;
    }
    
    /* 获取进程信号结构 */
    psig = pcb->signal;
    if (!psig) {
        return -EINVAL;
    }
    
    /* 阻塞信号 */
    for (i = 1; i <= 31; i++) {
        if (sigismember(set, i)) {
            sigaddset(&psig->mask, i);
        }
    }
    
    printk(KERN_INFO "signal_block: blocked signals for PID %d\n", pid);
    return 0;
}

/* 解除信号阻塞 */
int signal_unblock(pid_t pid, const sigset_t *set) {
    struct pcb *pcb;
    struct process_signal *psig;
    int i;
    
    if (!g_signal_system.initialized || !set) {
        return -EINVAL;
    }
    
    /* 查找进程 */
    pcb = pcb_find_by_pid(pid);
    if (!pcb) {
        return -ESRCH;
    }
    
    /* 获取进程信号结构 */
    psig = pcb->signal;
    if (!psig) {
        return -EINVAL;
    }
    
    /* 解除信号阻塞 */
    for (i = 1; i <= 31; i++) {
        if (sigismember(set, i)) {
            sigdelset(&psig->mask, i);
        }
    }
    
    printk(KERN_INFO "signal_unblock: unblocked signals for PID %d\n", pid);
    return 0;
}

/* 获取待处理信号 */
int signal_pending(pid_t pid, sigset_t *set) {
    struct pcb *pcb;
    struct process_signal *psig;
    struct sigqueue_item *item;
    
    if (!g_signal_system.initialized || !set) {
        return -EINVAL;
    }
    
    /* 查找进程 */
    pcb = pcb_find_by_pid(pid);
    if (!pcb) {
        return -ESRCH;
    }
    
    /* 获取进程信号结构 */
    psig = pcb->signal;
    if (!psig) {
        return -EINVAL;
    }
    
    /* 清空信号集合 */
    sigemptyset(set);
    
    /* 查找待处理信号 */
    list_for_each_entry(item, &psig->pending, list) {
        if (item->status == SIG_STATUS_PENDING) {
            sigaddset(set, item->signo);
        }
    }
    
    return 0;
}

/* 初始化进程信号结构 */
int process_signal_init(struct process_signal *psig, pid_t pid) {
    int i;
    
    if (!psig) {
        return -EINVAL;
    }
    
    psig->pid = pid;
    
    /* 初始化信号处理动作 */
    for (i = 0; i < 32; i++) {
        psig->actions[i].sa_handler = SIG_DFL;
        psig->actions[i].sa_flags = 0;
        psig->actions[i].sa_restorer = NULL;
        sigemptyset(&psig->actions[i].sa_mask);
    }
    
    /* 初始化信号掩码 */
    sigemptyset(&psig->mask);
    
    /* 初始化待处理信号队列 */
    INIT_LIST_HEAD(&psig->pending);
    
    /* 初始化锁 */
    spin_lock_init(&psig->lock);
    
    printk(KERN_INFO "process_signal_init: initialized for PID %d\n", pid);
    return 0;
}

/* 清理进程信号结构 */
void process_signal_cleanup(struct process_signal *psig) {
    struct sigqueue_item *item, *tmp;
    struct list_head *pos, *q;
    
    if (!psig) {
        return;
    }
    
    /* 清理待处理信号队列 */
    list_for_each_safe(pos, q, &psig->pending) {
        item = list_entry(pos, struct sigqueue_item, list);
        list_del(&item->list);
        kfree(item);
    }
    
    printk(KERN_INFO "process_signal_cleanup: cleaned up for PID %d\n", psig->pid);
}

/* 发送信号到进程 */
int process_signal_send(struct process_signal *psig, int signo, const struct siginfo *info) {
    struct sigqueue_item *item;
    unsigned long flags;
    
    if (!psig || !info) {
        return -EINVAL;
    }
    
    /* 检查信号是否被阻塞 */
    if (sigismember(&psig->mask, signo)) {
        printk(KERN_DEBUG "process_signal_send: signal %d blocked\n", signo);
        return 0;
    }
    
    /* 创建信号队列项 */
    item = kmalloc(sizeof(struct sigqueue_item), GFP_KERNEL);
    if (!item) {
        return -ENOMEM;
    }
    
    item->signo = signo;
    memcpy(&item->info, info, sizeof(struct siginfo));
    item->status = SIG_STATUS_PENDING;
    INIT_LIST_HEAD(&item->list);
    
    spin_lock_irqsave(&psig->lock, flags);
    
    /* 添加到待处理信号队列 */
    list_add_tail(&item->list, &psig->pending);
    
    spin_unlock_irqrestore(&psig->lock, flags);
    
    printk(KERN_INFO "process_signal_send: sent signal %d to PID %d\n", signo, psig->pid);
    return 0;
}

/* 处理进程信号 */
int process_signal_handle(struct process_signal *psig) {
    struct sigqueue_item *item, *tmp;
    struct list_head *pos, *q;
    unsigned long flags;
    int handled = 0;
    
    if (!psig) {
        return -EINVAL;
    }
    
    spin_lock_irqsave(&psig->lock, flags);
    
    /* 遍历待处理信号队列 */
    list_for_each_safe(pos, q, &psig->pending) {
        item = list_entry(pos, struct sigqueue_item, list);
        
        /* 检查信号是否被阻塞 */
        if (sigismember(&psig->mask, item->signo)) {
            continue;
        }
        
        /* 处理信号 */
        switch (psig->actions[item->signo].sa_handler) {
            case SIG_DFL:
                /* 默认处理 */
                printk(KERN_INFO "process_signal_handle: default handling signal %d\n", item->signo);
                /* 根据信号类型执行默认动作 */
                switch (item->signo) {
                    case SIGTERM:
                    case SIGINT:
                    case SIGKILL:
                        printk(KERN_INFO "process_signal_handle: terminating process %d\n", psig->pid);
                        /* 这里应该调用进程终止函数 */
                        break;
                    case SIGSEGV:
                        printk(KERN_INFO "process_signal_handle: segmentation fault\n");
                        /* 这里应该处理段错误 */
                        break;
                    default:
                        break;
                }
                break;
                
            case SIG_IGN:
                /* 忽略信号 */
                printk(KERN_INFO "process_signal_handle: ignoring signal %d\n", item->signo);
                break;
                
            default:
                /* 自定义处理函数 */
                printk(KERN_INFO "process_signal_handle: calling custom handler for signal %d\n", item->signo);
                if (psig->actions[item->signo].sa_handler) {
                    psig->actions[item->signo].sa_handler(item->signo);
                }
                break;
        }
        
        /* 标记信号为已处理 */
        item->status = SIG_STATUS_HANDLED;
        handled++;
        
        /* 从队列中移除 */
        list_del(&item->list);
        kfree(item);
    }
    
    spin_unlock_irqrestore(&psig->lock, flags);
    
    return handled;
}

/* 清空信号集合 */
void sigemptyset(sigset_t *set) {
    if (set) {
        memset(set, 0, sizeof(sigset_t));
    }
}

/* 填充信号集合 */
void sigfillset(sigset_t *set) {
    if (set) {
        memset(set, 0xFF, sizeof(sigset_t));
    }
}

/* 添加信号到集合 */
void sigaddset(sigset_t *set, int signo) {
    if (set && signo >= 1 && signo <= 31) {
        unsigned int word = (signo - 1) / (8 * sizeof(unsigned long));
        unsigned int bit = (signo - 1) % (8 * sizeof(unsigned long));
        set->sig[word] |= (1UL << bit);
    }
}

/* 从集合中删除信号 */
void sigdelset(sigset_t *set, int signo) {
    if (set && signo >= 1 && signo <= 31) {
        unsigned int word = (signo - 1) / (8 * sizeof(unsigned long));
        unsigned int bit = (signo - 1) % (8 * sizeof(unsigned long));
        set->sig[word] &= ~(1UL << bit);
    }
}

/* 检查信号是否在集合中 */
bool sigismember(const sigset_t *set, int signo) {
    if (set && signo >= 1 && signo <= 31) {
        unsigned int word = (signo - 1) / (8 * sizeof(unsigned long));
        unsigned int bit = (signo - 1) % (8 * sizeof(unsigned long));
        return (set->sig[word] & (1UL << bit)) != 0;
    }
    return false;
}
