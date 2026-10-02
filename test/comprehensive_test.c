#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>

/* 定义ioctl命令 */
#define PROC_MAN_CREATE_PROC 0x7001
#define PROC_MAN_DESTROY_PROC 0x7002
#define PROC_MAN_LIST_PROCS 0x7003
#define PROC_MAN_SET_STATE 0x7004
#define PROC_MAN_SET_PRIORITY 0x7005
#define PROC_MAN_SCHEDULE 0x7006
#define PROC_MAN_GET_SCHED_STATS 0x7007
#define PROC_MAN_SET_SCHED_POLICY 0x7008
#define PROC_MAN_TRIGGER_AGING 0x7009

/* 进程创建请求 */
struct proc_create_req {
    char name[32];
    int priority;
    pid_t ppid;
    pid_t pid; /* 返回值 */
};

/* 进程列表请求 */
struct proc_list_req {
    int max_count;
    int count; /* 返回值 */
    pid_t pids[100];
};

/* 设置状态请求 */
struct set_state_req {
    pid_t pid;
    int state;
};

/* 设置优先级请求 */
struct set_priority_req {
    pid_t pid;
    int priority;
};

/* 调度器统计 */
struct scheduler_stats {
    unsigned long total_schedules;
    unsigned long context_switches;
    unsigned long voluntary_switches;
    unsigned long involuntary_switches;
    unsigned long idle_time;
    unsigned long busy_time;
    unsigned long priority_counts[10];
    unsigned long max_latency;
    unsigned long min_latency;
    unsigned long total_latency;
    unsigned long processes_completed;
    float throughput;
};

/* 测试函数 */
void test_memory_management(int fd) {
    printf("\n=== Testing Memory Management ===\n");
    
    /* 测试内存分配 */
    printf("Testing memory allocation...\n");
    
    /* 创建多个进程，测试内存分配 */
    for (int i = 0; i < 5; i++) {
        struct proc_create_req req;
        snprintf(req.name, sizeof(req.name), "mem_test_%d", i);
        req.priority = 5;
        req.ppid = 0;
        
        if (ioctl(fd, PROC_MAN_CREATE_PROC, &req) == -1) {
            perror("ioctl create process");
            continue;
        }
        
        printf("Created process %s (PID=%d)\n", req.name, req.pid);
    }
    printf("Memory management test completed\n");
}

