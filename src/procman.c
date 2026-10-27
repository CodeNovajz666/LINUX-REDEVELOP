/*
 * procman.c - Simple Process Manager Kernel Module
 * 完整的主模块实现，包含ioctl处理、调度器集成等
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/spinlock.h>
#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

#include "pcb.h"
#include "process.h"
#include "scheduler.h"
#include "resource.h"
#include "error.h"
#include "procman.h"

/* 模块信息 */
MODULE_LICENSE("GPL");
MODULE_AUTHOR("CodeNovajz666");
MODULE_DESCRIPTION("Simple Process Manager with Priority Scheduling");
MODULE_VERSION("1.0.0");

/* 全局变量 */
static int major_number = 0;
static struct class* procman_class = NULL;
static struct device* procman_device = NULL;
static struct cdev procman_cdev;
static struct proc_dir_entry *proc_entry = NULL;

/* 全局进程表和调度器 */
static struct process_table g_proc_table;
static struct scheduler g_scheduler;

/* 文件操作结构 */
static struct file_operations procman_fops = {
    .owner = THIS_MODULE,
    .open = procman_open,
    .release = procman_release,
    .unlocked_ioctl = procman_ioctl,
    .compat_ioctl = procman_ioctl,
};

/* /proc文件系统操作 */
static int procman_proc_show(struct seq_file *m, void *v) {
    struct pcb *pcb;
    struct list_head *pos;
    unsigned long flags;
    struct resource_stats rstats;
    struct scheduler_stats sstats;
    
    seq_printf(m, "=== Process Manager Status ===\n\n");
    
    /* 资源统计 */
    resource_get_stats(&g_proc_table, &rstats);
    seq_printf(m, "Resource Statistics:\n");
    seq_printf(m, "  Total processes: %d\n", rstats.process_count);
    seq_printf(m, "  Running: %d, Ready: %d, Blocked: %d, Zombie: %d\n",
               rstats.running_count, rstats.ready_count,
               rstats.blocked_count, rstats.zombie_count);
    seq_printf(m, "  Total CPU time: %lu jiffies\n", rstats.total_cpu_time);
    seq_printf(m, "  Total memory: %lu bytes\n", rstats.total_memory);
    seq_printf(m, "  Total I/O operations: %lu\n", rstats.total_io_operations);
    seq_printf(m, "\n");
    
    /* 调度器统计 */
    scheduler_get_stats(&g_scheduler, &sstats);
    const char *policy_name = "Unknown";
    switch (scheduler_get_type(&g_scheduler)) {
        case SCHED_PRIORITY: policy_name = "Priority"; break;
        case SCHED_ROUND_ROBIN: policy_name = "Round Robin"; break;
        case SCHED_FCFS: policy_name = "FCFS"; break;
    }
    
    seq_printf(m, "Scheduler Statistics (%s):\n", policy_name);
    seq_printf(m, "  Total schedules: %lu\n", sstats.total_schedules);
    seq_printf(m, "  Context switches: %lu\n", sstats.context_switches);
    seq_printf(m, "  CPU utilization: %.1f%%\n",
               sstats.busy_time * 100.0 / (sstats.busy_time + sstats.idle_time));
    seq_printf(m, "  Throughput: %.2f processes/sec\n", sstats.throughput);
    seq_printf(m, "\n");
    
    /* 进程列表 */
    seq_printf(m, "Process List:\n");
    seq_printf(m, "PID\tPPID\tName\t\tState\t\tPriority\tCPU Time\tMemory\tChildren\n");
    seq_printf(m, "--------------------------------------------------------------------------------\n");
    
    spin_lock_irqsave(&g_proc_table.lock, flags);
    
    list_for_each(pos, &g_proc_table.all_processes) {
        pcb = list_entry(pos, struct pcb, list);
        
        const char *state_str;
        switch (pcb->state) {
            case PROC_STATE_NEW: state_str = "NEW"; break;
            case PROC_STATE_READY: state_str = "READY"; break;
            case PROC_STATE_RUNNING: state_str = "RUNNING"; break;
            case PROC_STATE_BLOCKED: state_str = "BLOCKED"; break;
            case PROC_STATE_ZOMBIE: state_str = "ZOMBIE"; break;
            case PROC_STATE_TERMINATED: state_str = "TERMINATED"; break;
            default: state_str = "UNKNOWN"; break;
        }
        
        seq_printf(m, "%d\t%d\t%-12s\t%-10s\t%d\t\t%lu\t\t%lu\t%d\n",
                   pcb->pid, pcb->ppid, pcb->name, state_str,
                   pcb->priority, pcb->cpu_time, pcb->memory_usage, pcb->child_count);
    }
    
    spin_unlock_irqrestore(&g_proc_table.lock, flags);
    
    seq_printf(m, "\n=== End of Report ===\n");
    return 0;
}

