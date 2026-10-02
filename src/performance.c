#include <linux/slab.h>
#include <linux/printk.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/spinlock.h>
#include "pcb.h"
#include "scheduler.h"
#include "memory.h"

/* 优化的数据结构 */

/* 高效的进程查找表 */
struct process_hash_table {
    struct pcb **buckets;      /* 哈希桶 */
    int size;                 /* 表大小 */
    int count;                /* 进程数量 */
    spinlock_t lock;          /* 锁 */
};

/* 优先级队列（用于调度） */
struct priority_queue {
    struct pcb **heap;        /* 堆数组 */
    int size;                 /* 当前大小 */
    int capacity;             /* 容量 */
    spinlock_t lock;          /* 锁 */
};

/* 内存分配器优化 */
struct optimized_memory_allocator {
    struct mem_block *free_list;    /* 空闲块链表 */
    struct mem_block *used_list;    /* 已使用块链表 */
    unsigned long total_memory;     /* 总内存 */
    unsigned long free_memory;      /* 空闲内存 */
    spinlock_t lock;                /* 锁 */
};

/* 哈希函数 */
static unsigned int hash_pid(pid_t pid, int size) {
    return pid % size;
}

/* 初始化进程哈希表 */
int process_hash_table_init(struct process_hash_table *table, int size) {
    if (!table || size <= 0) {
        return -EINVAL;
    }
    
    table->buckets = kmalloc(sizeof(struct pcb *) * size, GFP_KERNEL);
    if (!table->buckets) {
        return -ENOMEM;
    }
    
    memset(table->buckets, 0, sizeof(struct pcb *) * size);
    table->size = size;
    table->count = 0;
    spin_lock_init(&table->lock);
    
    printk(KERN_INFO "process_hash_table_init: initialized with size %d\n", size);
    return 0;
}

/* 清理进程哈希表 */
void process_hash_table_cleanup(struct process_hash_table *table) {
    if (!table) {
        return;
    }
    
    if (table->buckets) {
        kfree(table->buckets);
    }
    
    printk(KERN_INFO "process_hash_table_cleanup: cleaned up\n");
}

/* 添加进程到哈希表 */
int process_hash_table_add(struct process_hash_table *table, struct pcb *pcb) {
    unsigned int index;
    unsigned long flags;
    
    if (!table || !pcb) {
        return -EINVAL;
    }
    
    index = hash_pid(pcb->pid, table->size);
    
    spin_lock_irqsave(&table->lock, flags);
    
    /* 简单的线性探测解决冲突 */
    int i = index;
    do {
        if (!table->buckets[i]) {
            table->buckets[i] = pcb;
            table->count++;
            spin_unlock_irqrestore(&table->lock, flags);
            return 0;
        }
        i = (i + 1) % table->size;
    } while (i != index);
    
    spin_unlock_irqrestore(&table->lock, flags);
    return -ENOMEM; /* 哈希表已满 */
}

/* 从哈希表中移除进程 */
int process_hash_table_remove(struct process_hash_table *table, pid_t pid) {
    unsigned int index;
    unsigned long flags;
    
    if (!table) {
        return -EINVAL;
    }
    
    index = hash_pid(pid, table->size);
    
    spin_lock_irqsave(&table->lock, flags);
    
    /* 线性探测查找 */
    int i = index;
    do {
        if (table->buckets[i] && table->buckets[i]->pid == pid) {
            table->buckets[i] = NULL;
            table->count--;
            spin_unlock_irqrestore(&table->lock, flags);
            return 0;
        }
        i = (i + 1) % table->size;
    } while (i != index);
    
    spin_unlock_irqrestore(&table->lock, flags);
    return -ENOENT;
}

/* 从哈希表中查找进程 */
struct pcb *process_hash_table_find(struct process_hash_table *table, pid_t pid) {
    unsigned int index;
    unsigned long flags;
    struct pcb *pcb = NULL;
    
    if (!table) {
        return NULL;
    }
    
    index = hash_pid(pid, table->size);
    
    spin_lock_irqsave(&table->lock, flags);
    
    /* 线性探测查找 */
    int i = index;
    do {
        if (table->buckets[i] && table->buckets[i]->pid == pid) {
            pcb = table->buckets[i];
            break;
        }
        i = (i + 1) % table->size;
    } while (i != index);
    
    spin_unlock_irqrestore(&table->lock, flags);
    return pcb;
}

