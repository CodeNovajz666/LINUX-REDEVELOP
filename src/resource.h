#ifndef _RESOURCE_H
#define _RESOURCE_H

#include "pcb.h"

/* 资源统计结构 */
struct resource_stats {
    unsigned long total_cpu_time;       /* 总CPU时间 */
    unsigned long total_memory;         /* 总内存使用 */
    unsigned long total_io_operations;  /* 总IO操作数 */
    unsigned long total_context_switches; /* 总上下文切换 */
    int process_count;                  /* 进程计数 */
    int running_count;                  /* 运行进程数 */
    int ready_count;                    /* 就绪进程数 */
    int blocked_count;                  /* 阻塞进程数 */
    int zombie_count;                   /* 僵尸进程数 */
};

/* 资源管理函数 */
void resource_update_cpu(struct pcb *pcb, unsigned long cycles);
void resource_update_memory(struct pcb *pcb, unsigned long memory);
void resource_update_io(struct pcb *pcb, unsigned long io_count);
void resource_update_context_switch(struct pcb *pcb);
void resource_get_stats(struct process_table *ptable, struct resource_stats *stats);
void resource_print_stats(struct process_table *ptable);
unsigned long resource_calculate_usage(struct pcb *pcb);
void resource_reset_stats(struct pcb *pcb);

#endif /* _RESOURCE_H */