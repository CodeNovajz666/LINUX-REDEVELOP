#include "pcb.h"
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/spinlock.h>
#include <linux/jiffies.h>
#include <linux/printk.h>
#include <linux/errno.h>

/* 全局进程表实例 */
static struct process_table g_proc_table;

/* 内部函数声明 */
static pid_t allocate_pid(void);
static const char *state_to_string(proc_state_t state);

/* 创建新的PCB */
struct pcb *pcb_create(const char *name, int priority, pid_t ppid) {
    struct pcb *new_pcb;
    
    /* 参数验证 */
    if (!name || priority < 1 || priority > 10) {
        printk(KERN_ERR "pcb_create: invalid parameters\n");
        return NULL;
    }
    
    /* 分配PCB内存 */
    new_pcb = kmalloc(sizeof(struct pcb), GFP_KERNEL);
    if (!new_pcb) {
        printk(KERN_ERR "pcb_create: kmalloc failed\n");
        return NULL;
    }
    memset(new_pcb, 0, sizeof(struct pcb));
    
    /* 初始化PCB字段 */
    new_pcb->pid = allocate_pid();
    new_pcb->ppid = ppid;
    strncpy(new_pcb->name, name, sizeof(new_pcb->name) - 1);
    new_pcb->name[sizeof(new_pcb->name) - 1] = '\0';
    
    new_pcb->state = PROC_STATE_NEW;
    new_pcb->priority = priority;
    new_pcb->original_priority = priority;
    new_pcb->time_slice = priority * 10;
    
    new_pcb->create_time = jiffies;
    new_pcb->cpu_time = 0;
    new_pcb->context_switches = 0;
    new_pcb->io_operations = 0;
    new_pcb->memory_usage = 4096;  /* 默认4KB */
    
    /* 初始化链表 */
    INIT_LIST_HEAD(&new_pcb->list);
    INIT_LIST_HEAD(&new_pcb->ready_list);
    INIT_LIST_HEAD(&new_pcb->children);
    INIT_LIST_HEAD(&new_pcb->sibling);
    
    /* 初始化锁 */
    spin_lock_init(&new_pcb->lock);
    
    /* 初始化上下文 */
    for (int i = 0; i < 16; i++) {
        new_pcb->regs[i] = 0;
    }
    new_pcb->pc = 0;
    new_pcb->sp = 0x7FFFFFFF;
    
    /* 初始化内存管理 */
    new_pcb->memory = kmalloc(sizeof(struct process_memory), GFP_KERNEL);
    if (new_pcb->memory) {
        process_memory_init(new_pcb->memory, new_pcb->pid);
    }
    
    /* 初始化文件描述符表 */
    new_pcb->fdt = kmalloc(sizeof(struct file_descriptor_table), GFP_KERNEL);
    if (new_pcb->fdt) {
        file_descriptor_table_init(new_pcb->fdt);
    }
    
    /* 初始化信号处理结构 */
    new_pcb->signal = kmalloc(sizeof(struct process_signal), GFP_KERNEL);
    if (new_pcb->signal) {
        process_signal_init(new_pcb->signal, new_pcb->pid);
    }
    
    /* 初始化安全管理结构 */
    new_pcb->security = kmalloc(sizeof(struct process_security), GFP_KERNEL);
    if (new_pcb->security) {
        process_security_init(new_pcb->security, new_pcb->pid);
        new_pcb->private_data = new_pcb->security;
    }
    
    printk(KERN_INFO "pcb_create: created process '%s' (PID=%d, priority=%d)\n",
           name, new_pcb->pid, priority);
    
    return new_pcb;
}

/* 销毁PCB */
void pcb_destroy(struct pcb *pcb) {
    if (!pcb) {
        return;
    }
    
    /* 清理内存管理 */
    if (pcb->memory) {
        process_memory_cleanup(pcb->memory);
        kfree(pcb->memory);
    }
    
    /* 清理文件描述符表 */
    if (pcb->fdt) {
        file_descriptor_table_cleanup(pcb->fdt);
        kfree(pcb->fdt);
    }
    
    /* 清理信号处理结构 */
    if (pcb->signal) {
        process_signal_cleanup(pcb->signal);
        kfree(pcb->signal);
    }
    
    /* 清理安全管理结构 */
    if (pcb->security) {
        process_security_cleanup(pcb->security);
        kfree(pcb->security);
    }
    
    printk(KERN_INFO "pcb_destroy: destroying process '%s' (PID=%d)\n",
           pcb->name, pcb->pid);
    
    kfree(pcb);
}

