#include "scheduler.h"
#include <linux/spinlock.h>
#include <linux/printk.h>
#include <linux/jiffies.h>

/* 保存上下文 */
static void save_context(struct pcb *pcb) {
    if (!pcb) {
        return;
    }
    
    /* 模拟保存寄存器状态 */
    for (int i = 0; i < 16; i++) {
        pcb->regs[i] = pcb->pid * 1000 + i + pcb->cpu_time;  /* 模拟值 */
    }
    
    pcb->pc = pcb->pid * 10000 + pcb->cpu_time;
    pcb->sp = 0x7FFFFFFF - pcb->pid * 0x1000;
    
    /* 更新上下文切换计数 */
    pcb->context_switches++;
    
    printk(KERN_DEBUG "save_context: saved context for PID=%d\n", pcb->pid);
}

/* 恢复上下文 */
static void restore_context(struct pcb *pcb) {
    if (!pcb) {
        return;
    }
    
    printk(KERN_DEBUG "restore_context: restored context for PID=%d\n", pcb->pid);
}

/* 执行上下文切换 */
int scheduler_context_switch(struct scheduler *sched, struct pcb *new_process) {
    struct pcb *old_process;
    unsigned long flags;
    
    if (!sched || !sched->initialized) {
        return -EINVAL;
    }
    
    spin_lock_irqsave(&sched->lock, flags);
    
    old_process = sched->current_process;
    
    /* 如果新旧进程相同，不需要切换 */
    if (old_process == new_process) {
        spin_unlock_irqrestore(&sched->lock, flags);
        return 0;
    }
    
    /* 保存旧进程上下文 */
    if (old_process) {
        save_context(old_process);
        
        /* 如果进程还没结束，放回就绪队列 */
        if (old_process->state == PROC_STATE_RUNNING) {
            old_process->state = PROC_STATE_READY;
            scheduler_enqueue(sched, old_process);
            old_process->voluntary_switches++;
            sched->stats.voluntary_switches++;
        }
    }
    
    /* 设置新运行进程 */
    sched->current_process = new_process;
    
    /* 恢复新进程上下文 */
    if (new_process) {
        restore_context(new_process);
        
        /* 更新进程状态 */
        if (new_process->state == PROC_STATE_READY) {
            new_process->state = PROC_STATE_RUNNING;
        }
        
        new_process->last_sched_time = sched->clock_ticks;
        
        printk(KERN_DEBUG "scheduler_context_switch: switched to PID=%d\n", new_process->pid);
    } else {
        printk(KERN_DEBUG "scheduler_context_switch: no process to run (idle)\n");
    }
    
    /* 更新统计信息 */
    sched->stats.context_switches++;
    
    if (old_process) {
        if (old_process->state == PROC_STATE_READY) {
            old_process->involuntary_switches++;
            sched->stats.involuntary_switches++;
        }
    }
    
    spin_unlock_irqrestore(&sched->lock, flags);
    
    return 0;
}

/* 设置调度器类型 */
int scheduler_set_type(struct scheduler *sched, scheduler_type_t type) {
    unsigned long flags;
    
    if (!sched || !sched->initialized) {
        return -EINVAL;
    }
    
    if (type >= SCHED_MAX) {
        return -EINVAL;
    }
    
    spin_lock_irqsave(&sched->lock, flags);
    
    /* 如果类型改变，清空统计信息 */
    if (sched->config.type != type) {
        memset(&sched->stats, 0, sizeof(struct scheduler_stats));
        sched->stats.min_latency = (unsigned long)-1;
    }
    
    sched->config.type = type;
    
    spin_unlock_irqrestore(&sched->lock, flags);
    
    printk(KERN_INFO "scheduler_set_type: scheduler type set to %d\n", type);
    return 0;
}

/* 获取调度器类型 */
scheduler_type_t scheduler_get_type(struct scheduler *sched) {
    scheduler_type_t type;
    unsigned long flags;
    
    if (!sched || !sched->initialized) {
        return SCHED_PRIORITY;
    }
    
    spin_lock_irqsave(&sched->lock, flags);
    type = sched->config.type;
    spin_unlock_irqrestore(&sched->lock, flags);
    
    return type;
}

/* 重置调度器统计 */
void scheduler_reset_stats(struct scheduler *sched) {
    unsigned long flags;
    
    if (!sched || !sched->initialized) {
        return;
    }
    
    spin_lock_irqsave(&sched->lock, flags);
    
    memset(&sched->stats, 0, sizeof(struct scheduler_stats));
    sched->stats.min_latency = (unsigned long)-1;
    
    spin_unlock_irqrestore(&sched->lock, flags);
    
    printk(KERN_INFO "scheduler_reset_stats: statistics reset\n");
}