/*
 * [历史归档] 本示例属于项目起点 —— Linux 内核模块 proc_manager（src/），
 * 仅作历史保留。当前 Novix OS 的用户程序见 os/user/。
 */

/*
 * example_advanced.c - 高级使用示例
 * 展示调度器、统计、资源跟踪等高级功能
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>

#define DEVICE_NAME "/dev/procman"
#define PROC_MAN_IOC_MAGIC 'p'

/* ioctl命令定义 */
#define PROC_MAN_CREATE_PROC     _IOW(PROC_MAN_IOC_MAGIC, 1, struct proc_create_req)
#define PROC_MAN_DESTROY_PROC    _IOW(PROC_MAN_IOC_MAGIC, 2, pid_t)
#define PROC_MAN_LIST_PROCS      _IOR(PROC_MAN_IOC_MAGIC, 3, struct proc_list_req)
#define PROC_MAN_SET_STATE       _IOW(PROC_MAN_IOC_MAGIC, 7, struct set_state_req)
#define PROC_MAN_SET_PRIORITY    _IOW(PROC_MAN_IOC_MAGIC, 8, struct set_priority_req)
#define PROC_MAN_SCHEDULE        _IO(PROC_MAN_IOC_MAGIC, 10)
#define PROC_MAN_GET_SCHED_STATS _IOR(PROC_MAN_IOC_MAGIC, 11, struct scheduler_stats)
#define PROC_MAN_SET_SCHED_POLICY _IOW(PROC_MAN_IOC_MAGIC, 12, int)
#define PROC_MAN_GET_SCHED_POLICY _IOR(PROC_MAN_IOC_MAGIC, 13, int)
#define PROC_MAN_TRIGGER_AGING   _IO(PROC_MAN_IOC_MAGIC, 14)
#define PROC_MAN_RESET_SCHED_STATS _IO(PROC_MAN_IOC_MAGIC, 15)
#define PROC_MAN_GET_RESOURCE_STATS _IOR(PROC_MAN_IOC_MAGIC, 17, struct resource_stats)

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
    unsigned long memory_usage;
    unsigned long io_operations;
    unsigned long context_switches;
    unsigned long create_time;
};

struct proc_list_req {
    struct proc_info_resp *procs;
    int count;
    int max_count;
};

struct set_state_req {
    pid_t pid;
    int new_state;
};

struct set_priority_req {
    pid_t pid;
    int priority;
};

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

struct resource_stats {
    unsigned long total_cpu_time;
    unsigned long total_memory;
    unsigned long total_io_operations;
    unsigned long total_context_switches;
    int process_count;
    int running_count;
    int ready_count;
    int blocked_count;
    int zombie_count;
};

/* 辅助函数 */
void print_scheduler_stats(struct scheduler_stats *stats, int policy) {
    const char *policy_name = "Unknown";
    switch (policy) {
        case 0: policy_name = "Priority"; break;
        case 1: policy_name = "Round Robin"; break;
        case 2: policy_name = "FCFS"; break;
    }
    
    printf("\n=== Scheduler Statistics (%s) ===\n", policy_name);
    printf("Total schedules: %lu\n", stats->total_schedules);
    printf("Context switches: %lu\n", stats->context_switches);
    printf("Voluntary switches: %lu\n", stats->voluntary_switches);
    printf("Involuntary switches: %lu\n", stats->involuntary_switches);
    printf("Idle time: %lu, Busy time: %lu\n", stats->idle_time, stats->busy_time);
    
    if (stats->busy_time + stats->idle_time > 0) {
        printf("CPU utilization: %.1f%%\n", 
               stats->busy_time * 100.0 / (stats->busy_time + stats->idle_time));
    }
    
    printf("Throughput: %.2f processes/sec\n", stats->throughput);
    printf("Processes completed: %lu\n", stats->processes_completed);
    
    printf("Scheduling latency:\n");
    printf("  Min: %lu, Max: %lu", stats->min_latency, stats->max_latency);
    if (stats->total_schedules > 0) {
        printf(", Avg: %lu\n", stats->total_latency / stats->total_schedules);
    }
    
    printf("Priority distribution:\n");
    for (int i = 0; i < 10; i++) {
        if (stats->priority_counts[i] > 0) {
            printf("  Priority %d: %lu schedules\n", i + 1, stats->priority_counts[i]);
        }
    }
}

