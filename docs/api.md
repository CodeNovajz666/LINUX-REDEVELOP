# Process Manager API Documentation

> **[历史归档]** 本文档描述项目起点 —— Linux 内核模块 proc_manager
> （src/），并非当前的 Novix OS，仅作历史保留。
> 当前架构见 [os/docs/DESIGN.md](../os/docs/DESIGN.md)，
> 系统调用 ABI 见根目录 [DESIGN.md](../DESIGN.md)。

## 1. Overview
This document describes the user-space API for the Process Manager kernel module.

## 2. Device Interface

### 2.1 Device File
- Name: `/dev/procman`
- Type: Character device
- Permissions: Read/Write (0666)
- Major number: Dynamically allocated

### 2.2 Opening the Device
c
int fd = open("/dev/procman", O_RDWR);
if (fd < 0) {
perror("Failed to open device");
return 1;
}
## 3. ioctl Commands

### 3.1 Command Structure
All ioctl commands use the same magic number: `'p'` (0x70)

### 3.2 Command List

#### 3.2.1 Process Management
| Command | Code | Request Structure | Description |
|---------|------|-------------------|-------------|
| `PROC_MAN_CREATE_PROC` | 0x7001 | `struct proc_create_req` | Create new process |
| `PROC_MAN_DESTROY_PROC` | 0x7002 | `pid_t` | Destroy process by PID |
| `PROC_MAN_LIST_PROCS` | 0x7003 | `struct proc_list_req` | List all processes |
| `PROC_MAN_GET_PROC_COUNT` | 0x7004 | `int` | Get process count |
| `PROC_MAN_SET_STATE` | 0x7007 | `struct set_state_req` | Change process state |
| `PROC_MAN_SET_PRIORITY` | 0x7008 | `struct set_priority_req` | Change process priority |
| `PROC_MAN_WAKEUP_ALL` | 0x7009 | None | Wake up all blocked processes |
| `PROC_MAN_KILL_ALL` | 0x7010 | None | Kill all non-system processes |

#### 3.2.2 Scheduler Control
| Command | Code | Request Structure | Description |
|---------|------|-------------------|-------------|
| `PROC_MAN_SCHEDULE` | 0x700A | None | Trigger scheduling |
| `PROC_MAN_GET_SCHED_STATS` | 0x700B | `struct scheduler_stats` | Get scheduler statistics |
| `PROC_MAN_SET_SCHED_POLICY` | 0x700C | `int` | Set scheduling policy |
| `PROC_MAN_GET_SCHED_POLICY` | 0x700D | `int` | Get scheduling policy |
| `PROC_MAN_TRIGGER_AGING` | 0x700E | None | Trigger priority aging |
| `PROC_MAN_RESET_SCHED_STATS` | 0x700F | None | Reset scheduler statistics |

#### 3.2.3 Resource Monitoring
| Command | Code | Request Structure | Description |
|---------|------|-------------------|-------------|
| `PROC_MAN_GET_RESOURCE_STATS` | 0x7011 | `struct resource_stats` | Get resource statistics |

## 4. Data Structures

### 4.1 Process Creation Request
c
struct proc_create_req {
char name[32]; /* Process name (max 31 chars + null) */
int priority; /* Priority (1-10, 1=highest) */
pid_t parent_pid; /* Parent PID (0 for no parent) */
};
### 4.2 Process Information Response
c
struct proc_info_resp {
pid_t pid; /* Process ID */
pid_t ppid; /* Parent PID */
char name[32]; /* Process name */
int state; /* Process state (0-5) */
int priority; /* Current priority */
unsigned long cpu_time; /* CPU time used (jiffies) */
unsigned long memory_usage; /* Memory usage (bytes) */
unsigned long io_operations; /* I/O count */
unsigned long context_switches; /* Context switch count */
unsigned long create_time; /* Creation timestamp */
};
### 4.3 Process List Request
c
struct proc_list_req {
struct proc_info_resp procs; /Buffer for process info */
int count; /* Actual process count */
int max_count; /* Buffer capacity */
};
### 4.4 State Change Request
c
struct set_state_req {
pid_t pid; /* Process ID */
int new_state; /* New state (0-5) */
};
### 4.5 Priority Change Request
c
struct set_priority_req {
pid_t pid; /* Process ID */
int priority; /* New priority (1-10) */
};
### 4.6 Scheduler Statistics
c
struct scheduler_stats {
unsigned long total_schedules; /* Total scheduling decisions */
unsigned long context_switches; /* Context switches */
unsigned long voluntary_switches; /* Voluntary context switches */
unsigned long involuntary_switches; /* Involuntary context switches */
unsigned long idle_time; /* Idle time ticks */
unsigned long busy_time; /* Busy time ticks */
unsigned long priority_counts[10]; /* Schedule count per priority */
unsigned long max_latency; /* Maximum scheduling latency */
unsigned long min_latency; /* Minimum scheduling latency */
unsigned long total_latency; /* Total latency */
unsigned long processes_completed; /* Processes completed */
float throughput; /* Throughput (processes/sec) */
};
### 4.7 Resource Statistics
c
struct resource_stats {
unsigned long total_cpu_time; /* Total CPU time */
unsigned long total_memory; /* Total memory usage */
unsigned long total_io_operations; /* Total I/O operations */
unsigned long total_context_switches; /* Total context switches */
int process_count; /* Total process count */
int running_count; /* Running processes */
int ready_count; /* Ready processes */
int blocked_count; /* Blocked processes */
int zombie_count; /* Zombie processes */
};
## 5. Process States

