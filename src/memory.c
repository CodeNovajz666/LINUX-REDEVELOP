#include "memory.h"
#include <linux/slab.h>
#include <linux/printk.h>
#include <linux/errno.h>

/* 全局内存管理器 */
static struct memory_manager g_memory_manager;

/* 初始化内存管理器 */
int memory_manager_init(struct memory_manager *mm, unsigned long start, unsigned long size) {
    struct mem_block *initial_block;
    
    if (!mm) {
        return -EINVAL;
    }
    
    /* 初始化链表 */
    INIT_LIST_HEAD(&mm->free_blocks);
    INIT_LIST_HEAD(&mm->used_blocks);
    
    /* 初始化锁 */
    spin_lock_init(&mm->lock);
    
    /* 创建初始内存块 */
    initial_block = kmalloc(sizeof(struct mem_block), GFP_KERNEL);
    if (!initial_block) {
        printk(KERN_ERR "memory_manager_init: kmalloc failed\n");
        return -ENOMEM;
    }
    
    initial_block->start = start;
    initial_block->size = size;
    initial_block->state = MEM_BLOCK_FREE;
    initial_block->owner_pid = -1;
    initial_block->flags = 0;
    
    INIT_LIST_HEAD(&initial_block->list);
    list_add_tail(&initial_block->list, &mm->free_blocks);
    
    mm->strategy = MEM_ALLOC_FIRST_FIT;
    mm->total_memory = size;
    mm->free_memory = size;
    mm->initialized = true;
    
    printk(KERN_INFO "memory_manager_init: initialized with %lu bytes\n", size);
    return 0;
}

/* 清理内存管理器 */
void memory_manager_cleanup(struct memory_manager *mm) {
    struct mem_block *block, *tmp;
    struct list_head *pos, *q;
    
    if (!mm || !mm->initialized) {
        return;
    }
    
    /* 清理空闲块 */
    list_for_each_safe(pos, q, &mm->free_blocks) {
        block = list_entry(pos, struct mem_block, list);
        list_del(&block->list);
        kfree(block);
    }
    
    /* 清理已使用块 */
    list_for_each_safe(pos, q, &mm->used_blocks) {
        block = list_entry(pos, struct mem_block, list);
        list_del(&block->list);
        kfree(block);
    }
    
    mm->initialized = false;
    printk(KERN_INFO "memory_manager_cleanup: cleaned up\n");
}

/* 内存分配 */
void *memory_allocate(struct memory_manager *mm, size_t size) {
    struct mem_block *block, *new_block;
    struct list_head *pos;
    unsigned long flags;
    void *ptr = NULL;
    
    if (!mm || !mm->initialized || size == 0) {
        return NULL;
    }
    
    /* 对齐到页大小 */
    size = PAGE_ALIGN(size);
    
    spin_lock_irqsave(&mm->lock, flags);
    
    /* 查找合适的空闲块 */
    switch (mm->strategy) {
        case MEM_ALLOC_FIRST_FIT:
            list_for_each(pos, &mm->free_blocks) {
                block = list_entry(pos, struct mem_block, list);
                if (block->size >= size) {
                    ptr = (void *)block->start;
                    break;
                }
            }
            break;
            
        case MEM_ALLOC_BEST_FIT:
            {   
                struct mem_block *best_block = NULL;
                unsigned long best_size = -1;
                
                list_for_each(pos, &mm->free_blocks) {
                    block = list_entry(pos, struct mem_block, list);
                    if (block->size >= size && block->size < best_size) {
                        best_block = block;
                        best_size = block->size;
                    }
                }
                if (best_block) {
                    ptr = (void *)best_block->start;
                    block = best_block;
                }
            }
            break;
            
        case MEM_ALLOC_WORST_FIT:
            {   
                struct mem_block *worst_block = NULL;
                unsigned long worst_size = 0;
                
                list_for_each(pos, &mm->free_blocks) {
                    block = list_entry(pos, struct mem_block, list);
                    if (block->size >= size && block->size > worst_size) {
                        worst_block = block;
                        worst_size = block->size;
                    }
                }
                if (worst_block) {
                    ptr = (void *)worst_block->start;
                    block = worst_block;
                }
            }
            break;
            
        case MEM_ALLOC_NEXT_FIT:
            /* 实现下次适应算法 */
            static struct list_head *last_pos = NULL;
            if (!last_pos) {
                last_pos = &mm->free_blocks;
            }
            
            pos = last_pos->next;
            while (pos != last_pos) {
                block = list_entry(pos, struct mem_block, list);
                if (block->size >= size) {
                    ptr = (void *)block->start;
                    last_pos = pos;
                    break;
                }
                pos = pos->next;
            }
            
            if (!ptr) {
                pos = &mm->free_blocks->next;
                while (pos != last_pos) {
                    block = list_entry(pos, struct mem_block, list);
                    if (block->size >= size) {
                        ptr = (void *)block->start;
                        last_pos = pos;
                        break;
                    }
                    pos = pos->next;
                }
            }
            break;
    }
    
    if (ptr) {
        /* 从空闲链表中移除 */
        list_del(&block->list);
        
        /* 如果块大小大于请求大小，分割出剩余部分 */
        if (block->size > size) {
            new_block = kmalloc(sizeof(struct mem_block), GFP_KERNEL);
            if (new_block) {
                new_block->start = block->start + size;
                new_block->size = block->size - size;
                new_block->state = MEM_BLOCK_FREE;
                new_block->owner_pid = -1;
                new_block->flags = 0;
                INIT_LIST_HEAD(&new_block->list);
                list_add_tail(&new_block->list, &mm->free_blocks);
            }
            
            block->size = size;
        }
        
        /* 标记为已使用 */
        block->state = MEM_BLOCK_USED;
        list_add_tail(&block->list, &mm->used_blocks);
        
        mm->free_memory -= block->size;
        
        printk(KERN_DEBUG "memory_allocate: allocated %zu bytes at %p\n", size, ptr);
    } else {
        printk(KERN_WARNING "memory_allocate: no free memory for %zu bytes\n", size);
    }
    
    spin_unlock_irqrestore(&mm->lock, flags);
    return ptr;
}