/* 根据PID查找PCB */
struct pcb *pcb_find_by_pid(pid_t pid) {
    return process_table_find(&g_proc_table, pid);
}

/* 设置进程状态 */
void pcb_set_state(struct pcb *pcb, proc_state_t new_state) {
    unsigned long flags;
    
    if (!pcb) {
        return;
    }
    
    spin_lock_irqsave(&pcb->lock, flags);
    
    printk(KERN_DEBUG "pcb_set_state: PID=%d %s -> %s\n",
           pcb->pid, state_to_string(pcb->state), state_to_string(new_state));
    
    pcb->state = new_state;
    
    spin_unlock_irqrestore(&pcb->lock, flags);
}

/* 更新进程统计信息 */
void pcb_update_stats(struct pcb *pcb, unsigned long cpu_cycles) {
    unsigned long flags;
    
    if (!pcb) {
        return;
    }
    
    spin_lock_irqsave(&pcb->lock, flags);
    pcb->cpu_time += cpu_cycles;
    
    if (pcb->time_slice > 0) {
        pcb->time_slice--;
    }
    
    spin_unlock_irqrestore(&pcb->lock, flags);
}

/* 打印PCB信息 */
void pcb_dump_info(struct pcb *pcb) {
    if (!pcb) {
        printk(KERN_INFO "pcb_dump_info: NULL PCB\n");
        return;
    }
    
    printk(KERN_INFO "=== PCB Info (PID=%d) ===\n", pcb->pid);
    printk(KERN_INFO "  Name: %s\n", pcb->name);
    printk(KERN_INFO "  State: %s\n", state_to_string(pcb->state));
    printk(KERN_INFO "  Priority: %d (original: %d)\n", pcb->priority, pcb->original_priority);
    printk(KERN_INFO "  Parent PID: %d\n", pcb->ppid);
    printk(KERN_INFO "  CPU Time: %lu\n", pcb->cpu_time);
    printk(KERN_INFO "  Memory: %lu bytes\n", pcb->memory_usage);
    if (pcb->memory) {
        printk(KERN_INFO "  Managed Memory: %lu bytes\n", process_memory_get_usage(pcb->memory));
    }
    printk(KERN_INFO "  Created: %lu jiffies ago\n", jiffies - pcb->create_time);
    printk(KERN_INFO "  Children: %d\n", pcb->child_count);
    printk(KERN_INFO "============================\n");
}

/* 验证PCB完整性 */
int pcb_validate(struct pcb *pcb) {
    if (!pcb) {
        return -EINVAL;
    }
    
    if (pcb->pid <= 0) {
        return -EINVAL;
    }
    
    if (pcb->priority < 1 || pcb->priority > 10) {
        return -EINVAL;
    }
    
    if (strlen(pcb->name) == 0 || strlen(pcb->name) >= sizeof(pcb->name)) {
        return -EINVAL;
    }
    
    return 0;
}

/* 内部函数实现 */

/* 分配新的PID */
static pid_t allocate_pid(void) {
    static pid_t next_pid = 1000;
    unsigned long flags;
    pid_t pid;
    
    spin_lock_irqsave(&g_proc_table.lock, flags);
    pid = next_pid++;
    
    /* 简单的PID回绕处理 */
    if (next_pid > 65535) {
        next_pid = 1000;
    }
    
    spin_unlock_irqrestore(&g_proc_table.lock, flags);
    return pid;
}

/* 状态转字符串 */
static const char *state_to_string(proc_state_t state) {
    switch (state) {
        case PROC_STATE_NEW:        return "NEW";
        case PROC_STATE_READY:      return "READY";
        case PROC_STATE_RUNNING:    return "RUNNING";
        case PROC_STATE_BLOCKED:    return "BLOCKED";
        case PROC_STATE_ZOMBIE:     return "ZOMBIE";
        case PROC_STATE_TERMINATED: return "TERMINATED";
        default:                    return "UNKNOWN";
    }
}

