#ifndef _PCB_H
#define _PCB_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include "memory.h"
#include "file_system.h"
#include "signal.h"

/* 前向声明 */
struct process_security;

/* 进程状态定义 */
typedef enum {
    PROC_STATE_NEW = 0,      /* 新建 */
    PROC_STATE_READY,        /* 就绪 */
    PROC_STATE_RUNNING,      /* 运行 */
    PROC_STATE_BLOCKED,      /* 阻塞 */
    PROC_STATE_ZOMBIE,       /* 僵尸 */
    PROC_STATE_TERMINATED    /* 终止 */
} proc_state_t;

/* 进程控制块结构 */
struct pcb {
    /* 进程标识 */
    pid_t pid;              /* 进程ID */
    pid_t ppid;             /* 父进程ID */
    char name[32];          /* 进程名称 */
    
    /* 状态信息 */
    proc_state_t state;     /* 进程状态 */
    unsigned long create_time;  /* 创建时间 */
    unsigned long cpu_time;     /* CPU时间 */
    
    /* 调度信息 */
    int priority;           /* 优先级 (1-10) */
    int time_slice;         /* 剩余时间片 */
    int original_priority;  /* 原始优先级 */
    
    /* 资源统计 */
    unsigned long memory_usage;     /* 内存使用 */
    unsigned long io_operations;    /* IO操作数 */
    unsigned long context_switches; /* 上下文切换次数 */
    
    /* 进程关系 */
    struct pcb *parent;             /* 父进程 */
    struct list_head children;      /* 子进程列表头 */
    struct list_head sibling;       /* 兄弟进程节点 */
    int child_count;                /* 子进程数量 */
    
    /* 链表管理 */
    struct list_head list;          /* 全局进程链表 */
    struct list_head ready_list;    /* 就绪队列链表 */
    
    /* 上下文保存（模拟） */
    unsigned long regs[16];         /* 模拟寄存器 */
    unsigned long pc;               /* 程序计数器 */
    unsigned long sp;               /* 栈指针 */
    
    /* 锁保护 */
    spinlock_t lock;                /* PCB锁 */
    
    /* 调度统计 */
    unsigned long last_sched_time;  /* 上次调度时间 */
    unsigned long voluntary_switches;   /* 自愿切换 */
    unsigned long involuntary_switches; /* 非自愿切换 */
    
    /* 内存管理 */
    struct process_memory *memory;  /* 进程内存空间 */
    
    /* 文件系统 */
    struct file_descriptor_table *fdt;  /* 文件描述符表 */
    
    /* 信号处理 */
    struct process_signal *signal;  /* 信号处理结构 */
    
    /* 安全管理 */
    struct process_security *security;  /* 安全管理结构 */
    
    /* 实时调度参数 */
    void *realtime_params;  /* 实时调度参数 */
    
    /* 私有数据 */
    void *private_data;  /* 私有数据指针 */
};

/* 全局进程表 */
struct process_table {
    struct list_head all_processes; /* 所有进程链表 */
    struct list_head ready_queues[10]; /* 就绪队列（按优先级） */
    struct list_head blocked_queue; /* 阻塞队列 */
    struct pcb *current;            /* 当前运行进程 */
    spinlock_t lock;                /* 进程表锁 */
    int next_pid;                   /* 下一个可用PID */
    unsigned int process_count;     /* 进程计数 */
};

/* PCB操作函数 */
struct pcb *pcb_create(const char *name, int priority, pid_t ppid);
void pcb_destroy(struct pcb *pcb);
struct pcb *pcb_find_by_pid(pid_t pid);
void pcb_set_state(struct pcb *pcb, proc_state_t new_state);
void pcb_update_stats(struct pcb *pcb, unsigned long cpu_cycles);
void pcb_dump_info(struct pcb *pcb);
int pcb_validate(struct pcb *pcb);
int process_table_init(struct process_table *ptable);
void process_table_cleanup(struct process_table *ptable);
int process_table_add(struct process_table *ptable, struct pcb *pcb);
int process_table_remove(struct process_table *ptable, pid_t pid);
struct pcb *process_table_find(struct process_table *ptable, pid_t pid);
int process_table_count(struct process_table *ptable);

#endif /* _PCB_H */
