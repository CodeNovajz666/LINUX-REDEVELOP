#include "resource.h"
#include <linux/spinlock.h>
#include <linux/printk.h>
#include <linux/jiffies.h>

/* 更新CPU时间 */
void resource_update_cpu(struct pcb *pcb, unsigned long cycles) {
    unsigned long flags;
    
    if (!pcb) {
        return;
    }
    
    spin_lock_irqsave(&pcb->lock, flags);
    pcb->cpu_time += cycles;
    spin_unlock_irqrestore(&pcb->lock, flags);
}

/* 更新内存使用 */
void resource_update_memory(struct pcb *pcb, unsigned long memory) {
    unsigned long flags;
    
    if (!pcb) {
        return;
    }
    
    spin_lock_irqsave(&pcb->lock, flags);
    pcb->memory_usage += memory;
    spin_unlock_irqrestore(&pcb->lock, flags);
}

/* 更新IO操作数 */
void resource_update_io(struct pcb *pcb, unsigned long io_count) {
    unsigned long flags;
    
    if (!pcb) {
        return;
    }
    
    spin_lock_irqsave(&pcb->lock, flags);
    pcb->io_operations += io_count;
    spin_unlock_irqrestore(&pcb->lock, flags);
}

/* 更新上下文切换 */
void resource_update_context_switch(struct pcb *pcb) {
    unsigned long flags;
    
    if (!pcb) {
        return;
    }
    
    spin_lock_irqsave(&pcb->lock, flags);
    pcb->context_switches++;
    spin_unlock_irqrestore(&pcb->lock, flags);
}

/* 获取资源统计 */
void resource_get_stats(struct process_table *ptable, struct resource_stats *stats) {
    struct pcb *pcb;
    struct list_head *pos;
    unsigned long flags;
    
    if (!ptable || !stats) {
        return;
    }
    
    /* 初始化统计 */
    memset(stats, 0, sizeof(struct resource_stats));
    
    spin_lock_irqsave(&ptable->lock, flags);
    
    list_for_each(pos, &ptable->all_processes) {
        pcb = list_entry(pos, struct pcb, list);
        
        spin_lock(&pcb->lock);
        
        stats->total_cpu_time += pcb->cpu_time;
        stats->total_memory += pcb->memory_usage;
        stats->total_io_operations += pcb->io_operations;
        stats->total_context_switches += pcb->context_switches;
        
        switch (pcb->state) {
            case PROC_STATE_RUNNING:
                stats->running_count++;
                break;
            case PROC_STATE_READY:
                stats->ready_count++;
                break;
            case PROC_STATE_BLOCKED:
                stats->blocked_count++;
                break;
            case PROC_STATE_ZOMBIE:
                stats->zombie_count++;
                break;
            default:
                break;
        }
        
        stats->process_count++;
        spin_unlock(&pcb->lock);
    }
    
    spin_unlock_irqrestore(&ptable->lock, flags);
}

/* 打印资源统计 */
void resource_print_stats(struct process_table *ptable) {
    struct resource_stats stats;
    
    if (!ptable) {
        return;
    }
    
    resource_get_stats(ptable, &stats);
    
    printk(KERN_INFO "========== Resource Statistics ==========\n");
    printk(KERN_INFO "Total processes: %d\n", stats.process_count);
    printk(KERN_INFO "Running: %d, Ready: %d, Blocked: %d, Zombie: %d\n",
           stats.running_count, stats.ready_count,
           stats.blocked_count, stats.zombie_count);
    printk(KERN_INFO "Total CPU time: %lu jiffies\n", stats.total_cpu_time);
    printk(KERN_INFO "Total memory usage: %lu bytes\n", stats.total_memory);
    printk(KERN_INFO "Total I/O operations: %lu\n", stats.total_io_operations);
    printk(KERN_INFO "Total context switches: %lu\n", stats.total_context_switches);
    
    if (stats.process_count > 0) {
        printk(KERN_INFO "Average CPU time per process: %lu jiffies\n",
               stats.total_cpu_time / stats.process_count);
        printk(KERN_INFO "Average memory per process: %lu bytes\n",
               stats.total_memory / stats.process_count);
    }
    
    printk(KERN_INFO "=========================================\n");
}

/* 计算进程资源使用率 */
unsigned long resource_calculate_usage(struct pcb *pcb) {
    unsigned long usage = 0;
    unsigned long flags;
    
    if (!pcb) {
        return 0;
    }
    
    spin_lock_irqsave(&pcb->lock, flags);
    
    /* 综合计算资源使用率（简化版） */
    usage = pcb->cpu_time / 1000;                     /* CPU时间权重 */
    usage += pcb->memory_usage / 1024;                /* 内存权重 */
    usage += pcb->io_operations * 10;                 /* IO权重 */
    usage += pcb->context_switches * 5;               /* 上下文切换权重 */
    
    /* 考虑进程年龄（越老权重越低） */
    unsigned long age = jiffies - pcb->create_time;
    if (age > 0) {
        usage = usage * 1000 / age;  /* 归一化 */
    }
    
    spin_unlock_irqrestore(&pcb->lock, flags);
    
    return usage;
}

/* 重置进程统计 */
void resource_reset_stats(struct pcb *pcb) {
    unsigned long flags;
    
    if (!pcb) {
        return;
    }
    
    spin_lock_irqsave(&pcb->lock, flags);
    
    pcb->cpu_time = 0;
    pcb->memory_usage = 4096;  /* 重置为默认值 */
    pcb->io_operations = 0;
    pcb->context_switches = 0;
    pcb->voluntary_switches = 0;
    pcb->involuntary_switches = 0;
    
    spin_unlock_irqrestore(&pcb->lock, flags);
    
    printk(KERN_INFO "resource_reset_stats: reset stats for PID=%d\n", pcb->pid);
}