/* 进程表初始化 */
int process_table_init(struct process_table *ptable) {
    int i;
    
    if (!ptable) {
        return -EINVAL;
    }
    
    INIT_LIST_HEAD(&ptable->all_processes);
    INIT_LIST_HEAD(&ptable->blocked_queue);
    
    for (i = 0; i < 10; i++) {
        INIT_LIST_HEAD(&ptable->ready_queues[i]);
    }
    
    ptable->current = NULL;
    spin_lock_init(&ptable->lock);
    ptable->next_pid = 1000;
    ptable->process_count = 0;
    
    printk(KERN_INFO "process_table_init: process table initialized\n");
    return 0;
}

/* 清理进程表 */
void process_table_cleanup(struct process_table *ptable) {
    struct pcb *pcb, *tmp;
    struct list_head *pos, *q;
    unsigned long flags;
    
    if (!ptable) {
        return;
    }
    
    spin_lock_irqsave(&ptable->lock, flags);
    
    /* 安全遍历并释放所有PCB */
    list_for_each_safe(pos, q, &ptable->all_processes) {
        pcb = list_entry(pos, struct pcb, list);
        list_del(&pcb->list);
        pcb_destroy(pcb);
    }
    
    ptable->process_count = 0;
    ptable->current = NULL;
    
    spin_unlock_irqrestore(&ptable->lock, flags);
    
    printk(KERN_INFO "process_table_cleanup: cleaned up all processes\n");
}

/* 将PCB添加到进程表 */
int process_table_add(struct process_table *ptable, struct pcb *pcb) {
    unsigned long flags;
    
    if (!ptable || !pcb) {
        return -EINVAL;
    }
    
    /* 验证PCB */
    if (pcb_validate(pcb) != 0) {
        printk(KERN_ERR "process_table_add: invalid PCB\n");
        return -EINVAL;
    }
    
    spin_lock_irqsave(&ptable->lock, flags);
    
    /* 检查PID是否已存在 */
    if (process_table_find(ptable, pcb->pid) != NULL) {
        spin_unlock_irqrestore(&ptable->lock, flags);
        printk(KERN_ERR "process_table_add: PID %d already exists\n", pcb->pid);
        return -EEXIST;
    }
    
    /* 添加到所有进程链表 */
    list_add_tail(&pcb->list, &ptable->all_processes);
    ptable->process_count++;
    
    spin_unlock_irqrestore(&ptable->lock, flags);
    
    printk(KERN_INFO "process_table_add: added process '%s' (PID=%d)\n",
           pcb->name, pcb->pid);
    return 0;
}

/* 从进程表移除PCB */
int process_table_remove(struct process_table *ptable, pid_t pid) {
    struct pcb *pcb;
    unsigned long flags;
    
    if (!ptable || pid <= 0) {
        return -EINVAL;
    }
    
    spin_lock_irqsave(&ptable->lock, flags);
    
    pcb = process_table_find(ptable, pid);
    if (!pcb) {
        spin_unlock_irqrestore(&ptable->lock, flags);
        printk(KERN_ERR "process_table_remove: PID %d not found\n", pid);
        return -ENOENT;
    }
    
    /* 从所有链表中移除 */
    list_del(&pcb->list);
    
    /* 如果移除的是当前运行进程，清空current指针 */
    if (ptable->current == pcb) {
        ptable->current = NULL;
    }
    
    ptable->process_count--;
    
    spin_unlock_irqrestore(&ptable->lock, flags);
    
    /* 销毁PCB */
    pcb_destroy(pcb);
    
    printk(KERN_INFO "process_table_remove: removed process PID=%d\n", pid);
    return 0;
}

/* 查找PCB */
struct pcb *process_table_find(struct process_table *ptable, pid_t pid) {
    struct pcb *pcb;
    struct list_head *pos;
    unsigned long flags;
    
    if (!ptable || pid <= 0) {
        return NULL;
    }
    
    spin_lock_irqsave(&ptable->lock, flags);
    
    list_for_each(pos, &ptable->all_processes) {
        pcb = list_entry(pos, struct pcb, list);
        if (pcb->pid == pid) {
            spin_unlock_irqrestore(&ptable->lock, flags);
            return pcb;
        }
    }
    
    spin_unlock_irqrestore(&ptable->lock, flags);
    return NULL;
}

/* 获取进程数量 */
int process_table_count(struct process_table *ptable) {
    int count;
    unsigned long flags;
    
    if (!ptable) {
        return 0;
    }
    
    spin_lock_irqsave(&ptable->lock, flags);
    count = ptable->process_count;
    spin_unlock_irqrestore(&ptable->lock, flags);
    
    return count;
}