void print_resource_stats(struct resource_stats *stats) {
    printf("\n=== Resource Statistics ===\n");
    printf("Total processes: %d\n", stats->process_count);
    printf("Running: %d, Ready: %d, Blocked: %d, Zombie: %d\n",
           stats->running_count, stats->ready_count,
           stats->blocked_count, stats->zombie_count);
    printf("Total CPU time: %lu jiffies\n", stats->total_cpu_time);
    printf("Total memory: %lu bytes\n", stats->total_memory);
    printf("Total I/O operations: %lu\n", stats->total_io_operations);
    printf("Total context switches: %lu\n", stats->total_context_switches);
}

/* 示例1: 不同调度策略对比 */
void example_scheduler_policies(int fd) {
    printf("\n=== Example 1: Comparing Scheduler Policies ===\n");
    
    pid_t pids[5];
    struct proc_create_req req;
    struct scheduler_stats stats;
    
    /* 测试优先级调度 */
    printf("\n1. Priority Scheduling:\n");
    ioctl(fd, PROC_MAN_SET_SCHED_POLICY, (void*)0);
    ioctl(fd, PROC_MAN_RESET_SCHED_STATS, 0);
    
    /* 创建不同优先级的进程 */
    for (int i = 0; i < 5; i++) {
        char name[32];
        snprintf(name, sizeof(name), "proc_%d", i);
        strcpy(req.name, name);
        req.priority = (i % 3) + 1;  /* 优先级1-3 */
        req.parent_pid = 0;
        
        ioctl(fd, PROC_MAN_CREATE_PROC, &req);
        pids[i] = *((pid_t*)&req);
        printf("  Created %s (PID=%d, priority=%d)\n", name, pids[i], req.priority);
    }
    
    /* 运行调度器 */
    for (int i = 0; i < 20; i++) {
        int next_pid;
        ioctl(fd, PROC_MAN_SCHEDULE, &next_pid);
        usleep(50000);
    }
    
    /* 获取统计 */
    ioctl(fd, PROC_MAN_GET_SCHED_STATS, &stats);
    print_scheduler_stats(&stats, 0);
    
    /* 清理 */
    for (int i = 0; i < 5; i++) {
        ioctl(fd, PROC_MAN_DESTROY_PROC, &pids[i]);
    }
    
    /* 测试轮转调度 */
    printf("\n2. Round Robin Scheduling:\n");
    ioctl(fd, PROC_MAN_SET_SCHED_POLICY, (void*)1);
    ioctl(fd, PROC_MAN_RESET_SCHED_STATS, 0);
    
    for (int i = 0; i < 5; i++) {
        char name[32];
        snprintf(name, sizeof(name), "rr_%d", i);
        strcpy(req.name, name);
        req.priority = 5;  /* 相同优先级 */
        req.parent_pid = 0;
        
        ioctl(fd, PROC_MAN_CREATE_PROC, &req);
        pids[i] = *((pid_t*)&req);
    }
    
    for (int i = 0; i < 30; i++) {
        int next_pid;
        ioctl(fd, PROC_MAN_SCHEDULE, &next_pid);
        usleep(50000);
    }
    
    ioctl(fd, PROC_MAN_GET_SCHED_STATS, &stats);
    print_scheduler_stats(&stats, 1);
    
    /* 清理 */
    for (int i = 0; i < 5; i++) {
        ioctl(fd, PROC_MAN_DESTROY_PROC, &pids[i]);
    }
}

