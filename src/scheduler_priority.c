#include "scheduler.h"
#include <linux/spinlock.h>
#include <linux/printk.h>
#include <linux/jiffies.h>

/* 基于优先级的调度算法 */
struct pcb* schedule_priority(struct scheduler *sched) {
    struct pcb *next_process = NULL;
    unsigned long flags;
    int i;
    
    if (!sched || !sched->initialized || !sched->enabled) {
        return NULL;
    }
    
    spin_lock_irqsave(&sched->lock, flags);
    
    /* 更新时间统计 */
    scheduler_tick(sched);
    
    /* 从最高优先级（1）到最低优先级（10）查找就绪进程 */
    for (i = sched->config.max_priority - 1; i <= sched->config.min_priority - 1; i++) {
        if (!list_empty(&sched->ready_queues[i])) {
            /* 获取该优先级队列的第一个进程 */
            next_process = list_first_entry(&sched->ready_queues[i], struct pcb, ready_list);
            
            /* 从就绪队列移除 */
            list_del(&next_process->ready_list);
            
            /* 更新统计 */
            sched->stats.priority_counts[i]++;
            
            printk(KERN_DEBUG "schedule_priority: selected process %d (priority=%d)\n",
                   next_process->pid, next_process->priority);
            break;
        }
    }
    
    /* 如果没有找到就绪进程，返回NULL（空闲状态） */
    if (!next_process) {
        sched->stats.idle_time++;
        printk(KERN_DEBUG "schedule_priority: no ready process found\n");
    } else {
        sched->stats.busy_time++;
    }
    
    sched->stats.total_schedules++;
    
    spin_unlock_irqrestore(&sched->lock, flags);
    
    return next_process;
}

/* 时间片轮转调度算法 */
struct pcb* schedule_round_robin(struct scheduler *sched) {
    struct pcb *next_process = NULL;
    struct pcb *current = sched->current_process;
    unsigned long flags;
    int i;
    
    if (!sched || !sched->initialized || !sched->enabled) {
        return NULL;
    }
    
    spin_lock_irqsave(&sched->lock, flags);
    
    /* 更新时间统计 */
    scheduler_tick(sched);
    
    /* 检查当前进程是否还有时间片 */
    if (current && current->time_slice > 0) {
        /* 还有时间片，继续运行 */
        current->time_slice--;
        next_process = current;
        
        printk(KERN_DEBUG "schedule_round_robin: continuing process %d, time_slice=%d\n",
               current->pid, current->time_slice);
    } else {
        /* 时间片用完或没有运行进程，选择下一个 */
        if (current) {
            /* 将当前进程重新放入就绪队列 */
            current->time_slice = current->priority * 10;  /* 重置时间片 */
            
            /* 重新入队 */
            int priority_index = current->priority - 1;
            if (priority_index < 0) priority_index = 0;
            if (priority_index > 9) priority_index = 9;
            list_add_tail(&current->ready_list, &sched->ready_queues[priority_index]);
            
            printk(KERN_DEBUG "schedule_round_robin: requeued process %d\n", current->pid);
        }
        
        /* 查找下一个就绪进程 */
        for (i = 0; i < 10; i++) {
            if (!list_empty(&sched->ready_queues[i])) {
                next_process = list_first_entry(&sched->ready_queues[i], struct pcb, ready_list);
                list_del(&next_process->ready_list);
                
                /* 更新统计 */
                sched->stats.priority_counts[i]++;
                
                printk(KERN_DEBUG "schedule_round_robin: selected process %d from queue %d\n",
                       next_process->pid, i);
                break;
            }
        }
    }
    
    /* 如果没有找到就绪进程，返回NULL */
    if (!next_process) {
        sched->stats.idle_time++;
        printk(KERN_DEBUG "schedule_round_robin: no ready process found\n");
    } else {
        sched->stats.busy_time++;
    }
    
    sched->stats.total_schedules++;
    
    spin_unlock_irqrestore(&sched->lock, flags);
    
    return next_process;
}

/* 先来先服务调度算法 */
struct pcb* schedule_fcfs(struct scheduler *sched) {
    struct pcb *next_process = NULL;
    struct pcb *pcb;
    unsigned long flags;
    unsigned long earliest_time = (unsigned long)-1;
    int i, selected_queue = -1;
    
    if (!sched || !sched->initialized || !sched->enabled) {
        return NULL;
    }
    
    spin_lock_irqsave(&sched->lock, flags);
    
    /* 更新时间统计 */
    scheduler_tick(sched);
    
    /* 查找最早创建的进程 */
    for (i = 0; i < 10; i++) {
        list_for_each_entry(pcb, &sched->ready_queues[i], ready_list) {
            if (pcb->create_time < earliest_time) {
                earliest_time = pcb->create_time;
                next_process = pcb;
                selected_queue = i;
            }
        }
    }
    
    /* 如果找到进程，从队列中移除 */
    if (next_process) {
        list_del(&next_process->ready_list);
        
        /* 更新统计 */
        if (selected_queue >= 0 && selected_queue < 10) {
            sched->stats.priority_counts[selected_queue]++;
        }
        
        printk(KERN_DEBUG "schedule_fcfs: selected process %d (created at %lu)\n",
               next_process->pid, next_process->create_time);
    }
    
    /* 如果没有找到就绪进程，返回NULL */
    if (!next_process) {
        sched->stats.idle_time++;
        printk(KERN_DEBUG "schedule_fcfs: no ready process found\n");
    } else {
        sched->stats.busy_time++;
    }
    
    sched->stats.total_schedules++;
    
    spin_unlock_irqrestore(&sched->lock, flags);
    
    return next_process;
}

/* 主调度函数 */
struct pcb* scheduler_schedule(struct scheduler *sched) {
    struct pcb *next_process;
    unsigned long start_time, end_time;
    
    if (!sched || !sched->initialized || !sched->enabled) {
        return NULL;
    }
    
    start_time = jiffies;
    
    /* 根据调度器类型选择算法 */
    switch (sched->config.type) {
        case SCHED_PRIORITY:
            next_process = schedule_priority(sched);
            break;
        case SCHED_ROUND_ROBIN:
            next_process = schedule_round_robin(sched);
            break;
        case SCHED_FCFS:
            next_process = schedule_fcfs(sched);
            break;
        case SCHED_EDF:
            next_process = schedule_edf(sched);
            break;
        case SCHED_RMS:
            next_process = schedule_rms(sched);
            break;
        default:
            next_process = schedule_priority(sched);
            break;
    }
    
    /* 记录调度延迟 */
    end_time = jiffies;
    unsigned long latency = end_time - start_time;
    
    if (latency < sched->stats.min_latency) {
        sched->stats.min_latency = latency;
    }
    if (latency > sched->stats.max_latency) {
        sched->stats.max_latency = latency;
    }
    sched->stats.total_latency += latency;
    
    /* 更新当前进程 */
    if (next_process) {
        struct pcb *old_process = sched->current_process;
        sched->current_process = next_process;
        
        /* 更新吞吐量统计 */
        if (old_process && old_process->state == PROC_STATE_TERMINATED) {
            sched->stats.processes_completed++;
        }
        
        /* 计算吞吐量 */
        if (sched->clock_ticks > 0) {
            sched->stats.throughput = (float)sched->stats.processes_completed * HZ / sched->clock_ticks;
        }
        
        sched->last_schedule_time = jiffies;
    }
    
    return next_process;
}