static int procman_proc_open(struct inode *inode, struct file *file) {
    return single_open(file, procman_proc_show, NULL);
}

static const struct proc_ops procman_proc_fops = {
    .proc_open = procman_proc_open,
    .proc_read = seq_read,
    .proc_lseek = seq_lseek,
    .proc_release = single_release,
};

/* 文件操作函数 */
int procman_open(struct inode *inodep, struct file *filep) {
    printk(KERN_INFO "procman: Device opened\n");
    return 0;
}

int procman_release(struct inode *inodep, struct file *filep) {
    printk(KERN_INFO "procman: Device closed\n");
    return 0;
}

/* 内部处理函数 */

/* 创建进程处理 */
static int handle_create_proc(struct proc_create_req __user *u_req) {
    struct proc_create_req k_req;
    struct pcb *pcb;
    int ret;
    
    /* 从用户空间复制数据 */
    if (copy_from_user(&k_req, u_req, sizeof(struct proc_create_req))) {
        printk(KERN_ERR "handle_create_proc: copy_from_user failed\n");
        return -EFAULT;
    }
    
    /* 参数验证 */
    if ((ret = validate_process_name(k_req.name)) != PROC_SUCCESS) {
        printk(KERN_ERR "handle_create_proc: %s\n", procman_strerror(ret));
        return ret;
    }
    
    if ((ret = validate_process_priority(k_req.priority)) != PROC_SUCCESS) {
        printk(KERN_ERR "handle_create_proc: %s\n", procman_strerror(ret));
        return ret;
    }
    
    /* 检查容量 */
    if ((ret = check_process_table_capacity()) != PROC_SUCCESS) {
        printk(KERN_ERR "handle_create_proc: %s\n", procman_strerror(ret));
        return ret;
    }
    
    /* 创建PCB */
    pcb = pcb_create(k_req.name, k_req.priority, k_req.parent_pid);
    if (!pcb) {
        printk(KERN_ERR "handle_create_proc: pcb_create failed\n");
        return PROC_ERR_NOMEM;
    }
    
    /* 设置初始状态 */
    pcb_set_state(pcb, PROC_STATE_READY);
    
    /* 添加到进程表 */
    ret = process_table_add(&g_proc_table, pcb);
    if (ret != 0) {
        pcb_destroy(pcb);
        return ret;
    }
    
    /* 添加到调度器就绪队列 */
    scheduler_enqueue(&g_scheduler, pcb);
    
    /* 设置父子关系（如果有父进程） */
    if (k_req.parent_pid > 0) {
        struct pcb *parent = process_table_find(&g_proc_table, k_req.parent_pid);
        if (parent) {
            process_add_child(&g_proc_table, k_req.parent_pid, pcb->pid);
        }
    }
    
    printk(KERN_INFO "handle_create_proc: created process '%s' (PID=%d)\n",
           k_req.name, pcb->pid);
    
    return pcb->pid;
}

