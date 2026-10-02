/*
 * [历史归档] 本示例属于项目起点 —— Linux 内核模块 proc_manager（src/），
 * 仅作历史保留。当前 Novix OS 的用户程序见 os/user/。
 */

/*
 * example_simple.c - 简单使用示例
 * 展示如何与进程管理器内核模块交互
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <errno.h>

#define DEVICE_NAME "/dev/procman"
#define PROC_MAN_IOC_MAGIC 'p'

/* ioctl命令定义 */
#define PROC_MAN_CREATE_PROC     _IOW(PROC_MAN_IOC_MAGIC, 1, struct proc_create_req)
#define PROC_MAN_DESTROY_PROC    _IOW(PROC_MAN_IOC_MAGIC, 2, pid_t)
#define PROC_MAN_LIST_PROCS      _IOR(PROC_MAN_IOC_MAGIC, 3, struct proc_list_req)
#define PROC_MAN_GET_PROC_COUNT  _IOR(PROC_MAN_IOC_MAGIC, 4, int)
#define PROC_MAN_SCHEDULE        _IO(PROC_MAN_IOC_MAGIC, 10)

/* 数据结构 */
struct proc_create_req {
    char name[32];
    int priority;
    pid_t parent_pid;
};

struct proc_info_resp {
    pid_t pid;
    pid_t ppid;
    char name[32];
    int state;
    int priority;
    unsigned long cpu_time;
    unsigned long create_time;
};

struct proc_list_req {
    struct proc_info_resp *procs;
    int count;
    int max_count;
};

/* 状态转换字符串 */
const char *state_to_str(int state) {
    switch (state) {
        case 0: return "NEW";
        case 1: return "READY";
        case 2: return "RUNNING";
        case 3: return "BLOCKED";
        case 4: return "ZOMBIE";
        case 5: return "TERMINATED";
        default: return "UNKNOWN";
    }
}

/* 打开设备 */
int open_device(void) {
    int fd = open(DEVICE_NAME, O_RDWR);
    if (fd < 0) {
        perror("Failed to open device");
        return -1;
    }
    printf("Device opened successfully (fd=%d)\n", fd);
    return fd;
}

/* 创建进程示例 */
void example_create_process(int fd) {
    printf("\n=== Example 1: Creating Processes ===\n");
    
    struct proc_create_req req;
    pid_t pid;
    
    /* 创建init进程 */
    strcpy(req.name, "init");
    req.priority = 1;
    req.parent_pid = 0;
    
    if (ioctl(fd, PROC_MAN_CREATE_PROC, &req) < 0) {
        perror("Failed to create init process");
        return;
    }
    pid = *((pid_t*)&req);
    printf("Created 'init' (PID=%d, priority=1)\n", pid);
    
    /* 创建bash进程（子进程） */
    strcpy(req.name, "bash");
    req.priority = 5;
    req.parent_pid = pid;
    
    if (ioctl(fd, PROC_MAN_CREATE_PROC, &req) < 0) {
        perror("Failed to create bash process");
        return;
    }
    pid = *((pid_t*)&req);
    printf("Created 'bash' (PID=%d, priority=5, parent=1)\n", pid);
    
    /* 创建vim进程 */
    strcpy(req.name, "vim");
    req.priority = 3;
    req.parent_pid = 0;
    
    if (ioctl(fd, PROC_MAN_CREATE_PROC, &req) < 0) {
        perror("Failed to create vim process");
        return;
    }
    pid = *((pid_t*)&req);
    printf("Created 'vim' (PID=%d, priority=3)\n", pid);
}

/* 列出进程示例 */
void example_list_processes(int fd) {
    printf("\n=== Example 2: Listing Processes ===\n");
    
    struct proc_list_req req;
    struct proc_info_resp procs[10];
    int i;
    
    req.procs = procs;
    req.max_count = 10;
    
    if (ioctl(fd, PROC_MAN_LIST_PROCS, &req) < 0) {
        perror("Failed to list processes");
        return;
    }
    
    printf("Total processes: %d\n", req.count);
    printf("PID\tName\t\tState\t\tPriority\tCPU Time\n");
    printf("------------------------------------------------------------\n");
    
    for (i = 0; i < req.count; i++) {
        printf("%d\t%-10s\t%-10s\t%d\t\t%lu\n",
               procs[i].pid, procs[i].name,
               state_to_str(procs[i].state),
               procs[i].priority, procs[i].cpu_time);
    }
}