### 5.1 State Definitions
| Value | Constant | Description |
|-------|----------|-------------|
| 0 | `PROC_STATE_NEW` | Newly created process |
| 1 | `PROC_STATE_READY` | Ready to run |
| 2 | `PROC_STATE_RUNNING` | Currently running |
| 3 | `PROC_STATE_BLOCKED` | Blocked/waiting |
| 4 | `PROC_STATE_ZOMBIE` | Terminated, not reaped |
| 5 | `PROC_STATE_TERMINATED` | Terminated and reaped |

### 5.2 State Transitions
Allowed transitions:
- NEW → READY, TERMINATED
- READY → RUNNING, BLOCKED, TERMINATED
- RUNNING → READY, BLOCKED, TERMINATED
- BLOCKED → READY, TERMINATED
- ZOMBIE → TERMINATED
- TERMINATED → (no transitions)

## 6. Scheduling Policies

### 6.1 Policy Definitions
| Value | Constant | Description |
|-------|----------|-------------|
| 0 | `SCHED_PRIORITY` | Priority-based scheduling |
| 1 | `SCHED_ROUND_ROBIN` | Round-robin with time slices |
| 2 | `SCHED_FCFS` | First-Come First-Served |

### 6.2 Policy Characteristics
1. **Priority Scheduling**
   - Higher priority runs first
   - Same priority: FIFO order
   - Preemptive: Higher priority can preempt
   - Aging prevents starvation

2. **Round Robin**
   - Time quantum = priority * 10 ticks
   - Preemptive when time slice expires
   - Fair CPU allocation

3. **First-Come First-Served**
   - Non-preemptive
   - Processes run in creation order
   - Simple but can cause convoy effect

## 7. Error Codes

### 7.1 System Error Codes
Standard Linux error codes are used where applicable.

### 7.2 Custom Error Codes
| Code | Constant | Description |
|------|----------|-------------|
| 0 | `PROC_SUCCESS` | Success |
| -1 | `PROC_ERR_GENERIC` | General error |
| -2 | `PROC_ERR_NOMEM` | Out of memory |
| -3 | `PROC_ERR_INVALID_PID` | Invalid PID |
| -4 | `PROC_ERR_PID_EXISTS` | PID already exists |
| -5 | `PROC_ERR_PID_NOT_FOUND` | Process not found |
| -6 | `PROC_ERR_INVALID_STATE` | Invalid process state |
| -7 | `PROC_ERR_INVALID_PRIORITY` | Invalid priority |
| -8 | `PROC_ERR_INVALID_NAME` | Invalid process name |
| -9 | `PROC_ERR_QUEUE_FULL` | Queue is full |
| -10 | `PROC_ERR_QUEUE_EMPTY` | Queue is empty |
| -11 | `PROC_ERR_SCHEDULER` | Scheduler error |
| -12 | `PROC_ERR_PERMISSION` | Permission denied |

## 8. Usage Examples

### 8.1 Creating a Process
c
int create_process(int fd, const char *name, int priority) {
struct proc_create_req req;
pid_t pid = 0;
strncpy(req.name, name, sizeof(req.name)-1);
req.name[sizeof(req.name)-1] = '\0';
req.priority = priority;
req.parent_pid = 0;

if (ioctl(fd, PROC_MAN_CREATE_PROC, &req) < 0) {
    perror("Failed to create process");
    return -1;
}

pid = *((pid_t*)&req);
printf("Created process '%s' (PID=%d)\n", name, pid);
return pid;
}
### 8.2 Listing Processes
c
void list_processes(int fd) {
struct proc_list_req req;
struct proc_info_resp procs[100];
int i;
req.procs = procs;
req.max_count = 100;

if (ioctl(fd, PROC_MAN_LIST_PROCS, &req) < 0) {
    perror("Failed to list processes");
    return;
}

printf("Process List (%d processes):\n", req.count);
for (i = 0; i < req.count; i++) {
    printf("  PID=%d, Name=%s, State=%d, Priority=%d\n",
           procs[i].pid, procs[i].name,
           procs[i].state, procs[i].priority);
}
}
### 8.3 Triggering Scheduling
c
void schedule_processes(int fd, int iterations) {
for (int i = 0; i < iterations; i++) {
int next_pid;
if (ioctl(fd, PROC_MAN_SCHEDULE, &next_pid) < 0) {
    perror("Schedule failed");
    break;
}

if (next_pid > 0) {
    printf("Scheduled PID=%d\n", next_pid);
} else {
    printf("No process to run\n");
}

usleep(100000);  // 100ms delay
}
}
## 9. /proc Interface

### 9.1 Accessing /proc Interface
bash
View status
cat /proc/procman
Monitor in real-time
watch cat /proc/procman
### 9.2 Output Format
=== Process Manager Status ===
Resource Statistics:
Total processes: 5
Running: 1, Ready: 3, Blocked: 0, Zombie: 1
Total CPU time: 12345 jiffies
Total memory: 20480 bytes
Total I/O operations: 100
Scheduler Statistics (Priority):
Total schedules: 100
Context switches: 50
CPU utilization: 75.5%
Throughput: 10.2 processes/sec
Process List:
PID PPID Name State Priority CPU Time Memory Children
1 0 init READY 1 1000 4096 1
2 1 bash RUNNING 5 2000 8192 0
...
## 10. Best Practices

### 10.1 Error Handling
- Always check ioctl return values
- Use `procman_strerror()` for error messages
- Validate input parameters
- Handle memory allocation failures

### 10.2 Performance
- Batch operations when possible
- Use appropriate buffer sizes
- Minimize system calls
- Cache frequently accessed data

### 10.3 Security
- Validate all user inputs
- Check buffer boundaries
- Use appropriate permissions
- Sanitize process names