/* 销毁进程处理 */
static int handle_destroy_proc(pid_t __user *u_pid) {
    pid_t pid;
    
    /* 从用户空间获取PID */
    if (copy_from_user(&pid, u_pid, sizeof(pid_t))) {
        printk(KERN_ERR "handle_destroy_proc: copy_from_user failed\n");
        return -EFAULT;
    }
    
    if ((validate_pid(pid)) != PROC_SUCCESS) {
        printk(KERN_ERR "handle_destroy_proc: invalid PID %d\n", pid);
        return PROC_ERR_INVALID_PID;
    }
    
    /* 从调度器队列移除 */
    struct pcb *pcb = process_table_find(&g_proc_table, pid);
    if (pcb) {
        scheduler_dequeue(&g_scheduler, pcb);
    }
    
    /* 从进程表移除 */
    int ret = process_table_remove(&g_proc_table, pid);
    if (ret != 0) {
        printk(KERN_ERR "handle_destroy_proc: failed to remove PID %d\n", pid);
        return ret;
    }
    
    printk(KERN_INFO "handle_destroy_proc: destroyed process PID=%d\n", pid);
    return 0;
}

/* 进程列表处理 */
static int handle_list_procs(struct proc_list_req __user *u_req) {
    struct proc_list_req k_req;
    struct proc_info_resp *proc_buf = NULL;
    struct pcb *pcb;
    struct list_head *pos;
    unsigned long flags;
    int i = 0;
    
    /* 从用户空间获取请求头 */
    if (copy_from_user(&k_req, u_req, sizeof(struct proc_list_req))) {
        printk(KERN_ERR "handle_list_procs: copy_from_user failed\n");
        return -EFAULT;
    }
    
    if (k_req.max_count <= 0) {
        printk(KERN_ERR "handle_list_procs: invalid max_count %d\n", k_req.max_count);
        return -EINVAL;
    }
    
    /* 分配内核缓冲区 */
    proc_buf = kmalloc(sizeof(struct proc_info_resp) * k_req.max_count, GFP_KERNEL);
    if (!proc_buf) {
        printk(KERN_ERR "handle_list_procs: kmalloc failed\n");
        return -ENOMEM;
    }
    
    spin_lock_irqsave(&g_proc_table.lock, flags);
    
    /* 遍历进程表 */
    list_for_each(pos, &g_proc_table.all_processes) {
        if (i >= k_req.max_count) {
            break;
        }
        
        pcb = list_entry(pos, struct pcb, list);
        
        /* 填充进程信息 */
        proc_buf[i].pid = pcb->pid;
        proc_buf[i].ppid = pcb->ppid;
        strncpy(proc_buf[i].name, pcb->name, sizeof(proc_buf[i].name) - 1);
        proc_buf[i].name[sizeof(proc_buf[i].name) - 1] = '\0';
        proc_buf[i].state = pcb->state;
        proc_buf[i].priority = pcb->priority;
        proc_buf[i].cpu_time = pcb->cpu_time;
        proc_buf[i].memory_usage = pcb->memory_usage;
        proc_buf[i].io_operations = pcb->io_operations;
        proc_buf[i].context_switches = pcb->context_switches;
        proc_buf[i].create_time = pcb->create_time;
        
        i++;
    }
    
    spin_unlock_irqrestore(&g_proc_table.lock, flags);
    
    /* 复制回用户空间 */
    if (copy_to_user(u_req->procs, proc_buf, 
                     sizeof(struct proc_info_resp) * i)) {
        kfree(proc_buf);
        return -EFAULT;
    }
    
    /* 更新计数 */
    k_req.count = i;
    if (copy_to_user(&u_req->count, &k_req.count, sizeof(int))) {
        kfree(proc_buf);
        return -EFAULT;
    }
    
    kfree(proc_buf);
    return 0;
}

/* 获取进程计数 */
static int handle_get_proc_count(int __user *u_count) {
    int count = process_table_count(&g_proc_table);
    
    if (copy_to_user(u_count, &count, sizeof(int))) {
        return -EFAULT;
    }
    
    return 0;
}