/* 示例2: 优先级老化机制 */
void example_aging_mechanism(int fd) {
    printf("\n=== Example 2: Aging Mechanism ===\n");
    
    pid_t pids[3];
    struct proc_create_req req;
    struct scheduler_stats stats;
    
    ioctl(fd, PROC_MAN_SET_SCHED_POLICY, (void*)0);
    ioctl(fd, PROC_MAN_RESET_SCHED_STATS, 0);
    
    /* 创建高低优先级进程 */
    strcpy(req.name, "high_prio");
    req.priority = 1;
    req.parent_pid = 0;
    ioctl(fd, PROC_MAN_CREATE_PROC, &req);
    pids[0] = *((pid_t*)&req);
    
    strcpy(req.name, "medium_prio");
    req.priority = 5;
    ioctl(fd, PROC_MAN_CREATE_PROC, &req);
    pids[1] = *((pid_t*)&req);
    
    strcpy(req.name, "low_prio");
    req.priority = 10;
    ioctl(fd, PROC_MAN_CREATE_PROC, &req);
    pids[2] = *((pid_t*)&req);
    
    printf("Created processes with priorities 1, 5, 10\n");
    
    /* 运行一段时间（低优先级可能得不到执行） */
    printf("\nRunning scheduler 10 times (before aging):\n");
    for (int i = 0; i < 10; i++) {
        int next_pid;
        ioctl(fd, PROC_MAN_SCHEDULE, &next_pid);
        if (next_pid > 0) {
            printf("  Schedule %d: PID=%d\n", i+1, next_pid);
        }
        usleep(100000);
    }
    
    /* 获取统计 */
    ioctl(fd, PROC_MAN_GET_SCHED_STATS, &stats);
    printf("\nBefore aging:\n");
    for (int i = 0; i < 10; i++) {
        if (stats.priority_counts[i] > 0) {
            printf("  Priority %d: %lu schedules\n", i+1, stats.priority_counts[i]);
        }
    }
    
    /* 触发老化 */
    printf("\nTriggering aging mechanism...\n");
    int aged;
    ioctl(fd, PROC_MAN_TRIGGER_AGING, &aged);
    printf("Aged %d processes\n", aged);
    
    /* 重置统计 */
    ioctl(fd, PROC_MAN_RESET_SCHED_STATS, 0);
    
    /* 再次运行 */
    printf("\nRunning scheduler 10 times (after aging):\n");
    for (int i = 0; i < 10; i++) {
        int next_pid;
        ioctl(fd, PROC_MAN_SCHEDULE, &next_pid);
        if (next_pid > 0) {
            printf("  Schedule %d: PID=%d\n", i+1, next_pid);
        }
        usleep(100000);
    }
    
    /* 获取新统计 */
    ioctl(fd, PROC_MAN_GET_SCHED_STATS, &stats);
    printf("\nAfter aging:\n");
    for (int i = 0; i < 10; i++) {
        if (stats.priority_counts[i] > 0) {
            printf("  Priority %d: %lu schedules\n", i+1, stats.priority_counts[i]);
        }
    }
    
    /* 清理 */
    for (int i = 0; i < 3; i++) {
        ioctl(fd, PROC_MAN_DESTROY_PROC, &pids[i]);
    }
}

/* 示例3: 资源跟踪 */
void example_resource_tracking(int fd) {
    printf("\n=== Example 3: Resource Tracking ===\n");
    
    pid_t pids[3];
    struct proc_create_req req;
    struct resource_stats stats;
    
    /* 创建进程 */
    for (int i = 0; i < 3; i++) {
        char name[32];
        snprintf(name, sizeof(name), "worker_%d", i);
        strcpy(req.name, name);
        req.priority = (i % 3) + 1;
        req.parent_pid = 0;
        
        ioctl(fd, PROC_MAN_CREATE_PROC, &req);
        pids[i] = *((pid_t*)&req);
    }
    
    /* 运行一些操作 */
    for (int i = 0; i < 10; i++) {
        int next_pid;
        ioctl(fd, PROC_MAN_SCHEDULE, &next_pid);
        usleep(100000);
    }
    
    /* 获取资源统计 */
    ioctl(fd, PROC_MAN_GET_RESOURCE_STATS, &stats);
    print_resource_stats(&stats);
    
    /* 模拟状态变化 */
    printf("\nChanging process states...\n");
    for (int i = 0; i < 3; i++) {
        struct set_state_req state_req = {.pid = pids[i], .new_state = 3}; /* BLOCKED */
        ioctl(fd, PROC_MAN_SET_STATE, &state_req);
    }
    
    /* 获取更新后的统计 */
    ioctl(fd, PROC_MAN_GET_RESOURCE_STATS, &stats);
    printf("\nAfter blocking processes:\n");
    printf("Running: %d, Ready: %d, Blocked: %d, Zombie: %d\n",
           stats.running_count, stats.ready_count,
           stats.blocked_count, stats.zombie_count);
    
    /* 清理 */
    for (int i = 0; i < 3; i++) {
        ioctl(fd, PROC_MAN_DESTROY_PROC, &pids[i]);
    }
}

/* 示例4: 并发测试 */
void *concurrent_thread(void *arg) {
    int fd = *(int*)arg;
    struct proc_create_req req;
    char name[32];
    
    for (int i = 0; i < 5; i++) {
        snprintf(name, sizeof(name), "thread_%ld_%d", (long)pthread_self(), i);
        strcpy(req.name, name);
        req.priority = (rand() % 10) + 1;
        req.parent_pid = 0;
        
        pid_t pid = 0;
        if (ioctl(fd, PROC_MAN_CREATE_PROC, &req) >= 0) {
            pid = *((pid_t*)&req);
            
            /* 做一些调度 */
            for (int j = 0; j < 3; j++) {
                int next_pid;
                ioctl(fd, PROC_MAN_SCHEDULE, &next_pid);
                usleep(10000);
            }
            
            /* 销毁进程 */
            ioctl(fd, PROC_MAN_DESTROY_PROC, &pid);
        }
    }
    
    return NULL;
}

