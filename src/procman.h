#ifndef _PROCMAN_H
#define _PROCMAN_H

#include <linux/ioctl.h>

/* 设备名称 */
#define DEVICE_NAME "procman"
#define DEVICE_CLASS "procman"

/* ioctl命令码 */
#define PROC_MAN_IOC_MAGIC 'p'

#define PROC_MAN_CREATE_PROC     _IOW(PROC_MAN_IOC_MAGIC, 1, struct proc_create_req)
#define PROC_MAN_DESTROY_PROC    _IOW(PROC_MAN_IOC_MAGIC, 2, pid_t)
#define PROC_MAN_LIST_PROCS      _IOR(PROC_MAN_IOC_MAGIC, 3, struct proc_list_req)
#define PROC_MAN_GET_PROC_COUNT  _IOR(PROC_MAN_IOC_MAGIC, 4, int)
#define PROC_MAN_DUMP_PROC_INFO  _IOW(PROC_MAN_IOC_MAGIC, 5, pid_t)
#define PROC_MAN_GET_STATS       _IOR(PROC_MAN_IOC_MAGIC, 6, struct proc_stats)
#define PROC_MAN_SET_STATE       _IOW(PROC_MAN_IOC_MAGIC, 7, struct set_state_req)
#define PROC_MAN_SET_PRIORITY    _IOW(PROC_MAN_IOC_MAGIC, 8, struct set_priority_req)
#define PROC_MAN_WAKEUP_ALL      _IO(PROC_MAN_IOC_MAGIC, 9)
#define PROC_MAN_SCHEDULE        _IO(PROC_MAN_IOC_MAGIC, 10)
#define PROC_MAN_GET_SCHED_STATS _IOR(PROC_MAN_IOC_MAGIC, 11, struct scheduler_stats)
#define PROC_MAN_SET_SCHED_POLICY _IOW(PROC_MAN_IOC_MAGIC, 12, int)
#define PROC_MAN_GET_SCHED_POLICY _IOR(PROC_MAN_IOC_MAGIC, 13, int)
#define PROC_MAN_TRIGGER_AGING   _IO(PROC_MAN_IOC_MAGIC, 14)
#define PROC_MAN_RESET_SCHED_STATS _IO(PROC_MAN_IOC_MAGIC, 15)
#define PROC_MAN_KILL_ALL        _IO(PROC_MAN_IOC_MAGIC, 16)
#define PROC_MAN_GET_RESOURCE_STATS _IOR(PROC_MAN_IOC_MAGIC, 17, struct resource_stats)

#define PROC_MAN_IOC_MAXNR 17

/* 数据结构定义 */

/* 进程创建请求 */
struct proc_create_req {
    char name[32];
    int priority;
    pid_t parent_pid;
};

/* 进程信息响应 */
struct proc_info_resp {
    pid_t pid;
    pid_t ppid;
    char name[32];
    int state;
    int priority;
    unsigned long cpu_time;
    unsigned long memory_usage;
    unsigned long io_operations;
    unsigned long context_switches;
    unsigned long create_time;
};

/* 进程列表请求 */
struct proc_list_req {
    struct proc_info_resp *procs;
    int count;
    int max_count;
};

/* 进程统计 */
struct proc_stats {
    int total_processes;
    int running_processes;
    int ready_processes;
    int blocked_processes;
    int zombie_processes;
    unsigned long total_cpu_time;
    unsigned long total_memory_usage;
    unsigned long total_io_operations;
};

/* 状态设置请求 */
struct set_state_req {
    pid_t pid;
    int new_state;
};

/* 优先级设置请求 */
struct set_priority_req {
    pid_t pid;
    int priority;
};

#endif /* _PROCMAN_H */