/* 设置进程状态 */
static int handle_set_state(struct set_state_req __user *u_req) {
    struct set_state_req k_req;
    int ret;
    
    if (copy_from_user(&k_req, u_req, sizeof(struct set_state_req))) {
        return -EFAULT;
    }
    
    if ((ret = validate_pid(k_req.pid)) != PROC_SUCCESS) {
        return ret;
    }
    
    if (k_req.new_state < 0 || k_req.new_state >= 6) {
        return PROC_ERR_INVALID_STATE;
    }
    
    ret = process_change_state(&g_proc_table, k_req.pid, k_req.new_state);
    if (ret == 0) {
        /* 如果状态变为就绪，加入调度器队列 */
        if (k_req.new_state == PROC_STATE_READY) {
            struct pcb *pcb = process_table_find(&g_proc_table, k_req.pid);
            if (pcb) {
                scheduler_enqueue(&g_scheduler, pcb);
            }
        }
    }
    
    return ret;
}

/* 设置优先级 */
static int handle_set_priority(struct set_priority_req __user *u_req) {
    struct set_priority_req k_req;
    int ret;
    
    if (copy_from_user(&k_req, u_req, sizeof(struct set_priority_req))) {
        return -EFAULT;
    }
    
    if ((ret = validate_pid(k_req.pid)) != PROC_SUCCESS) {
        return ret;
    }
    
    if ((ret = validate_process_priority(k_req.priority)) != PROC_SUCCESS) {
        return ret;
    }
    
    ret = process_set_priority(&g_proc_table, k_req.pid, k_req.priority);
    if (ret == 0) {
        /* 如果进程在就绪队列中，需要重新入队 */
        struct pcb *pcb = process_table_find(&g_proc_table, k_req.pid);
        if (pcb && pcb->state == PROC_STATE_READY) {
            scheduler_dequeue(&g_scheduler, pcb);
            scheduler_enqueue(&g_scheduler, pcb);
        }
    }
    
    return ret;
}

/* 调度处理 */
static int handle_schedule(int __user *u_pid) {
    struct pcb *next_process = scheduler_schedule(&g_scheduler);
    
    if (next_process) {
        /* 执行上下文切换 */
        scheduler_context_switch(&g_scheduler, next_process);
        
        if (u_pid && copy_to_user(u_pid, &next_process->pid, sizeof(int))) {
            return -EFAULT;
        }
        
        return next_process->pid;
    } else {
        int zero = 0;
        if (u_pid && copy_to_user(u_pid, &zero, sizeof(int))) {
            return -EFAULT;
        }
        return 0;  /* 空闲 */
    }
}

/* 设置调度策略 */
static int handle_set_sched_policy(int __user *u_policy) {
    int policy;
    
    if (copy_from_user(&policy, u_policy, sizeof(int))) {
        return -EFAULT;
    }
    
    if (policy < 0 || policy >= SCHED_MAX) {
        return PROC_ERR_INVALID_STATE;
    }
    
    return scheduler_set_type(&g_scheduler, policy);
}

/* 触发老化 */
static int handle_trigger_aging(int __user *u_count) {
    int aged = scheduler_aging(&g_scheduler);
    
    if (u_count && copy_to_user(u_count, &aged, sizeof(int))) {
        return -EFAULT;
    }
    
    return 0;
}

/* 重置调度统计 */
static int handle_reset_sched_stats(void) {
    scheduler_reset_stats(&g_scheduler);
    return 0;
}

/* 杀死所有进程 */
static int handle_kill_all(int __user *u_count) {
    int killed = process_kill_all(&g_proc_table);
    
    if (u_count && copy_to_user(u_count, &killed, sizeof(int))) {
        return -EFAULT;
    }
    
    return 0;
}

/* 获取资源统计 */
static int handle_get_resource_stats(struct resource_stats __user *u_stats) {
    struct resource_stats k_stats;
    
    resource_get_stats(&g_proc_table, &k_stats);
    
    if (copy_to_user(u_stats, &k_stats, sizeof(struct resource_stats))) {
        return -EFAULT;
    }
    
    return 0;
}

