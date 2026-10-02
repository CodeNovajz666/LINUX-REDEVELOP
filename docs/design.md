# Process Manager Design Document

> **[历史归档]** 本文档描述项目起点 —— Linux 内核模块 proc_manager
> （src/）：一个用伪造 PCB/伪造上下文切换模拟进程管理的教学模块，
> 并非当前的 Novix OS，仅作历史保留。
> 当前架构见 [os/docs/DESIGN.md](../os/docs/DESIGN.md)（含模块 → OS 映射表）。

## 1. Overview
This document describes the design and implementation of a process manager kernel module for Linux.

## 2. Architecture

### 2.1 High-Level Design
User Space Kernel Space
| |
| ioctl() calls |
|----------------------->| proc_manager.ko
| | |
| Responses | v
|<-----------------------| Process Table
| | |
| | v
Test Programs Scheduler -- PCB Linked Lists
| |
v v
Demonstration /proc/procman
### 2.2 Components
- **PCB Management**: Process Control Block data structures
- **Process Table**: Global process management
- **Scheduler**: Priority-based scheduling algorithms
- **Resource Tracker**: CPU, memory, I/O statistics
- **User Interface**: ioctl commands and /proc interface

## 3. Data Structures

### 3.1 Process Control Block (PCB)
c
struct pcb {
/* Identification */
pid_t pid; // Process ID
pid_t ppid; // Parent PID
char name[32]; // Process name
/* State and Scheduling */
proc_state_t state;    // Process state
int priority;          // Priority (1-10)
int time_slice;        // Remaining time slice

/* Statistics */
unsigned long cpu_time;     // CPU time used
unsigned long memory_usage; // Memory usage
unsigned long io_operations; // I/O count

/* Relationships */
struct pcb *parent;         // Parent process
struct list_head children;  // Child list
int child_count;           // Number of children

/* Kernel Lists */
struct list_head list;      // Global process list
struct list_head ready_list; // Ready queue

/* Synchronization */
spinlock_t lock;           // Per-PCB lock
};
### 3.2 Process States
┌─────────┐
│ NEW │
└────┬────┘
│ create()
▼
┌─────────┐
│ READY │◄─────┐
└────┬────┘ │
│ schedule()│ block()
▼ │
┌─────────┐ │
│ RUNNING │──────┘
└────┬────┘
│ exit()
▼
┌─────────┐
│ ZOMBIE │
└────┬────┘
│ reap()
▼
┌─────────┐
│TERMINATED
└─────────┘
## 4. Scheduling Algorithms

### 4.1 Priority Scheduling
- Higher priority processes run first
- Same priority: FIFO order
- Preemptive: Higher priority can preempt lower priority
- Aging: Prevents starvation

### 4.2 Round Robin
- Time-sliced scheduling
- Time quantum = priority * 10 ticks
- Preemptive when time slice expires

### 4.3 First-Come First-Served
- Simple non-preemptive scheduling
- Processes run in creation order
- Demonstrates convoy effect

## 5. User Interface

### 5.1 ioctl Commands
| Command | Code | Description |
|---------|------|-------------|
| CREATE_PROC | 0x7001 | Create new process |
| DESTROY_PROC | 0x7002 | Destroy process |
| LIST_PROCS | 0x7003 | List all processes |
| SET_STATE | 0x7004 | Change process state |
| SET_PRIORITY | 0x7005 | Change priority |
| SCHEDULE | 0x7006 | Trigger scheduling |
| GET_SCHED_STATS | 0x7007 | Get scheduler stats |

### 5.2 /proc Interface
- File: `/proc/procman`
- Read-only status information
- Process list and statistics
- Real-time monitoring

## 6. Error Handling

### 6.1 Error Codes
- `PROC_SUCCESS` (0): Success
- `PROC_ERR_NOMEM` (-ENOMEM): Out of memory
- `PROC_ERR_INVALID_PID` (-EINVAL): Invalid PID
- `PROC_ERR_PID_EXISTS` (-EEXIST): PID already exists
- `PROC_ERR_PID_NOT_FOUND` (-ENOENT): Process not found
- `PROC_ERR_INVALID_STATE` (-EINVAL): Invalid state
- `PROC_ERR_QUEUE_FULL` (-ENOSPC): Queue full

### 6.2 Validation
- Parameter validation for all user inputs
- State transition validation
- Resource limit checks
- Capacity limits (max 1024 processes)

## 7. Performance Considerations

### 7.1 Optimizations
- Lock-free read paths where possible
- Per-PCB locking for fine-grained control
- Efficient linked list operations
- Batch operations for multiple processes

### 7.2 Performance Targets
- Process creation: < 10μs
- Scheduling decision: < 1μs
- Context switch: < 5μs
- Maximum processes: 1024

## 8. Testing Strategy

### 8.1 Test Categories
1. **Unit Tests**: Individual functions
2. **Integration Tests**: Module interactions
3. **Stress Tests**: High load scenarios
4. **Boundary Tests**: Edge cases
5. **Memory Tests**: Leak detection

### 8.2 Test Programs
- `procman_test`: Basic functionality
- `procman_test_v2`: Enhanced testing
- `scheduler_test`: Scheduler algorithms
- `stress_test`: Performance testing

## 9. Security Considerations

### 9.1 Input Validation
- Validate all user-space parameters
- Check string lengths and null termination
- Validate numerical ranges
- Check pointer validity

### 9.2 Resource Limits
- Maximum process count: 1024
- Maximum name length: 31 characters
- Priority range: 1-10
- Memory limits per process

## 10. Future Enhancements

### 10.1 Planned Features
- Process groups and sessions
- Signal handling simulation
- Inter-process communication
- Memory management simulation
- File system operations

### 10.2 Performance Improvements
- Lock-free data structures
- RCU-based synchronization
- Per-CPU ready queues
- Hardware acceleration support