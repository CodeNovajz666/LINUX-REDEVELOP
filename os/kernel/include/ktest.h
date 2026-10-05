#ifndef KTEST_H
#define KTEST_H

/* returns number of failed tests */
int ktest_run_early(void);   /* pmm/heap/paging, before multitasking */
int ktest_run_mt(void);      /* scheduler/task/syscall, must run inside a task */
int ktest_run_fs(void);      /* ramfs/VFS/fd table, must run inside a task */
int ktest_run_vm(void);      /* vma/demand paging/swap/mmap, must run inside a task */
int ktest_run_smp(void);     /* topology/spinlock/cross-CPU/TLB IPI, after smp_release */
int ktest_run_disk(void);    /* driver model/FAT32/RTC (v0.8.0), after smp_release */
int ktest_run_uid(void);     /* users/login/permissions (v0.9.0), after the root is up */
int ktest_run_shell(void);   /* osh scripting plumbing (v1.0.0), after the root is up */
int ktest_failed_count(void);
int ktest_total_count(void);

#endif /* KTEST_H */
