#include "scheduler.h"
#include <linux/spinlock.h>
#include <linux/printk.h>
#include <linux/jiffies.h>

/* 实时调度算法：最早截止时间优先（EDF） */
struct pcb* schedule_edf(struct scheduler *sched) {
    struct pcb *next_process = NULL;
    struct pcb *pcb;
    struct realtime_params *rt_params;
    unsigned long flags;
    unsigned long earliest_deadline = (unsigned long)-1;
    int i;
    
    if (!sched || !sched->initialized || !sched->enabled) {
        return NULL;
    }
    
    spin_lock_irqsave(&sched->lock, flags);
    
    /* 更新时间统计 */
    scheduler_tick(sched);
    
    /* 查找最早截止时间的进程 */
    for (i = 0; i < 10; i++) {
        list_for_each_entry(pcb, &sched->ready_queues[i], ready_list) {
            rt_params = (struct realtime_params *)pcb->realtime_params;
            if (rt_params && rt_params->next_deadline < earliest_deadline) {
                earliest_deadline = rt_params->next_deadline;
                next_process = pcb;
            }
        }
    }
    
    /* 如果找到进程，从队列中移除 */
    if (next_process) {
        list_del(&next_process->ready_list);
        
        /* 更新下一个截止时间 */
        rt_params = (struct realtime_params *)next_process->realtime_params;
        if (rt_params) {
            rt_params->next_deadline += rt_params->period;
        }
        
        /* 更新统计 */
        int priority_index = next_process->priority - 1;
        if (priority_index < 0) priority_index = 0;
        if (priority_index > 9) priority_index = 9;
        sched->stats.priority_counts[priority_index]++;
        
        printk(KERN_DEBUG "schedule_edf: selected process %d (deadline=%lu)\n",
               next_process->pid, earliest_deadline);
    } else {
        sched->stats.idle_time++;
        printk(KERN_DEBUG "schedule_edf: no ready process found\n");
    }
    
    sched->stats.total_schedules++;
    
    spin_unlock_irqrestore(&sched->lock, flags);
    
    return next_process;
}

/* 实时调度算法：速率单调调度（RMS） */
struct pcb* schedule_rms(struct scheduler *sched) {
    struct pcb *next_process = NULL;
    struct pcb *pcb;
    struct realtime_params *rt_params;
    unsigned long flags;
    unsigned long min_period = (unsigned long)-1;
    int i;
    
    if (!sched || !sched->initialized || !sched->enabled) {
        return NULL;
    }
    
    spin_lock_irqsave(&sched->lock, flags);
    
    /* 更新时间统计 */
    scheduler_tick(sched);
    
    /* 查找周期最小的进程 */
    for (i = 0; i < 10; i++) {
        list_for_each_entry(pcb, &sched->ready_queues[i], ready_list) {
            rt_params = (struct realtime_params *)pcb->realtime_params;
            if (rt_params && rt_params->period < min_period) {
                min_period = rt_params->period;
                next_process = pcb;
            }
        }
    }
    
    /* 如果找到进程，从队列中移除 */
    if (next_process) {
        list_del(&next_process->ready_list);
        
        /* 更新下一个截止时间 */
        rt_params = (struct realtime_params *)next_process->realtime_params;
        if (rt_params) {
            rt_params->next_deadline += rt_params->period;
        }
        
        /* 更新统计 */
        int priority_index = next_process->priority - 1;
        if (priority_index < 0) priority_index = 0;
        if (priority_index > 9) priority_index = 9;
        sched->stats.priority_counts[priority_index]++;
        
        printk(KERN_DEBUG "schedule_rms: selected process %d (period=%lu)\n",
               next_process->pid, min_period);
    } else {
        sched->stats.idle_time++;
        printk(KERN_DEBUG "schedule_rms: no ready process found\n");
    }
    
    sched->stats.total_schedules++;
    
    spin_unlock_irqrestore(&sched->lock, flags);
    
    return next_process;
}

/* 设置实时任务参数 */
int scheduler_set_realtime_params(struct pcb *pcb, unsigned long period, unsigned long deadline, unsigned long execution_time) {
    struct realtime_params *rt_params;
    
    if (!pcb) {
        return -EINVAL;
    }
    
    /* 分配实时参数结构 */
    rt_params = kmalloc(sizeof(struct realtime_params), GFP_KERNEL);
    if (!rt_params) {
        return -ENOMEM;
    }
    
    rt_params->period = period;
    rt_params->deadline = deadline;
    rt_params->execution_time = execution_time;
    rt_params->next_deadline = jiffies + deadline;
    
    /* 设置为进程私有数据 */
    pcb->realtime_params = rt_params;
    
    printk(KERN_INFO "scheduler_set_realtime_params: PID %d set to real-time task (period=%lu, deadline=%lu)\n",
           pcb->pid, period, deadline);
    return 0;
}

/* 获取实时任务参数 */
int scheduler_get_realtime_params(struct pcb *pcb, struct realtime_params *params) {
    struct realtime_params *rt_params;
    
    if (!pcb || !params) {
        return -EINVAL;
    }
    
    rt_params = (struct realtime_params *)pcb->realtime_params;
    if (!rt_params) {
        return -ENOENT;
    }
    
    memcpy(params, rt_params, sizeof(struct realtime_params));
    return 0;
}

/* 检查实时任务可行性 */
bool scheduler_check_realtime_feasibility(struct scheduler *sched) {
    struct pcb *pcb;
    struct realtime_params *rt_params;
    unsigned long flags;
    float utilization = 0.0;
    int i, task_count = 0;
    
    if (!sched || !sched->initialized) {
        return false;
    }
    
    spin_lock_irqsave(&sched->lock, flags);
    
    /* 计算系统利用率 */
    for (i = 0; i < 10; i++) {
        list_for_each_entry(pcb, &sched->ready_queues[i], ready_list) {
            rt_params = (struct realtime_params *)pcb->realtime_params;
            if (rt_params && rt_params->period > 0) {
                utilization += (float)rt_params->execution_time / rt_params->period;
                task_count++;
            }
        }
    }
    
    spin_unlock_irqrestore(&sched->lock, flags);
    
    /* 检查RMS可行性条件：利用率 <= n(2^(1/n) - 1) */
    if (task_count > 0) {
        float bound = task_count * (pow(2, 1.0 / task_count) - 1);
        if (utilization <= bound) {
            printk(KERN_INFO "scheduler_check_realtime_feasibility: feasible (utilization=%.2f, bound=%.2f)\n",
                   utilization, bound);
            return true;
        } else {
            printk(KERN_WARNING "scheduler_check_realtime_feasibility: not feasible (utilization=%.2f, bound=%.2f)\n",
                   utilization, bound);
            return false;
        }
    }
    
    return true;
}