/* 初始化优先级队列 */
int priority_queue_init(struct priority_queue *queue, int capacity) {
    if (!queue || capacity <= 0) {
        return -EINVAL;
    }
    
    queue->heap = kmalloc(sizeof(struct pcb *) * capacity, GFP_KERNEL);
    if (!queue->heap) {
        return -ENOMEM;
    }
    
    queue->size = 0;
    queue->capacity = capacity;
    spin_lock_init(&queue->lock);
    
    printk(KERN_INFO "priority_queue_init: initialized with capacity %d\n", capacity);
    return 0;
}

/* 清理优先级队列 */
void priority_queue_cleanup(struct priority_queue *queue) {
    if (!queue) {
        return;
    }
    
    if (queue->heap) {
        kfree(queue->heap);
    }
    
    printk(KERN_INFO "priority_queue_cleanup: cleaned up\n");
}

/* 堆化操作 */
static void heapify(struct priority_queue *queue, int index) {
    int smallest = index;
    int left = 2 * index + 1;
    int right = 2 * index + 2;
    
    /* 找出最小的节点 */
    if (left < queue->size && queue->heap[left]->priority < queue->heap[smallest]->priority) {
        smallest = left;
    }
    
    if (right < queue->size && queue->heap[right]->priority < queue->heap[smallest]->priority) {
        smallest = right;
    }
    
    /* 如果最小节点不是当前节点，交换并继续堆化 */
    if (smallest != index) {
        struct pcb *temp = queue->heap[index];
        queue->heap[index] = queue->heap[smallest];
        queue->heap[smallest] = temp;
        heapify(queue, smallest);
    }
}

/* 向优先级队列添加进程 */
int priority_queue_push(struct priority_queue *queue, struct pcb *pcb) {
    unsigned long flags;
    
    if (!queue || !pcb) {
        return -EINVAL;
    }
    
    spin_lock_irqsave(&queue->lock, flags);
    
    /* 检查队列是否已满 */
    if (queue->size >= queue->capacity) {
        spin_unlock_irqrestore(&queue->lock, flags);
        return -ENOMEM;
    }
    
    /* 添加到堆的末尾 */
    int i = queue->size;
    queue->heap[i] = pcb;
    queue->size++;
    
    /* 向上调整堆 */
    while (i > 0 && queue->heap[(i - 1) / 2]->priority > queue->heap[i]->priority) {
        struct pcb *temp = queue->heap[(i - 1) / 2];
        queue->heap[(i - 1) / 2] = queue->heap[i];
        queue->heap[i] = temp;
        i = (i - 1) / 2;
    }
    
    spin_unlock_irqrestore(&queue->lock, flags);
    return 0;
}

/* 从优先级队列中弹出进程 */
struct pcb *priority_queue_pop(struct priority_queue *queue) {
    unsigned long flags;
    struct pcb *pcb = NULL;
    
    if (!queue) {
        return NULL;
    }
    
    spin_lock_irqsave(&queue->lock, flags);
    
    if (queue->size > 0) {
        pcb = queue->heap[0];
        queue->size--;
        queue->heap[0] = queue->heap[queue->size];
        heapify(queue, 0);
    }
    
    spin_unlock_irqrestore(&queue->lock, flags);
    return pcb;
}

/* 初始化优化的内存分配器 */
int optimized_memory_allocator_init(struct optimized_memory_allocator *allocator, unsigned long size) {
    if (!allocator) {
        return -EINVAL;
    }
    
    /* 创建初始内存块 */
    struct mem_block *block = kmalloc(sizeof(struct mem_block), GFP_KERNEL);
    if (!block) {
        return -ENOMEM;
    }
    
    block->start = 0;
    block->size = size;
    block->state = MEM_BLOCK_FREE;
    block->owner_pid = -1;
    block->flags = 0;
    INIT_LIST_HEAD(&block->list);
    
    allocator->free_list = block;
    allocator->used_list = NULL;
    allocator->total_memory = size;
    allocator->free_memory = size;
    spin_lock_init(&allocator->lock);
    
    printk(KERN_INFO "optimized_memory_allocator_init: initialized with %lu bytes\n", size);
    return 0;
}

/* 清理优化的内存分配器 */
void optimized_memory_allocator_cleanup(struct optimized_memory_allocator *allocator) {
    struct mem_block *block, *tmp;
    
    if (!allocator) {
        return;
    }
    
    /* 清理空闲块 */
    block = allocator->free_list;
    while (block) {
        tmp = block;
        block = block->next;
        kfree(tmp);
    }
    
    /* 清理已使用块 */
    block = allocator->used_list;
    while (block) {
        tmp = block;
        block = block->next;
        kfree(tmp);
    }
    
    printk(KERN_INFO "optimized_memory_allocator_cleanup: cleaned up\n");
}

