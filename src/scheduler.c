#include "scheduler.h"
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/printk.h>
#include <linux/jiffies.h>
#include <linux/string.h>
#include <linux/errno.h>

/* 默认配置 */
static struct scheduler_config default_config = {
    .type = SCHED_PRIORITY,
    .time_slice = 100,
    .preemptive = 1,
    .max_priority = 1,
    .min_priority = 10,
    .aging_time = 1000,
};

/* 初始化调度器 */
int scheduler_init(struct scheduler *sched, struct scheduler_config *config) {
    int i;
    
    if (!sched) {
        return -EINVAL;
    }
    
    /* 使用默认配置或提供的配置 */
    if (config) {
        memcpy(&sched->config, config, sizeof(struct scheduler_config));
    } else {
        memcpy(&sched->config, &default_config, sizeof(struct scheduler_config));
    }
    
    /* 验证配置 */
    if (sched->config.type >= SCHED_MAX) {
        printk(KERN_ERR "scheduler_init: invalid scheduler type %d\n", sched->config.type);
        return -EINVAL;
    }
    
    if (sched->config.max_priority < sched->config.min_priority) {
        printk(KERN_ERR "scheduler_init: invalid priority range %d-%d\n",
               sched->config.max_priority, sched->config.min_priority);
        return -EINVAL;
    }
    
    /* 初始化就绪队列 */
    for (i = 0; i < 10; i++) {
        INIT_LIST_HEAD(&sched->ready_queues[i]);
    }
    
    /* 初始化阻塞队列 */
    INIT_LIST_HEAD(&sched->blocked_queue);
    
    /* 初始化锁 */
    spin_lock_init(&sched->lock);
    
    /* 初始化统计信息 */
    memset(&sched->stats, 0, sizeof(struct scheduler_stats));
    sched->stats.min_latency = (unsigned long)-1;
    
    /* 设置运行进程为NULL */
    sched->current_process = NULL;
    
    /* 初始化状态 */
    sched->enabled = true;
    sched->initialized = true;
    sched->clock_ticks = 0;
    sched->last_schedule_time = jiffies;
    
    printk(KERN_INFO "scheduler_init: scheduler initialized (type=%d, preemptive=%d)\n",
           sched->config.type, sched->config.preemptive);
    
    return 0;
}

/* 清理调度器 */
void scheduler_cleanup(struct scheduler *sched) {
    unsigned long flags;
    
    if (!sched || !sched->initialized) {
        return;
    }
    
    spin_lock_irqsave(&sched->lock, flags);
    
    /* 禁用调度器 */
    sched->enabled = false;
    sched->current_process = NULL;
    
    /* 打印统计信息 */
    scheduler_print_stats(sched);
    
    /* 重置状态 */
    sched->initialized = false;
    
    spin_unlock_irqrestore(&sched->lock, flags);
    
    printk(KERN_INFO "scheduler_cleanup: scheduler cleaned up\n");
}

/* 将进程加入就绪队列 */
int scheduler_enqueue(struct scheduler *sched, struct pcb *pcb) {
    unsigned long flags;
    int priority_index;
    
    if (!sched || !pcb || !sched->initialized) {
        return -EINVAL;
    }
    
    if (pcb->state != PROC_STATE_READY) {
        printk(KERN_WARNING "scheduler_enqueue: process %d not in READY state\n", pcb->pid);
        return -EINVAL;
    }
    
    spin_lock_irqsave(&sched->lock, flags);
    
    /* 计算优先级索引 */
    priority_index = pcb->priority - 1;
    if (priority_index < 0) priority_index = 0;
    if (priority_index > 9) priority_index = 9;
    
    /* 添加到对应优先级的就绪队列 */
    list_add_tail(&pcb->ready_list, &sched->ready_queues[priority_index]);
    
    /* 更新进程的调度时间 */
    pcb->last_sched_time = sched->clock_ticks;
    
    spin_unlock_irqrestore(&sched->lock, flags);
    
    printk(KERN_DEBUG "scheduler_enqueue: enqueued process %d (priority=%d)\n",
           pcb->pid, pcb->priority);
    
    return 0;
}

/* 从就绪队列移除进程 */
int scheduler_dequeue(struct scheduler *sched, struct pcb *pcb) {
    unsigned long flags;
    
    if (!sched || !pcb || !sched->initialized) {
        return -EINVAL;
    }
    
    spin_lock_irqsave(&sched->lock, flags);
    
    /* 如果进程在就绪队列中，移除它 */
    if (!list_empty(&pcb->ready_list)) {
        list_del_init(&pcb->ready_list);
    }
    
    spin_unlock_irqrestore(&sched->lock, flags);
    
    printk(KERN_DEBUG "scheduler_dequeue: dequeued process %d\n", pcb->pid);
    
    return 0;
}