/* 获取进程计数示例 */
void example_get_count(int fd) {
    printf("\n=== Example 3: Getting Process Count ===\n");
    
    int count;
    if (ioctl(fd, PROC_MAN_GET_PROC_COUNT, &count) < 0) {
        perror("Failed to get process count");
        return;
    }
    
    printf("Current process count: %d\n", count);
}

/* 调度示例 */
void example_schedule(int fd) {
    printf("\n=== Example 4: Scheduling ===\n");
    
    printf("Running scheduler 5 times:\n");
    for (int i = 0; i < 5; i++) {
        int next_pid;
        
        if (ioctl(fd, PROC_MAN_SCHEDULE, &next_pid) < 0) {
            perror("Schedule failed");
            break;
        }
        
        if (next_pid > 0) {
            printf("  Schedule %d: Running PID=%d\n", i+1, next_pid);
        } else {
            printf("  Schedule %d: No process to run (idle)\n", i+1);
        }
        
        usleep(100000);  /* 100ms delay */
    }
}

/* 销毁进程示例 */
void example_destroy_process(int fd) {
    printf("\n=== Example 5: Destroying Processes ===\n");
    
    /* 先列出进程看看有哪些 */
    struct proc_list_req req;
    struct proc_info_resp procs[10];
    
    req.procs = procs;
    req.max_count = 10;
    
    if (ioctl(fd, PROC_MAN_LIST_PROCS, &req) < 0) {
        perror("Failed to list processes");
        return;
    }
    
    /* 销毁所有非-init进程 */
    for (int i = 0; i < req.count; i++) {
        if (strcmp(procs[i].name, "init") != 0) {
            pid_t pid = procs[i].pid;
            if (ioctl(fd, PROC_MAN_DESTROY_PROC, &pid) < 0) {
                printf("Failed to destroy PID=%d\n", pid);
            } else {
                printf("Destroyed PID=%d (%s)\n", pid, procs[i].name);
            }
        }
    }
}

/* 错误处理示例 */
void example_error_handling(int fd) {
    printf("\n=== Example 6: Error Handling ===\n");
    
    /* 测试1: 无效的进程名 */
    struct proc_create_req req;
    strcpy(req.name, "");  /* 空名字 */
    req.priority = 5;
    req.parent_pid = 0;
    
    if (ioctl(fd, PROC_MAN_CREATE_PROC, &req) < 0) {
        printf("✓ Correctly rejected empty process name\n");
    }
    
    /* 测试2: 无效的优先级 */
    strcpy(req.name, "test");
    req.priority = 0;  /* 无效优先级 */
    
    if (ioctl(fd, PROC_MAN_CREATE_PROC, &req) < 0) {
        printf("✓ Correctly rejected invalid priority 0\n");
    }
    
    req.priority = 11;  /* 无效优先级 */
    
    if (ioctl(fd, PROC_MAN_CREATE_PROC, &req) < 0) {
        printf("✓ Correctly rejected invalid priority 11\n");
    }
    
    /* 测试3: 不存在的PID */
    pid_t invalid_pid = 99999;
    if (ioctl(fd, PROC_MAN_DESTROY_PROC, &invalid_pid) < 0) {
        printf("✓ Correctly rejected non-existent PID\n");
    }
}

int main() {
    printf("=== Process Manager Simple Example ===\n");
    printf("Demonstrates basic interaction with the kernel module\n\n");
    
    /* 打开设备 */
    int fd = open_device();
    if (fd < 0) {
        return 1;
    }
    
    /* 运行示例 */
    example_create_process(fd);
    example_list_processes(fd);
    example_get_count(fd);
    example_schedule(fd);
    example_error_handling(fd);
    example_destroy_process(fd);
    
    /* 最终列表 */
    example_list_processes(fd);
    
    /* 关闭设备 */
    close(fd);
    printf("\nDevice closed\n");
    
    printf("\n=== Example Complete ===\n");
    printf("All examples demonstrated successfully!\n");
    
    return 0;
}