void example_concurrency(int fd) {
    printf("\n=== Example 4: Concurrency Test ===\n");
    
    pthread_t threads[4];
    int thread_fd = fd;
    
    printf("Creating 4 threads, each creating/destroying 5 processes...\n");
    
    for (int i = 0; i < 4; i++) {
        pthread_create(&threads[i], NULL, concurrent_thread, &thread_fd);
    }
    
    for (int i = 0; i < 4; i++) {
        pthread_join(threads[i], NULL);
    }
    
    printf("All threads completed\n");
    
    /* 验证最终状态 */
    struct resource_stats stats;
    ioctl(fd, PROC_MAN_GET_RESOURCE_STATS, &stats);
    printf("\nFinal resource stats:\n");
    printf("Total processes: %d (should be 1 for init)\n", stats.process_count);
    printf("Total CPU time: %lu\n", stats.total_cpu_time);
    
    if (stats.process_count == 1) {
        printf("✓ Concurrency test PASSED - no process leaks\n");
    } else {
        printf("✗ Concurrency test FAILED - process count: %d\n", stats.process_count);
    }
}

/* 示例5: 性能基准测试 */
void example_performance_benchmark(int fd) {
    printf("\n=== Example 5: Performance Benchmark ===\n");
    
    struct timeval start, end;
    int iterations = 1000;
    
    /* 测试进程创建性能 */
    gettimeofday(&start, NULL);
    for (int i = 0; i < iterations; i++) {
        char name[32];
        struct proc_create_req req;
        pid_t pid = 0;
        
        snprintf(name, sizeof(name), "bench_%d", i);
        strcpy(req.name, name);
        req.priority = 5;
        req.parent_pid = 0;
        
        if (ioctl(fd, PROC_MAN_CREATE_PROC, &req) >= 0) {
            pid = *((pid_t*)&req);
            ioctl(fd, PROC_MAN_DESTROY_PROC, &pid);
        }
    }
    gettimeofday(&end, NULL);
    
    long time_us = (end.tv_sec - start.tv_sec) * 1000000 + 
                   (end.tv_usec - start.tv_usec);
    printf("Create/destroy %d processes: %ld us (%.2f us/op)\n", 
           iterations, time_us, (float)time_us / iterations);
    
    /* 测试调度性能 */
    ioctl(fd, PROC_MAN_RESET_SCHED_STATS, 0);
    
    /* 先创建一些进程 */
    pid_t pids[10];
    for (int i = 0; i < 10; i++) {
        char name[32];
        struct proc_create_req req;
        
        snprintf(name, sizeof(name), "sched_%d", i);
        strcpy(req.name, name);
        req.priority = 5;
        req.parent_pid = 0;
        
        ioctl(fd, PROC_MAN_CREATE_PROC, &req);
        pids[i] = *((pid_t*)&req);
    }
    
    gettimeofday(&start, NULL);
    for (int i = 0; i < iterations; i++) {
        int next_pid;
        ioctl(fd, PROC_MAN_SCHEDULE, &next_pid);
    }
    gettimeofday(&end, NULL);
    
    time_us = (end.tv_sec - start.tv_sec) * 1000000 + 
              (end.tv_usec - start.tv_usec);
    printf("Schedule %d times: %ld us (%.2f us/op)\n", 
           iterations, time_us, (float)time_us / iterations);
    
    /* 清理 */
    for (int i = 0; i < 10; i++) {
        ioctl(fd, PROC_MAN_DESTROY_PROC, &pids[i]);
    }
}

int main() {
    printf("=== Process Manager Advanced Example ===\n");
    printf("Demonstrates advanced features of the kernel module\n\n");
    
    /* 打开设备 */
    int fd = open(DEVICE_NAME, O_RDWR);
    if (fd < 0) {
        perror("Failed to open device");
        printf("Make sure the module is loaded: sudo insmod proc_manager.ko\n");
        return 1;
    }
    
    printf("Device opened successfully\n");
    
    srand(time(NULL));
    
    /* 运行高级示例 */
    example_scheduler_policies(fd);
    example_aging_mechanism(fd);
    example_resource_tracking(fd);
    example_concurrency(fd);
    example_performance_benchmark(fd);
    
    /* 关闭设备 */
    close(fd);
    printf("\nDevice closed\n");
    
    printf("\n=== Advanced Example Complete ===\n");
    printf("All advanced features demonstrated successfully!\n");
    
    return 0;
}