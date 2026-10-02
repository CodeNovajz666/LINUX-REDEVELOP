#ifndef _SCHEDULER_H
#define _SCHEDULER_H

#include "pcb.h"

/* 调度器类型 */
typedef enum {
    SCHED_PRIORITY = 0,     /* 优先级调度 */
    SCHED_ROUND_ROBIN = 1,  /* 时间片轮转 */
    SCHED_FCFS = 2,         /* 先来先服务 */
    SCHED_EDF = 3,          /* 最早截止时间优先 */
    SCHED_RMS = 4,           /* 速率单调调度 */
    SCHED_MAX
} scheduler_type_t;

/* 调度器配置 */
struct scheduler_config {
    scheduler_type_t type;          /* 调度器类型 */
    int time_slice;                 /* 时间片长度 */
    int preemptive;                 /* 是否抢占 */
    int max_priority;               /* 最大优先级 */
    int min_priority;               /* 最小优先级 */
    unsigned long aging_time;       /* 老化时间 */
};

/* 调度器统计 */
struct scheduler_stats {
    unsigned long total_schedules;          /* 总调度次数 */
    unsigned long context_switches;         /* 上下文切换次数 */
    unsigned long voluntary_switches;       /* 自愿切换 */
    unsigned long involuntary_switches;     /* 非自愿切换 */
    unsigned long idle_time;                /* 空闲时间 */
    unsigned long busy_time;                /* 忙碌时间 */
    unsigned long priority_counts[10];      /* 优先级调度计数 */
    unsigned long max_latency;              /* 最大延迟 */
    unsigned long min_latency;              /* 最小延迟 */
    unsigned long total_latency;            /* 总延迟 */
    unsigned long processes_completed;      /* 完成进程数 */
    float throughput;                       /* 吞吐量 */
};

/* 调度器主结构 */
struct scheduler {
    struct scheduler_config config;         /* 配置 */
    struct list_head ready_queues[10];      /* 就绪队列 */
    struct list_head blocked_queue;         /* 阻塞队列 */
    struct pcb *current_process;            /* 当前运行进程 */
    struct scheduler_stats stats;           /* 统计信息 */
    spinlock_t lock;                        /* 锁 */
    bool enabled;                           /* 启用状态 */
    bool initialized;                       /* 初始化状态 */
    unsigned long clock_ticks;              /* 时钟滴答 */
    unsigned long last_schedule_time;       /* 上次调度时间 */
};

/* 调度器API */
int scheduler_init(struct scheduler *sched, struct scheduler_config *config);
void scheduler_cleanup(struct scheduler *sched);
int scheduler_enqueue(struct scheduler *sched, struct pcb *pcb);
int scheduler_dequeue(struct scheduler *sched, struct pcb *pcb);
struct pcb* scheduler_schedule(struct scheduler *sched);
int scheduler_context_switch(struct scheduler *sched, struct pcb *new_process);
void scheduler_update_stats(struct scheduler *sched, struct pcb *old, struct pcb *new);
int scheduler_aging(struct scheduler *sched);
void scheduler_print_stats(struct scheduler *sched);

/* 调度算法 */
struct pcb* schedule_priority(struct scheduler *sched);
struct pcb* schedule_round_robin(struct scheduler *sched);
struct pcb* schedule_fcfs(struct scheduler *sched);
struct pcb* schedule_edf(struct scheduler *sched);
struct pcb* schedule_rms(struct scheduler *sched);

/* 实时调度相关 */
int scheduler_set_realtime_params(struct pcb *pcb, unsigned long period, unsigned long deadline, unsigned long execution_time);
int scheduler_get_realtime_params(struct pcb *pcb, struct realtime_params *params);
bool scheduler_check_realtime_feasibility(struct scheduler *sched);

/* 实时任务参数 */
struct realtime_params {
    unsigned long period;        /* 任务周期 */
    unsigned long deadline;      /* 任务截止时间 */
    unsigned long execution_time; /* 执行时间 */
    unsigned long next_deadline;  /* 下一个截止时间 */
};

#endif /* _SCHEDULER_H */