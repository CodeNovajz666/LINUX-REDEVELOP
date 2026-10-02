#include "process.h"
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/printk.h>
#include <linux/errno.h>

/* 状态转换验证矩阵 */
static const int state_transition_matrix[6][6] = {
    /* NEW, READY, RUNNING, BLOCKED, ZOMBIE, TERMINATED */
    {0, 1, 0, 0, 0, 1},  /* NEW */
    {0, 0, 1, 1, 0, 1},  /* READY */
    {0, 1, 0, 1, 0, 1},  /* RUNNING */
    {0, 1, 0, 0, 0, 1},  /* BLOCKED */
    {0, 0, 0, 0, 0, 1},  /* ZOMBIE */
    {0, 0, 0, 0, 0, 0},  /* TERMINATED */
};

/* 验证状态转换 */
static int validate_state_transition(proc_state_t old_state, proc_state_t new_state) {
    if (old_state >= 6 || new_state >= 6) {
        return 0;
    }
    return state_transition_matrix[old_state][new_state];
}

/* 改变进程状态 */
int process_change_state(struct process_table *ptable, pid_t pid, proc_state_t new_state) {
    struct pcb *pcb;
    unsigned long flags;
    
    if (!ptable || pid <= 0) {
        return -EINVAL;
    }
    
    /* 查找进程 */
    pcb = process_table_find(ptable, pid);
    if (!pcb) {
        printk(KERN_ERR "process_change_state: PID %d not found\n", pid);
        return -ENOENT;
    }
    
    /* 验证状态转换 */
    if (!validate_state_transition(pcb->state, new_state)) {
        printk(KERN_ERR "process_change_state: invalid transition %d -> %d\n",
               pcb->state, new_state);
        return -EINVAL;
    }
    
    spin_lock_irqsave(&pcb->lock, flags);
    
    /* 记录旧状态 */
    proc_state_t old_state = pcb->state;
    
    /* 更新状态 */
    pcb->state = new_state;
    
    /* 如果是终止状态，更新统计信息 */
    if (new_state == PROC_STATE_TERMINATED) {
        pcb->time_slice = 0;
    }
    
    spin_unlock_irqrestore(&pcb->lock, flags);
    
    printk(KERN_INFO "process_change_state: PID=%d %d -> %d\n",
           pid, old_state, new_state);
    
    return 0;
}

/* 设置进程优先级 */
int process_set_priority(struct process_table *ptable, pid_t pid, int priority) {
    struct pcb *pcb;
    unsigned long flags;
    
    if (!ptable || pid <= 0) {
        return -EINVAL;
    }
    
    if (priority < 1 || priority > 10) {
        printk(KERN_ERR "process_set_priority: invalid priority %d\n", priority);
        return -EINVAL;
    }
    
    /* 查找进程 */
    pcb = process_table_find(ptable, pid);
    if (!pcb) {
        printk(KERN_ERR "process_set_priority: PID %d not found\n", pid);
        return -ENOENT;
    }
    
    spin_lock_irqsave(&pcb->lock, flags);
    
    /* 更新优先级 */
    pcb->priority = priority;
    pcb->time_slice = priority * 10;  /* 更新时间片 */
    
    spin_unlock_irqrestore(&pcb->lock, flags);
    
    printk(KERN_INFO "process_set_priority: PID=%d priority set to %d\n", pid, priority);
    return 0;
}

/* 唤醒所有阻塞进程 */
int process_wakeup_all_blocked(struct process_table *ptable) {
    struct pcb *pcb;
    struct list_head *pos;
    unsigned long flags;
    int count = 0;
    
    if (!ptable) {
        return 0;
    }
    
    spin_lock_irqsave(&ptable->lock, flags);
    
    list_for_each(pos, &ptable->all_processes) {
        pcb = list_entry(pos, struct pcb, list);
        
        if (pcb->state == PROC_STATE_BLOCKED) {
            spin_lock(&pcb->lock);
            pcb->state = PROC_STATE_READY;
            spin_unlock(&pcb->lock);
            count++;
        }
    }
    
    spin_unlock_irqrestore(&ptable->lock, flags);
    
    if (count > 0) {
        printk(KERN_INFO "process_wakeup_all_blocked: woke up %d processes\n", count);
    }
    
    return count;
}

/* 杀死所有进程 */
int process_kill_all(struct process_table *ptable) {
    struct pcb *pcb, *tmp;
    struct list_head *pos, *q;
    unsigned long flags;
    int count = 0;
    
    if (!ptable) {
        return 0;
    }
    
    spin_lock_irqsave(&ptable->lock, flags);
    
    list_for_each_safe(pos, q, &ptable->all_processes) {
        pcb = list_entry(pos, struct pcb, list);
        
        /* 跳过初始进程 */
        if (pcb->pid == 1 || pcb->ppid == 0) {
            continue;
        }
        
        list_del(&pcb->list);
        ptable->process_count--;
        
        /* 销毁PCB */
        pcb_destroy(pcb);
        count++;
    }
    
    spin_unlock_irqrestore(&ptable->lock, flags);
    
    printk(KERN_INFO "process_kill_all: killed %d processes\n", count);
    return count;
}