/* 内存释放 */
int memory_free(struct memory_manager *mm, void *ptr) {
    struct mem_block *block, *tmp;
    struct list_head *pos, *q;
    unsigned long flags;
    int found = 0;
    
    if (!mm || !mm->initialized || !ptr) {
        return -EINVAL;
    }
    
    spin_lock_irqsave(&mm->lock, flags);
    
    /* 查找要释放的块 */
    list_for_each_safe(pos, q, &mm->used_blocks) {
        block = list_entry(pos, struct mem_block, list);
        if ((void *)block->start == ptr) {
            /* 从已使用链表中移除 */
            list_del(&block->list);
            block->state = MEM_BLOCK_FREE;
            block->owner_pid = -1;
            
            /* 合并相邻的空闲块 */
            /* 前向合并 */
            list_for_each_safe(pos, q, &mm->free_blocks) {
                tmp = list_entry(pos, struct mem_block, list);
                if (tmp->start + tmp->size == block->start) {
                    block->start = tmp->start;
                    block->size += tmp->size;
                    list_del(&tmp->list);
                    kfree(tmp);
                    break;
                }
            }
            
            /* 后向合并 */
            list_for_each_safe(pos, q, &mm->free_blocks) {
                tmp = list_entry(pos, struct mem_block, list);
                if (block->start + block->size == tmp->start) {
                    block->size += tmp->size;
                    list_del(&tmp->list);
                    kfree(tmp);
                    break;
                }
            }
            
            /* 添加到空闲链表 */
            list_add_tail(&block->list, &mm->free_blocks);
            mm->free_memory += block->size;
            
            found = 1;
            printk(KERN_DEBUG "memory_free: freed %lu bytes at %p\n", block->size, ptr);
            break;
        }
    }
    
    spin_unlock_irqrestore(&mm->lock, flags);
    
    return found ? 0 : -ENOENT;
}

/* 获取空闲内存 */
unsigned long memory_get_free(struct memory_manager *mm) {
    if (!mm || !mm->initialized) {
        return 0;
    }
    return mm->free_memory;
}

/* 获取已使用内存 */
unsigned long memory_get_used(struct memory_manager *mm) {
    if (!mm || !mm->initialized) {
        return 0;
    }
    return mm->total_memory - mm->free_memory;
}

/* 初始化进程内存 */
int process_memory_init(struct process_memory *pm, pid_t pid) {
    if (!pm) {
        return -EINVAL;
    }
    
    pm->pid = pid;
    INIT_LIST_HEAD(&pm->regions);
    pm->total_size = 0;
    spin_lock_init(&pm->lock);
    
    printk(KERN_INFO "process_memory_init: initialized for PID %d\n", pid);
    return 0;
}

/* 清理进程内存 */
void process_memory_cleanup(struct process_memory *pm) {
    struct mem_region *region, *tmp;
    struct list_head *pos, *q;
    
    if (!pm) {
        return;
    }
    
    list_for_each_safe(pos, q, &pm->regions) {
        region = list_entry(pos, struct mem_region, list);
        list_del(&region->list);
        kfree(region);
    }
    
    printk(KERN_INFO "process_memory_cleanup: cleaned up for PID %d\n", pm->pid);
}

