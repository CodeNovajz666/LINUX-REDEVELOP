#ifndef _ERROR_H
#define _ERROR_H

#include <linux/errno.h>

/* 自定义错误码 */
enum procman_error {
    PROC_SUCCESS = 0,
    PROC_ERR_GENERIC = -1,
    PROC_ERR_NOMEM = -ENOMEM,
    PROC_ERR_INVALID_PID = -EINVAL,
    PROC_ERR_PID_EXISTS = -EEXIST,
    PROC_ERR_PID_NOT_FOUND = -ENOENT,
    PROC_ERR_INVALID_STATE = -EINVAL,
    PROC_ERR_INVALID_PRIORITY = -EINVAL,
    PROC_ERR_INVALID_NAME = -EINVAL,
    PROC_ERR_QUEUE_FULL = -ENOSPC,
    PROC_ERR_QUEUE_EMPTY = -ENODATA,
    PROC_ERR_SCHEDULER = -EFAULT,
    PROC_ERR_PERMISSION = -EPERM,
    PROC_ERR_IOCTL = -ENOTTY,
};

/* 错误处理函数 */
const char *procman_strerror(int err);
int validate_process_name(const char *name);
int validate_process_priority(int priority);
int validate_pid(pid_t pid);
int validate_state_transition(int old_state, int new_state);
int check_process_table_capacity(void);
int check_resource_limits(unsigned long memory_request, unsigned long cpu_request);
int verify_pcb_integrity(struct pcb *pcb);

#endif /* _ERROR_H */