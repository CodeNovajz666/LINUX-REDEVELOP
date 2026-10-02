#ifndef _PROCESS_H
#define _PROCESS_H

#include "pcb.h"

/* 进程管理函数 */
int process_change_state(struct process_table *ptable, pid_t pid, proc_state_t new_state);
int process_set_priority(struct process_table *ptable, pid_t pid, int priority);
int process_wakeup_all_blocked(struct process_table *ptable);
int process_kill_all(struct process_table *ptable);

/* 进程列表操作 */
int process_get_list(struct process_table *ptable, struct pcb **buffer, int max_count);
int process_get_ready_list(struct process_table *ptable, struct pcb **buffer, int max_count);
int process_get_running(struct process_table *ptable, struct pcb **buffer);

/* 进程统计 */
unsigned long process_get_total_cpu_time(struct process_table *ptable);
unsigned long process_get_total_memory(struct process_table *ptable);
int process_get_count_by_state(struct process_table *ptable, proc_state_t state);

/* 进程关系 */
int process_add_child(struct process_table *ptable, pid_t parent_pid, pid_t child_pid);
int process_remove_child(struct process_table *ptable, pid_t parent_pid, pid_t child_pid);
int process_get_children(struct process_table *ptable, pid_t parent_pid, 
                         pid_t *children, int max_count);

#endif /* _PROCESS_H */