/* 进程内存分配 */
int process_memory_allocate(struct process_memory *pm, size_t size, mem_region_type_t type) {
    struct mem_region *region;
    unsigned long flags;
    
    if (!pm) {
        return -EINVAL;
    }
    
    /* 对齐到页大小 */
    size = PAGE_ALIGN(size);
    
    /* 分配内存 */
    void *addr = memory_allocate(&g_memory_manager, size);
    if (!addr) {
        return -ENOMEM;
    }
    
    /* 创建内存区域 */
    region = kmalloc(sizeof(struct mem_region), GFP_KERNEL);
    if (!region) {
        memory_free(&g_memory_manager, addr);
        return -ENOMEM;
    }
    
    region->start = (unsigned long)addr;
    region->size = size;
    region->type = type;
    
    /* 设置权限 */
    switch (type) {
        case MEM_REGION_CODE:
            region->permissions = 0x7; /* 读/写/执行 */
            break;
        case MEM_REGION_DATA:
            region->permissions = 0x3; /* 读/写 */
            break;
        case MEM_REGION_STACK:
            region->permissions = 0x3; /* 读/写 */
            break;
        case MEM_REGION_HEAP:
            region->permissions = 0x3; /* 读/写 */
            break;
        case MEM_REGION_MMAP:
            region->permissions = 0x7; /* 读/写/执行 */
            break;
        default:
            region->permissions = 0x3; /* 读/写 */
            break;
    }
    
    INIT_LIST_HEAD(&region->list);
    
    spin_lock_irqsave(&pm->lock, flags);
    list_add_tail(&region->list, &pm->regions);
    pm->total_size += size;
    spin_unlock_irqrestore(&pm->lock, flags);
    
    printk(KERN_INFO "process_memory_allocate: PID %d allocated %zu bytes at %p (type=%d)\n",
           pm->pid, size, addr, type);
    return 0;
}

/* 进程内存释放 */
int process_memory_free(struct process_memory *pm, unsigned long addr) {
    struct mem_region *region, *tmp;
    struct list_head *pos, *q;
    unsigned long flags;
    int found = 0;
    
    if (!pm) {
        return -EINVAL;
    }
    
    spin_lock_irqsave(&pm->lock, flags);
    
    list_for_each_safe(pos, q, &pm->regions) {
        region = list_entry(pos, struct mem_region, list);
        if (region->start == addr) {
            /* 释放内存 */
            memory_free(&g_memory_manager, (void *)addr);
            
            /* 从链表中移除 */
            list_del(&region->list);
            pm->total_size -= region->size;
            kfree(region);
            
            found = 1;
            printk(KERN_INFO "process_memory_free: PID %d freed memory at %lx\n", pm->pid, addr);
            break;
        }
    }
    
    spin_unlock_irqrestore(&pm->lock, flags);
    return found ? 0 : -ENOENT;
}

/* 内存映射 */
int process_memory_map(struct process_memory *pm, unsigned long addr, size_t size, int permissions) {
    struct mem_region *region;
    unsigned long flags;
    
    if (!pm) {
        return -EINVAL;
    }
    
    /* 对齐到页大小 */
    size = PAGE_ALIGN(size);
    
    /* 创建内存区域 */
    region = kmalloc(sizeof(struct mem_region), GFP_KERNEL);
    if (!region) {
        return -ENOMEM;
    }
    
    region->start = addr;
    region->size = size;
    region->type = MEM_REGION_MMAP;
    region->permissions = permissions;
    
    INIT_LIST_HEAD(&region->list);
    
    spin_lock_irqsave(&pm->lock, flags);
    list_add_tail(&region->list, &pm->regions);
    pm->total_size += size;
    spin_unlock_irqrestore(&pm->lock, flags);
    
    printk(KERN_INFO "process_memory_map: PID %d mapped %zu bytes at %lx (permissions=%d)\n",
           pm->pid, size, addr, permissions);
    return 0;
}

/* 获取进程内存使用情况 */
unsigned long process_memory_get_usage(struct process_memory *pm) {
    if (!pm) {
        return 0;
    }
    return pm->total_size;
}

/* 获取页数量 */
unsigned long get_page_count(size_t size) {
    return (size + PAGE_SIZE - 1) / PAGE_SIZE;
}

/* 检查地址是否页对齐 */
bool is_page_aligned(unsigned long addr) {
    return (addr & (PAGE_SIZE - 1)) == 0;
}
