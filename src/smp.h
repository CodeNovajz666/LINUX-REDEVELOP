#ifndef _SMP_H
#define _SMP_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include "pcb.h"

/* CPU状态 */
typedef enum {
    CPU_STATE_IDLE = 0,        /* 空闲 */
    CPU_STATE_RUNNING = 1,      /* 运行中 */
    CPU_STATE_OFFLINE = 2,      /* 离线 */
    CPU_STATE_HALTED = 3        /* 暂停 */
} cpu_state_t;

/* CPU结构 */
struct cpu {
    int id;                     /* CPU ID */
    cpu_state_t state;          /* CPU状态 */
    struct pcb *current_process; /* 当前运行进程 */
    struct scheduler *scheduler; /* CPU专用调度器 */
    unsigned long idle_time;     /* 空闲时间 */
    unsigned long busy_time;     /* 忙碌时间 */
    spinlock_t lock;            /* 锁 */
    struct list_head list;       /* 链表节点 */
};

/* 多处理器系统结构 */
struct smp_system {
    struct list_head cpus;       /* CPU链表 */
    int cpu_count;               /* CPU数量 */
    int online_cpus;             /* 在线CPU数量 */
    spinlock_t lock;             /* 锁 */
    bool initialized;            /* 初始化状态 */
};

/* 负载均衡策略 */
typedef enum {
    LOAD_BALANCE_NONE = 0,      /* 无负载均衡 */
    LOAD_BALANCE_PUSH = 1,      /* 推送策略 */
    LOAD_BALANCE_PULL = 2,      /* 拉取策略 */
    LOAD_BALANCE_BOTH = 3       /* 双向策略 */
} load_balance_strategy_t;

/* 负载均衡配置 */
struct load_balance_config {
    load_balance_strategy_t strategy; /* 负载均衡策略 */
    int threshold;              /* 负载阈值 */
    int interval;               /* 均衡间隔 */
    bool enabled;               /* 是否启用 */
};

/* SMP API */
int smp_init(void);
void smp_cleanup(void);
int smp_add_cpu(int id);
int smp_remove_cpu(int id);
struct cpu *smp_get_cpu(int id);
struct cpu *smp_get_current_cpu(void);
int smp_get_cpu_count(void);
int smp_get_online_cpu_count(void);

/* 负载均衡API */
int load_balance_init(struct load_balance_config *config);
void load_balance_cleanup(void);
int load_balance_trigger(void);
int load_balance_between_cpus(struct cpu *src_cpu, struct cpu *dst_cpu);

/* 多处理器调度API */
int smp_schedule(struct cpu *cpu);
int smp_migrate_process(struct pcb *pcb, int cpu_id);
int smp_balance_load(void);

/* CPU操作API */
int cpu_set_state(int cpu_id, cpu_state_t state);
cpu_state_t cpu_get_state(int cpu_id);
int cpu_idle(int cpu_id);
int cpu_halt(int cpu_id);
int cpu_resume(int cpu_id);

#endif /* _SMP_H */
