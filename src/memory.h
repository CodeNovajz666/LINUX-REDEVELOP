#ifndef _MEMORY_H
#define _MEMORY_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/spinlock.h>

/* 内存分配策略 */
typedef enum {
    MEM_ALLOC_FIRST_FIT = 0,      /* 首次适应 */
    MEM_ALLOC_BEST_FIT = 1,       /* 最佳适应 */
    MEM_ALLOC_WORST_FIT = 2,      /* 最坏适应 */
    MEM_ALLOC_NEXT_FIT = 3        /* 下次适应 */
} mem_alloc_strategy_t;

/* 内存块状态 */
typedef enum {
    MEM_BLOCK_FREE = 0,            /* 空闲 */
    MEM_BLOCK_USED = 1,            /* 已使用 */
    MEM_BLOCK_RESERVED = 2         /* 保留 */
} mem_block_state_t;

/* 内存块结构 */
struct mem_block {
    unsigned long start;            /* 起始地址 */
    unsigned long size;             /* 大小（字节） */
    mem_block_state_t state;        /* 状态 */
    struct list_head list;          /* 链表节点 */
    struct mem_block *next;         /* 下一个内存块 */
    pid_t owner_pid;                /* 拥有者PID */
    int flags;                      /* 标志位 */
};

/* 内存区域类型 */
typedef enum {
    MEM_REGION_CODE = 0,            /* 代码段 */
    MEM_REGION_DATA = 1,            /* 数据段 */
    MEM_REGION_STACK = 2,           /* 栈段 */
    MEM_REGION_HEAP = 3,            /* 堆段 */
    MEM_REGION_MMAP = 4             /* 内存映射 */
} mem_region_type_t;

/* 内存区域 */
struct mem_region {
    unsigned long start;            /* 起始地址 */
    unsigned long size;             /* 大小（字节） */
    mem_region_type_t type;         /* 类型 */
    int permissions;                /* 权限（读/写/执行） */
    struct list_head list;          /* 链表节点 */
};

/* 进程内存空间 */
struct process_memory {
    pid_t pid;                      /* 进程PID */
    struct list_head regions;       /* 内存区域链表 */
    unsigned long total_size;       /* 总内存使用 */
    spinlock_t lock;                /* 锁 */
};

/* 内存管理器 */
struct memory_manager {
    struct list_head free_blocks;   /* 空闲块链表 */
    struct list_head used_blocks;   /* 已使用块链表 */
    mem_alloc_strategy_t strategy;  /* 分配策略 */
    unsigned long total_memory;     /* 总内存 */
    unsigned long free_memory;      /* 空闲内存 */
    spinlock_t lock;                /* 锁 */
    bool initialized;               /* 初始化状态 */
};

/* 内存管理器API */
int memory_manager_init(struct memory_manager *mm, unsigned long start, unsigned long size);
void memory_manager_cleanup(struct memory_manager *mm);
void *memory_allocate(struct memory_manager *mm, size_t size);
int memory_free(struct memory_manager *mm, void *ptr);
unsigned long memory_get_free(struct memory_manager *mm);
unsigned long memory_get_used(struct memory_manager *mm);

/* 进程内存管理API */
int process_memory_init(struct process_memory *pm, pid_t pid);
void process_memory_cleanup(struct process_memory *pm);
int process_memory_allocate(struct process_memory *pm, size_t size, mem_region_type_t type);
int process_memory_free(struct process_memory *pm, unsigned long addr);
int process_memory_map(struct process_memory *pm, unsigned long addr, size_t size, int permissions);
unsigned long process_memory_get_usage(struct process_memory *pm);

/* 内存页管理 */
#define PAGE_SIZE 4096
#define PAGE_ALIGN(addr) (((addr) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))

/* 内存相关工具函数 */
unsigned long get_page_count(size_t size);
bool is_page_aligned(unsigned long addr);

#endif /* _MEMORY_H */