/* 更新调度器时钟 */
void scheduler_tick(struct scheduler *sched) {
    if (!sched || !sched->initialized) {
        return;
    }
    
    sched->clock_ticks++;
    
    /* 定期执行老化 */
    if (sched->clock_ticks % sched->config.aging_time == 0) {
        scheduler_aging(sched);
    }
}

/* 进程老化（防止低优先级饥饿） */
int scheduler_aging(struct scheduler *sched) {
    struct pcb *pcb, *tmp;
    int i;
    unsigned long flags;
    int aged_count = 0;
    
    if (!sched || !sched->initialized) {
        return 0;
    }
    
    spin_lock_irqsave(&sched->lock, flags);
    
    /* 遍历所有优先级队列（从低到高） */
    for (i = sched->config.min_priority - 1; i > sched->config.max_priority - 1; i--) {
        list_for_each_entry_safe(pcb, tmp, &sched->ready_queues[i], ready_list) {
            /* 计算等待时间 */
            unsigned long wait_time = sched->clock_ticks - pcb->last_sched_time;
            
            /* 如果等待时间超过老化时间，提升优先级 */
            if (wait_time > sched->config.aging_time && pcb->priority > sched->config.max_priority) {
                /* 从当前队列移除 */
                list_del(&pcb->ready_list);
                
                /* 提升优先级 */
                pcb->priority--;
                if (pcb->priority < sched->config.max_priority) {
                    pcb->priority = sched->config.max_priority;
                }
                
                /* 添加到更高优先级队列 */
                int new_index = pcb->priority - 1;
                list_add_tail(&pcb->ready_list, &sched->ready_queues[new_index]);
                
                aged_count++;
                
                printk(KERN_DEBUG "scheduler_aging: process %d aged from %d to %d\n",
                       pcb->pid, pcb->priority + 1, pcb->priority);
            }
        }
    }
    
    spin_unlock_irqrestore(&sched->lock, flags);
    
    if (aged_count > 0) {
        printk(KERN_INFO "scheduler_aging: aged %d processes\n", aged_count);
    }
    
    return aged_count;
}

/* 获取调度器统计信息 */
void scheduler_get_stats(struct scheduler *sched, struct scheduler_stats *stats) {
    unsigned long flags;
    
    if (!sched || !stats) {
        return;
    }
    
    spin_lock_irqsave(&sched->lock, flags);
    memcpy(stats, &sched->stats, sizeof(struct scheduler_stats));
    spin_unlock_irqrestore(&sched->lock, flags);
}

/* 打印调度器统计信息 */
void scheduler_print_stats(struct scheduler *sched) {
    struct scheduler_stats stats;
    int i;
    
    if (!sched) {
        return;
    }
    
    scheduler_get_stats(sched, &stats);
    
    printk(KERN_INFO "========== Scheduler Statistics ==========\n");
    printk(KERN_INFO "Scheduler type: %d\n", sched->config.type);
    printk(KERN_INFO "Total schedules: %lu\n", stats.total_schedules);
    printk(KERN_INFO "Context switches: %lu\n", stats.context_switches);
    printk(KERN_INFO "Voluntary switches: %lu\n", stats.voluntary_switches);
    printk(KERN_INFO "Involuntary switches: %lu\n", stats.involuntary_switches);
    printk(KERN_INFO "Idle time: %lu ticks\n", stats.idle_time);
    printk(KERN_INFO "Busy time: %lu ticks\n", stats.busy_time);
    
    if (stats.busy_time + stats.idle_time > 0) {
        printk(KERN_INFO "CPU utilization: %.1f%%\n",
               stats.busy_time * 100.0 / (stats.busy_time + stats.idle_time));
    }
    
    printk(KERN_INFO "Throughput: %.2f processes/sec\n", stats.throughput);
    printk(KERN_INFO "Processes completed: %lu\n", stats.processes_completed);
    
    printk(KERN_INFO "Scheduling latency:\n");
    printk(KERN_INFO "  Min: %lu jiffies\n", stats.min_latency);
    printk(KERN_INFO "  Max: %lu jiffies\n", stats.max_latency);
    if (stats.total_schedules > 0) {
        printk(KERN_INFO "  Avg: %lu jiffies\n", stats.total_latency / stats.total_schedules);
    }
    
    printk(KERN_INFO "Priority distribution:\n");
    for (i = 0; i < 10; i++) {
        if (stats.priority_counts[i] > 0) {
            printk(KERN_INFO "  Priority %d: %lu schedules\n", i + 1, stats.priority_counts[i]);
        }
    }
    
    printk(KERN_INFO "==========================================\n");
}