/* 优化的内存分配 */
void *optimized_memory_allocate(struct optimized_memory_allocator *allocator, size_t size) {
    struct mem_block *block, *best_block = NULL;
    unsigned long flags;
    void *ptr = NULL;
    
    if (!allocator || size == 0) {
        return NULL;
    }
    
    /* 对齐到页大小 */
    size = PAGE_ALIGN(size);
    
    spin_lock_irqsave(&allocator->lock, flags);
    
    /* 最佳适应算法 */
    block = allocator->free_list;
    while (block) {
        if (block->size >= size && (!best_block || block->size < best_block->size)) {
            best_block = block;
        }
        block = block->next;
    }
    
    if (best_block) {
        /* 分割块 */
        if (best_block->size > size) {
            struct mem_block *new_block = kmalloc(sizeof(struct mem_block), GFP_KERNEL);
            if (new_block) {
                new_block->start = best_block->start + size;
                new_block->size = best_block->size - size;
                new_block->state = MEM_BLOCK_FREE;
                new_block->owner_pid = -1;
                new_block->flags = 0;
                new_block->next = best_block->next;
                best_block->next = new_block;
                best_block->size = size;
            }
        }
        
        /* 标记为已使用 */
        best_block->state = MEM_BLOCK_USED;
        
        /* 从空闲链表移到已使用链表 */
        /* 这里需要实现链表操作 */
        
        allocator->free_memory -= best_block->size;
        ptr = (void *)best_block->start;
    }
    
    spin_unlock_irqrestore(&allocator->lock, flags);
    return ptr;
}

/* 优化的内存释放 */
int optimized_memory_free(struct optimized_memory_allocator *allocator, void *ptr) {
    struct mem_block *block, *prev;
    unsigned long flags;
    int found = 0;
    
    if (!allocator || !ptr) {
        return -EINVAL;
    }
    
    spin_lock_irqsave(&allocator->lock, flags);
    
    /* 查找要释放的块 */
    block = allocator->used_list;
    prev = NULL;
    while (block) {
        if ((void *)block->start == ptr) {
            /* 标记为空闲 */
            block->state = MEM_BLOCK_FREE;
            block->owner_pid = -1;
            
            /* 从已使用链表移到空闲链表 */
            /* 这里需要实现链表操作 */
            
            /* 合并相邻的空闲块 */
            /* 这里需要实现合并逻辑 */
            
            allocator->free_memory += block->size;
            found = 1;
            break;
        }
        prev = block;
        block = block->next;
    }
    
    spin_unlock_irqrestore(&allocator->lock, flags);
    return found ? 0 : -ENOENT;
}

/* 性能统计 */
struct performance_stats {
    unsigned long scheduling_latency;     /* 调度延迟 */
    unsigned long memory_allocation_time; /* 内存分配时间 */
    unsigned long process_creation_time;  /* 进程创建时间 */
    unsigned long context_switch_time;    /* 上下文切换时间 */
    unsigned long signal_handling_time;   /* 信号处理时间 */
};

/* 全局性能统计 */
static struct performance_stats g_performance_stats;

/* 记录性能数据 */
void record_performance_data(const char *event, unsigned long time) {
    if (strcmp(event, "scheduling") == 0) {
        g_performance_stats.scheduling_latency = time;
    } else if (strcmp(event, "memory_allocation") == 0) {
        g_performance_stats.memory_allocation_time = time;
    } else if (strcmp(event, "process_creation") == 0) {
        g_performance_stats.process_creation_time = time;
    } else if (strcmp(event, "context_switch") == 0) {
        g_performance_stats.context_switch_time = time;
    } else if (strcmp(event, "signal_handling") == 0) {
        g_performance_stats.signal_handling_time = time;
    }
}

/* 打印性能统计 */
void print_performance_stats(void) {
    printk(KERN_INFO "========== Performance Statistics ==========\n");
    printk(KERN_INFO "Scheduling latency: %lu ns\n", g_performance_stats.scheduling_latency);
    printk(KERN_INFO "Memory allocation time: %lu ns\n", g_performance_stats.memory_allocation_time);
    printk(KERN_INFO "Process creation time: %lu ns\n", g_performance_stats.process_creation_time);
    printk(KERN_INFO "Context switch time: %lu ns\n", g_performance_stats.context_switch_time);
    printk(KERN_INFO "Signal handling time: %lu ns\n", g_performance_stats.signal_handling_time);
    printk(KERN_INFO "==========================================\n");
}