/* 主ioctl处理函数 */
long procman_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
    int ret = 0;
    
    printk(KERN_DEBUG "procman_ioctl: cmd=0x%x, arg=0x%lx\n", cmd, arg);
    
    switch (cmd) {
        case PROC_MAN_CREATE_PROC:
            ret = handle_create_proc((struct proc_create_req __user *)arg);
            break;
            
        case PROC_MAN_DESTROY_PROC:
            ret = handle_destroy_proc((pid_t __user *)arg);
            break;
            
        case PROC_MAN_LIST_PROCS:
            ret = handle_list_procs((struct proc_list_req __user *)arg);
            break;
            
        case PROC_MAN_GET_PROC_COUNT:
            ret = handle_get_proc_count((int __user *)arg);
            break;
            
        case PROC_MAN_SET_STATE:
            ret = handle_set_state((struct set_state_req __user *)arg);
            break;
            
        case PROC_MAN_SET_PRIORITY:
            ret = handle_set_priority((struct set_priority_req __user *)arg);
            break;
            
        case PROC_MAN_WAKEUP_ALL: {
            int count = process_wakeup_all_blocked(&g_proc_table);
            if (copy_to_user((int __user *)arg, &count, sizeof(int))) {
                ret = -EFAULT;
            }
            break;
        }
            
        case PROC_MAN_SCHEDULE:
            ret = handle_schedule((int __user *)arg);
            break;
            
        case PROC_MAN_GET_SCHED_STATS: {
            struct scheduler_stats stats;
            scheduler_get_stats(&g_scheduler, &stats);
            if (copy_to_user((struct scheduler_stats __user *)arg, &stats, 
                           sizeof(struct scheduler_stats))) {
                ret = -EFAULT;
            }
            break;
        }
            
        case PROC_MAN_SET_SCHED_POLICY:
            ret = handle_set_sched_policy((int __user *)arg);
            break;
            
        case PROC_MAN_GET_SCHED_POLICY: {
            scheduler_type_t policy = scheduler_get_type(&g_scheduler);
            if (copy_to_user((int __user *)arg, &policy, sizeof(int))) {
                ret = -EFAULT;
            }
            break;
        }
            
        case PROC_MAN_TRIGGER_AGING:
            ret = handle_trigger_aging((int __user *)arg);
            break;
            
        case PROC_MAN_RESET_SCHED_STATS:
            ret = handle_reset_sched_stats();
            break;
            
        case PROC_MAN_KILL_ALL:
            ret = handle_kill_all((int __user *)arg);
            break;
            
        case PROC_MAN_GET_RESOURCE_STATS:
            ret = handle_get_resource_stats((struct resource_stats __user *)arg);
            break;
            
        default:
            printk(KERN_WARNING "procman_ioctl: unknown command 0x%x\n", cmd);
            ret = -ENOTTY;
            break;
    }
    
    return ret;
}

