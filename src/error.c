#include "error.h"
#include <linux/string.h>
#include <linux/printk.h>

/* 错误信息映射 */
static const char *error_messages[] = {
    [0] = "Success",
    [-EPERM] = "Operation not permitted",
    [-ENOENT] = "No such process",
    [-ESRCH] = "No such process",
    [-EINTR] = "Interrupted system call",
    [-EIO] = "I/O error",
    [-ENXIO] = "No such device or address",
    [-E2BIG] = "Argument list too long",
    [-ENOEXEC] = "Exec format error",
    [-EBADF] = "Bad file number",
    [-ECHILD] = "No child processes",
    [-EAGAIN] = "Try again",
    [-ENOMEM] = "Out of memory",
    [-EACCES] = "Permission denied",
    [-EFAULT] = "Bad address",
    [-ENOTBLK] = "Block device required",
    [-EBUSY] = "Device or resource busy",
    [-EEXIST] = "Process already exists",
    [-EXDEV] = "Cross-device link",
    [-ENODEV] = "No such device",
    [-ENOTDIR] = "Not a directory",
    [-EISDIR] = "Is a directory",
    [-EINVAL] = "Invalid argument",
    [-ENFILE] = "File table overflow",
    [-EMFILE] = "Too many open files",
    [-ENOTTY] = "Not a typewriter",
    [-ETXTBSY] = "Text file busy",
    [-EFBIG] = "File too large",
    [-ENOSPC] = "No space left on device",
    [-ESPIPE] = "Illegal seek",
    [-EROFS] = "Read-only file system",
    [-EMLINK] = "Too many links",
    [-EPIPE] = "Broken pipe",
    [-EDOM] = "Math argument out of domain of func",
    [-ERANGE] = "Math result not representable",
    
    /* 自定义错误 */
    [PROC_ERR_GENERIC] = "General error",
    [PROC_ERR_NOMEM] = "Out of memory",
    [PROC_ERR_INVALID_PID] = "Invalid process ID",
    [PROC_ERR_PID_EXISTS] = "Process ID already exists",
    [PROC_ERR_PID_NOT_FOUND] = "Process not found",
    [PROC_ERR_INVALID_STATE] = "Invalid process state",
    [PROC_ERR_INVALID_PRIORITY] = "Invalid priority (must be 1-10)",
    [PROC_ERR_INVALID_NAME] = "Invalid process name",
    [PROC_ERR_QUEUE_FULL] = "Queue is full",
    [PROC_ERR_QUEUE_EMPTY] = "Queue is empty",
    [PROC_ERR_SCHEDULER] = "Scheduler error",
    [PROC_ERR_PERMISSION] = "Permission denied",
    [PROC_ERR_IOCTL] = "Invalid ioctl command",
};

/* 获取错误信息 */
const char *procman_strerror(int err) {
    int idx = -err;
    
    if (err >= 0) {
        return error_messages[0];
    }
    
    if (idx < 0 || idx >= (int)ARRAY_SIZE(error_messages)) {
        return "Unknown error";
    }
    
    if (!error_messages[idx]) {
        return "Unknown error";
    }
    
    return error_messages[idx];
}

/* 验证进程名 */
int validate_process_name(const char *name) {
    int len;
    
    if (!name) {
        return PROC_ERR_INVALID_NAME;
    }
    
    len = strlen(name);
    if (len == 0 || len >= 32) {
        return PROC_ERR_INVALID_NAME;
    }
    
    /* 检查名称是否只包含可打印字符 */
    for (int i = 0; i < len; i++) {
        if (name[i] < 32 || name[i] > 126) {
            return PROC_ERR_INVALID_NAME;
        }
    }
    
    return PROC_SUCCESS;
}

/* 验证优先级 */
int validate_process_priority(int priority) {
    if (priority < 1 || priority > 10) {
        return PROC_ERR_INVALID_PRIORITY;
    }
    return PROC_SUCCESS;
}

/* 验证PID */
int validate_pid(pid_t pid) {
    if (pid <= 0) {
        return PROC_ERR_INVALID_PID;
    }
    return PROC_SUCCESS;
}

/* 验证状态转换 */
int validate_state_transition(int old_state, int new_state) {
    /* 状态转换矩阵 */
    static const int matrix[6][6] = {
        {0, 1, 0, 0, 0, 1},  /* NEW -> READY, TERMINATED */
        {0, 0, 1, 1, 0, 1},  /* READY -> RUNNING, BLOCKED, TERMINATED */
        {0, 1, 0, 1, 0, 1},  /* RUNNING -> READY, BLOCKED, TERMINATED */
        {0, 1, 0, 0, 0, 1},  /* BLOCKED -> READY, TERMINATED */
        {0, 0, 0, 0, 0, 1},  /* ZOMBIE -> TERMINATED */
        {0, 0, 0, 0, 0, 0},  /* TERMINATED -> none */
    };
    
    if (old_state < 0 || old_state >= 6 || new_state < 0 || new_state >= 6) {
        return PROC_ERR_INVALID_STATE;
    }
    
    if (matrix[old_state][new_state] == 0) {
        return PROC_ERR_INVALID_STATE;
    }
    
    return PROC_SUCCESS;
}

/* 检查进程表容量 */
int check_process_table_capacity(void) {
    static const int MAX_PROCESSES = 1024;
    extern struct process_table g_proc_table;
    
    if (g_proc_table.process_count >= MAX_PROCESSES) {
        printk(KERN_WARNING "check_process_table_capacity: process table full (%d/%d)\n",
               g_proc_table.process_count, MAX_PROCESSES);
        return PROC_ERR_QUEUE_FULL;
    }
    
    return PROC_SUCCESS;
}

/* 检查资源限制 */
int check_resource_limits(unsigned long memory_request, unsigned long cpu_request) {
    static const unsigned long MAX_MEMORY_PER_PROCESS = 1024 * 1024;  /* 1MB */
    static const unsigned long MAX_CPU_TIME = 3600 * HZ;  /* 1小时 */
    
    /* 内存限制 */
    if (memory_request > MAX_MEMORY_PER_PROCESS) {
        printk(KERN_WARNING "check_resource_limits: memory request too large: %lu\n", 
               memory_request);
        return PROC_ERR_PERMISSION;
    }
    
    /* CPU时间限制（简化检查） */
    if (cpu_request > MAX_CPU_TIME) {
        printk(KERN_WARNING "check_resource_limits: CPU time request too large: %lu\n", 
               cpu_request);
        return PROC_ERR_PERMISSION;
    }
    
    return PROC_SUCCESS;
}

/* 验证PCB完整性 */
int verify_pcb_integrity(struct pcb *pcb) {
    int ret;
    
    if (!pcb) {
        return PROC_ERR_INVALID_PID;
    }
    
    if ((ret = validate_pid(pcb->pid)) != PROC_SUCCESS) {
        return ret;
    }
    
    if ((ret = validate_process_name(pcb->name)) != PROC_SUCCESS) {
        return ret;
    }
    
    if ((ret = validate_process_priority(pcb->priority)) != PROC_SUCCESS) {
        return ret;
    }
    
    if (pcb->state < 0 || pcb->state >= 6) {
        return PROC_ERR_INVALID_STATE;
    }
    
    if (pcb->cpu_time > 0xFFFFFFFFFFFFFFFF) {  /* 简单溢出检查 */
        return PROC_ERR_GENERIC;
    }
    
    return PROC_SUCCESS;
}