/* 获取进程列表 */
int process_get_list(struct process_table *ptable, struct pcb **buffer, int max_count) {
    struct pcb *pcb;
    struct list_head *pos;
    unsigned long flags;
    int count = 0;
    
    if (!ptable || !buffer || max_count <= 0) {
        return 0;
    }
    
    spin_lock_irqsave(&ptable->lock, flags);
    
    list_for_each(pos, &ptable->all_processes) {
        if (count >= max_count) {
            break;
        }
        
        pcb = list_entry(pos, struct pcb, list);
        buffer[count++] = pcb;
    }
    
    spin_unlock_irqrestore(&ptable->lock, flags);
    
    return count;
}

/* 获取就绪进程列表 */
int process_get_ready_list(struct process_table *ptable, struct pcb **buffer, int max_count) {
    struct pcb *pcb;
    struct list_head *pos;
    unsigned long flags;
    int count = 0;
    
    if (!ptable || !buffer || max_count <= 0) {
        return 0;
    }
    
    spin_lock_irqsave(&ptable->lock, flags);
    
    list_for_each(pos, &ptable->all_processes) {
        if (count >= max_count) {
            break;
        }
        
        pcb = list_entry(pos, struct pcb, list);
        
        if (pcb->state == PROC_STATE_READY) {
            buffer[count++] = pcb;
        }
    }
    
    spin_unlock_irqrestore(&ptable->lock, flags);
    
    return count;
}

/* 获取运行进程 */
int process_get_running(struct process_table *ptable, struct pcb **buffer) {
    unsigned long flags;
    
    if (!ptable || !buffer) {
        return -EINVAL;
    }
    
    spin_lock_irqsave(&ptable->lock, flags);
    
    if (ptable->current) {
        *buffer = ptable->current;
        spin_unlock_irqrestore(&ptable->lock, flags);
        return 1;
    }
    
    spin_unlock_irqrestore(&ptable->lock, flags);
    return 0;
}

/* 获取总CPU时间 */
unsigned long process_get_total_cpu_time(struct process_table *ptable) {
    struct pcb *pcb;
    struct list_head *pos;
    unsigned long flags;
    unsigned long total = 0;
    
    if (!ptable) {
        return 0;
    }
    
    spin_lock_irqsave(&ptable->lock, flags);
    
    list_for_each(pos, &ptable->all_processes) {
        pcb = list_entry(pos, struct pcb, list);
        spin_lock(&pcb->lock);
        total += pcb->cpu_time;
        spin_unlock(&pcb->lock);
    }
    
    spin_unlock_irqrestore(&ptable->lock, flags);
    
    return total;
}

/* 获取总内存使用 */
unsigned long process_get_total_memory(struct process_table *ptable) {
    struct pcb *pcb;
    struct list_head *pos;
    unsigned long flags;
    unsigned long total = 0;
    
    if (!ptable) {
        return 0;
    }
    
    spin_lock_irqsave(&ptable->lock, flags);
    
    list_for_each(pos, &ptable->all_processes) {
        pcb = list_entry(pos, struct pcb, list);
        spin_lock(&pcb->lock);
        total += pcb->memory_usage;
        spin_unlock(&pcb->lock);
    }
    
    spin_unlock_irqrestore(&ptable->lock, flags);
    
    return total;
}

/* 按状态统计进程数 */
int process_get_count_by_state(struct process_table *ptable, proc_state_t state) {
    struct pcb *pcb;
    struct list_head *pos;
    unsigned long flags;
    int count = 0;
    
    if (!ptable) {
        return 0;
    }
    
    spin_lock_irqsave(&ptable->lock, flags);
    
    list_for_each(pos, &ptable->all_processes) {
        pcb = list_entry(pos, struct pcb, list);
        
        if (pcb->state == state) {
            count++;
        }
    }
    
    spin_unlock_irqrestore(&ptable->lock, flags);
    
    return count;
}

/* 添加子进程关系 */
int process_add_child(struct process_table *ptable, pid_t parent_pid, pid_t child_pid) {
    struct pcb *parent, *child;
    unsigned long flags;
    
    if (!ptable || parent_pid <= 0 || child_pid <= 0) {
        return -EINVAL;
    }
    
    /* 查找父进程 */
    parent = process_table_find(ptable, parent_pid);
    if (!parent) {
        printk(KERN_ERR "process_add_child: parent PID %d not found\n", parent_pid);
        return -ENOENT;
    }
    
    /* 查找子进程 */
    child = process_table_find(ptable, child_pid);
    if (!child) {
        printk(KERN_ERR "process_add_child: child PID %d not found\n", child_pid);
        return -ENOENT;
    }
    
    spin_lock_irqsave(&parent->lock, flags);
    
    /* 设置父子关系 */
    child->parent = parent;
    child->ppid = parent_pid;
    
    /* 添加到子进程列表 */
    list_add_tail(&child->sibling, &parent->children);
    parent->child_count++;
    
    spin_unlock_irqrestore(&parent->lock, flags);
    
    printk(KERN_INFO "process_add_child: PID=%d added child PID=%d\n",
           parent_pid, child_pid);
    
    return 0;
}