/* 模块初始化 */
static int __init procman_init(void) {
    int ret = 0;
    dev_t dev_no = 0;
    
    printk(KERN_INFO "procman: Initializing Process Manager v1.0.0\n");
    
    /* 1. 初始化进程表 */
    ret = process_table_init(&g_proc_table);
    if (ret) {
        printk(KERN_ERR "procman: Failed to initialize process table: %d\n", ret);
        return ret;
    }
    
    /* 2. 初始化调度器 */
    ret = scheduler_init(&g_scheduler, NULL);
    if (ret) {
        printk(KERN_ERR "procman: Failed to initialize scheduler: %d\n", ret);
        process_table_cleanup(&g_proc_table);
        return ret;
    }
    
    /* 3. 注册字符设备 */
    ret = alloc_chrdev_region(&dev_no, 0, 1, DEVICE_NAME);
    if (ret < 0) {
        printk(KERN_ERR "procman: Failed to allocate char device region\n");
        goto err_scheduler_cleanup;
    }
    
    major_number = MAJOR(dev_no);
    printk(KERN_INFO "procman: Allocated device major number %d\n", major_number);
    
    /* 4. 创建设备类 */
    procman_class = class_create(DEVICE_NAME);
    if (IS_ERR(procman_class)) {
        printk(KERN_ERR "procman: Failed to create device class\n");
        ret = PTR_ERR(procman_class);
        goto err_chrdev_region;
    }
    
    /* 5. 初始化cdev */
    cdev_init(&procman_cdev, &procman_fops);
    procman_cdev.owner = THIS_MODULE;
    
    /* 6. 添加cdev到内核 */
    ret = cdev_add(&procman_cdev, dev_no, 1);
    if (ret < 0) {
        printk(KERN_ERR "procman: Failed to add cdev\n");
        goto err_class_destroy;
    }
    
    /* 7. 创建设备文件 */
    procman_device = device_create(procman_class, NULL, dev_no, NULL, DEVICE_NAME);
    if (IS_ERR(procman_device)) {
        printk(KERN_ERR "procman: Failed to create device\n");
        ret = PTR_ERR(procman_device);
        goto err_cdev_del;
    }
    
    /* 8. 创建/proc接口 */
    proc_entry = proc_create("procman", 0444, NULL, &procman_proc_fops);
    if (!proc_entry) {
        printk(KERN_WARNING "procman: Failed to create /proc/procman\n");
    } else {
        printk(KERN_INFO "procman: Created /proc/procman interface\n");
    }
    
    /* 9. 创建初始进程 */
    struct pcb *init = pcb_create("init", 1, 0);
    if (init) {
        pcb_set_state(init, PROC_STATE_READY);
        process_table_add(&g_proc_table, init);
        scheduler_enqueue(&g_scheduler, init);
    }
    
    struct pcb *kthreadd = pcb_create("kthreadd", 1, 1);
    if (kthreadd) {
        pcb_set_state(kthreadd, PROC_STATE_READY);
        process_table_add(&g_proc_table, kthreadd);
        scheduler_enqueue(&g_scheduler, kthreadd);
        process_add_child(&g_proc_table, 1, kthreadd->pid);
    }
    
    printk(KERN_INFO "procman: Module initialized successfully\n");
    printk(KERN_INFO "procman: Device file: /dev/%s\n", DEVICE_NAME);
    if (proc_entry) {
        printk(KERN_INFO "procman: Status interface: /proc/procman\n");
    }
    
    return 0;

/* 错误处理 */
err_cdev_del:
    cdev_del(&procman_cdev);
err_class_destroy:
    class_destroy(procman_class);
err_chrdev_region:
    unregister_chrdev_region(dev_no, 1);
err_scheduler_cleanup:
    scheduler_cleanup(&g_scheduler);
    process_table_cleanup(&g_proc_table);
    
    return ret;
}

/* 模块退出 */
static void __exit procman_exit(void) {
    dev_t dev_no = MKDEV(major_number, 0);
    
    printk(KERN_INFO "procman: Exiting Process Manager Module\n");
    
    /* 1. 移除/proc接口 */
    if (proc_entry) {
        proc_remove(proc_entry);
    }
    
    /* 2. 清理调度器 */
    scheduler_cleanup(&g_scheduler);
    
    /* 3. 清理进程表 */
    process_table_cleanup(&g_proc_table);
    
    /* 4. 删除设备文件 */
    if (procman_device) {
        device_destroy(procman_class, dev_no);
    }
    
    /* 5. 删除cdev */
    cdev_del(&procman_cdev);
    
    /* 6. 销毁设备类 */
    if (procman_class) {
        class_destroy(procman_class);
    }
    
    /* 7. 注销设备号 */
    unregister_chrdev_region(dev_no, 1);
    
    printk(KERN_INFO "procman: Module removed successfully\n");
}

/* 模块声明 */
module_init(procman_init);
module_exit(procman_exit);