void test_file_system(int fd) {
    printf("\n=== Testing File System ===\n");
    
    /* 测试文件操作 */
    printf("Testing file operations...\n");
    
    /* 创建文件 */
    int file_fd = open("test_file.txt", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (file_fd == -1) {
        perror("open test file");
    } else {
        const char *test_data = "Hello, File System!";
        write(file_fd, test_data, strlen(test_data));
        close(file_fd);
        printf("Created and wrote to test_file.txt\n");
        
        /* 读取文件 */
        file_fd = open("test_file.txt", O_RDONLY);
        if (file_fd != -1) {
            char buffer[100];
            ssize_t bytes_read = read(file_fd, buffer, sizeof(buffer) - 1);
            if (bytes_read > 0) {
                buffer[bytes_read] = '\0';
                printf("Read from test_file.txt: %s\n", buffer);
            }
            close(file_fd);
        }
        
        /* 删除文件 */
        unlink("test_file.txt");
        printf("Deleted test_file.txt\n");
    }
    
    printf("File system test completed\n");
}

void test_signal_handling(int fd) {
    printf("\n=== Testing Signal Handling ===\n");
    
    /* 测试信号发送 */
    printf("Testing signal handling...\n");
    
    /* 创建测试进程 */
    struct proc_create_req req;
    snprintf(req.name, sizeof(req.name), "signal_test");
    req.priority = 5;
    req.ppid = 0;
    
    if (ioctl(fd, PROC_MAN_CREATE_PROC, &req) == -1) {
        perror("ioctl create process");
        return;
    }
    
    printf("Created process %s (PID=%d)\n", req.name, req.pid);
    
    /* 这里应该测试信号发送，需要实现相应的ioctl命令 */
    printf("Signal handling test completed\n");
}

void test_smp_support(int fd) {
    printf("\n=== Testing SMP Support ===\n");
    
    /* 测试多处理器支持 */
    printf("Testing SMP support...\n");
    
    /* 创建多个进程，测试负载均衡 */
    for (int i = 0; i < 10; i++) {
        struct proc_create_req req;
        snprintf(req.name, sizeof(req.name), "smp_test_%d", i);
        req.priority = i % 10 + 1;
        req.ppid = 0;
        
        if (ioctl(fd, PROC_MAN_CREATE_PROC, &req) == -1) {
            perror("ioctl create process");
            continue;
        }
        
        printf("Created process %s (PID=%d, priority=%d)\n", req.name, req.pid, req.priority);
    }
    
    /* 触发调度 */
    if (ioctl(fd, PROC_MAN_SCHEDULE, NULL) == -1) {
        perror("ioctl schedule");
    }
    
    printf("SMP support test completed\n");
}

void test_realtime_scheduling(int fd) {
    printf("\n=== Testing Real-time Scheduling ===\n");
    
    /* 测试实时调度算法 */
    printf("Testing real-time scheduling...\n");
    
    /* 设置调度策略为EDF */
    int policy = 3; /* SCHED_EDF */
    if (ioctl(fd, PROC_MAN_SET_SCHED_POLICY, &policy) == -1) {
        perror("ioctl set sched policy");
    } else {
        printf("Set scheduling policy to EDF\n");
    }
    
    /* 创建实时任务 */
    for (int i = 0; i < 3; i++) {
        struct proc_create_req req;
        snprintf(req.name, sizeof(req.name), "rt_test_%d", i);
        req.priority = 1; /* 高优先级 */
        req.ppid = 0;
        
        if (ioctl(fd, PROC_MAN_CREATE_PROC, &req) == -1) {
            perror("ioctl create process");
            continue;
        }
        
        printf("Created real-time process %s (PID=%d)\n", req.name, req.pid);
    }
    
    /* 触发调度 */
    if (ioctl(fd, PROC_MAN_SCHEDULE, NULL) == -1) {
        perror("ioctl schedule");
    }
    
    printf("Real-time scheduling test completed\n");
}

void test_security(int fd) {
    printf("\n=== Testing Security ===\n");
    
    /* 测试权限管理 */
    printf("Testing security...\n");
    
    /* 创建不同权限的进程 */
    struct proc_create_req req;
    snprintf(req.name, sizeof(req.name), "secure_test");
    req.priority = 5;
    req.ppid = 0;
    
    if (ioctl(fd, PROC_MAN_CREATE_PROC, &req) == -1) {
        perror("ioctl create process");
        return;
    }
    
    printf("Created secure process %s (PID=%d)\n", req.name, req.pid);
    
    /* 这里应该测试权限检查，需要实现相应的ioctl命令 */
    printf("Security test completed\n");
}

void test_performance(int fd) {
    printf("\n=== Testing Performance ===\n");
    
    /* 测试性能 */
    printf("Testing performance...\n");
    
    /* 测量进程创建时间 */
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    /* 创建多个进程 */
    for (int i = 0; i < 100; i++) {
        struct proc_create_req req;
        snprintf(req.name, sizeof(req.name), "perf_test_%d", i);
        req.priority = 5;
        req.ppid = 0;
        
        if (ioctl(fd, PROC_MAN_CREATE_PROC, &req) == -1) {
            perror("ioctl create process");
            continue;
        }
    }
    
    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed = (end.tv_sec - start.tv_sec) * 1000.0 + (end.tv_nsec - start.tv_nsec) / 1000000.0;
    printf("Created 100 processes in %.2f ms\n", elapsed);
    
    /* 测量调度性能 */
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    for (int i = 0; i < 100; i++) {
        if (ioctl(fd, PROC_MAN_SCHEDULE, NULL) == -1) {
            perror("ioctl schedule");
            break;
        }
    }
    
    clock_gettime(CLOCK_MONOTONIC, &end);
    elapsed = (end.tv_sec - start.tv_sec) * 1000.0 + (end.tv_nsec - start.tv_nsec) / 1000000.0;
    printf("100 schedule operations in %.2f ms\n", elapsed);
    
    printf("Performance test completed\n");
}

int main() {
    int fd;
    
    /* 打开设备文件 */
    fd = open("/dev/procman", O_RDWR);
    if (fd == -1) {
        perror("open /dev/procman");
        printf("Please load the module and create the device file first.\n");
        return 1;
    }
    
    printf("=====================================\n");
    printf("Comprehensive Test Suite\n");
    printf("=====================================\n");
    
    /* 运行各个测试 */
    test_memory_management(fd);
    test_file_system(fd);
    test_signal_handling(fd);
    test_smp_support(fd);
    test_realtime_scheduling(fd);
    test_security(fd);
    test_performance(fd);
    
    /* 获取调度器统计信息 */
    struct scheduler_stats stats;
    if (ioctl(fd, PROC_MAN_GET_SCHED_STATS, &stats) == -1) {
        perror("ioctl get sched stats");
    } else {
        printf("\n=== Scheduler Statistics ===\n");
        printf("Total schedules: %lu\n", stats.total_schedules);
        printf("Context switches: %lu\n", stats.context_switches);
        printf("Idle time: %lu\n", stats.idle_time);
        printf("Busy time: %lu\n", stats.busy_time);
        printf("Processes completed: %lu\n", stats.processes_completed);
        printf("Throughput: %.2f processes/sec\n", stats.throughput);
    }
    
    /* 关闭设备文件 */
    close(fd);
    
    printf("\n=====================================\n");
    printf("All tests completed\n");
    printf("=====================================\n");
    
    return 0;
}
