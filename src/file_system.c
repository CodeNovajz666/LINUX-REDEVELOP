#include "file_system.h"
#include <linux/slab.h>
#include <linux/printk.h>
#include <linux/errno.h>
#include <linux/time.h>
#include <linux/string.h>

/* 全局文件系统状态 */
static struct file_system {
    struct list_head files;          /* 所有文件链表 */
    struct list_head directories;    /* 所有目录链表 */
    struct directory *root;          /* 根目录 */
    spinlock_t lock;                 /* 锁 */
    bool initialized;                /* 初始化状态 */
} g_file_system;

/* 全局文件描述符表 */
static struct file_descriptor_table g_global_fdt;

/* 初始化文件系统 */
int file_system_init(void) {
    struct directory *root_dir;
    
    /* 初始化链表 */
    INIT_LIST_HEAD(&g_file_system.files);
    INIT_LIST_HEAD(&g_file_system.directories);
    
    /* 初始化锁 */
    spin_lock_init(&g_file_system.lock);
    
    /* 创建根目录 */
    root_dir = kmalloc(sizeof(struct directory), GFP_KERNEL);
    if (!root_dir) {
        printk(KERN_ERR "file_system_init: kmalloc failed\n");
        return -ENOMEM;
    }
    
    root_dir->name = kmalloc(2, GFP_KERNEL);
    if (!root_dir->name) {
        kfree(root_dir);
        printk(KERN_ERR "file_system_init: kmalloc failed\n");
        return -ENOMEM;
    }
    strcpy(root_dir->name, "/");
    
    root_dir->path = kmalloc(2, GFP_KERNEL);
    if (!root_dir->path) {
        kfree(root_dir->name);
        kfree(root_dir);
        printk(KERN_ERR "file_system_init: kmalloc failed\n");
        return -ENOMEM;
    }
    strcpy(root_dir->path, "/");
    
    INIT_LIST_HEAD(&root_dir->entries);
    root_dir->parent = NULL;
    INIT_LIST_HEAD(&root_dir->list);
    
    list_add_tail(&root_dir->list, &g_file_system.directories);
    g_file_system.root = root_dir;
    
    /* 初始化全局文件描述符表 */
    if (file_descriptor_table_init(&g_global_fdt) != 0) {
        printk(KERN_ERR "file_system_init: failed to initialize file descriptor table\n");
        file_system_cleanup();
        return -EINVAL;
    }
    
    g_file_system.initialized = true;
    printk(KERN_INFO "file_system_init: initialized\n");
    return 0;
}

/* 清理文件系统 */
void file_system_cleanup(void) {
    struct file *file, *file_tmp;
    struct directory *dir, *dir_tmp;
    struct dir_entry *entry, *entry_tmp;
    struct list_head *pos, *q;
    
    if (!g_file_system.initialized) {
        return;
    }
    
    /* 清理所有文件 */
    list_for_each_safe(pos, q, &g_file_system.files) {
        file = list_entry(pos, struct file, list);
        list_del(&file->list);
        if (file->name) kfree(file->name);
        if (file->path) kfree(file->path);
        kfree(file);
    }
    
    /* 清理所有目录 */
    list_for_each_safe(pos, q, &g_file_system.directories) {
        dir = list_entry(pos, struct directory, list);
        
        /* 清理目录项 */
        list_for_each_safe(pos, q, &dir->entries) {
            entry = list_entry(pos, struct dir_entry, list);
            list_del(&entry->list);
            if (entry->name) kfree(entry->name);
            kfree(entry);
        }
        
        list_del(&dir->list);
        if (dir->name) kfree(dir->name);
        if (dir->path) kfree(dir->path);
        kfree(dir);
    }
    
    /* 清理文件描述符表 */
    file_descriptor_table_cleanup(&g_global_fdt);
    
    g_file_system.initialized = false;
    printk(KERN_INFO "file_system_cleanup: cleaned up\n");
}

/* 打开文件 */
int fs_open(const char *path, int flags, mode_t mode) {
    struct file *file = NULL;
    struct directory *dir = g_file_system.root;
    struct dir_entry *entry;
    int fd;
    unsigned long flags_lock;
    
    if (!g_file_system.initialized || !path) {
        return -EINVAL;
    }
    
    spin_lock_irqsave(&g_file_system.lock, flags_lock);
    
    /* 查找文件 */
    /* 简化实现：直接在根目录查找 */
    list_for_each_entry(entry, &dir->entries, list) {
        if (strcmp(entry->name, path) == 0) {
            file = entry->file;
            break;
        }
    }
    
    /* 如果文件不存在且有创建标志 */
    if (!file && (flags & O_CREAT)) {
        /* 创建新文件 */
        file = kmalloc(sizeof(struct file), GFP_KERNEL);
        if (!file) {
            spin_unlock_irqrestore(&g_file_system.lock, flags_lock);
            return -ENOMEM;
        }
        
        file->name = kmalloc(strlen(path) + 1, GFP_KERNEL);
        if (!file->name) {
            kfree(file);
            spin_unlock_irqrestore(&g_file_system.lock, flags_lock);
            return -ENOMEM;
        }
        strcpy(file->name, path);
        
        file->path = kmalloc(strlen(path) + 1, GFP_KERNEL);
        if (!file->path) {
            kfree(file->name);
            kfree(file);
            spin_unlock_irqrestore(&g_file_system.lock, flags_lock);
            return -ENOMEM;
        }
        strcpy(file->path, path);
        
        file->type = FILE_TYPE_REGULAR;
        file->mode = mode;
        file->uid = 0; /* 默认为root */
        file->gid = 0;
        file->size = 0;
        
        /* 设置时间戳 */
        struct timespec64 ts;
        ktime_get_real_ts64(&ts);
        file->atime = ts.tv_sec;
        file->mtime = ts.tv_sec;
        file->ctime = ts.tv_sec;
        
        file->ref_count = 0;
        INIT_LIST_HEAD(&file->list);
        
        /* 添加到文件链表 */
        list_add_tail(&file->list, &g_file_system.files);
        
        /* 添加到目录项 */
        entry = kmalloc(sizeof(struct dir_entry), GFP_KERNEL);
        if (!entry) {
            list_del(&file->list);
            kfree(file->path);
            kfree(file->name);
            kfree(file);
            spin_unlock_irqrestore(&g_file_system.lock, flags_lock);
            return -ENOMEM;
        }
        
        entry->name = kmalloc(strlen(path) + 1, GFP_KERNEL);
        if (!entry->name) {
            kfree(entry);
            list_del(&file->list);
            kfree(file->path);
            kfree(file->name);
            kfree(file);
            spin_unlock_irqrestore(&g_file_system.lock, flags_lock);
            return -ENOMEM;
        }
        strcpy(entry->name, path);
        entry->type = FILE_TYPE_REGULAR;
        entry->file = file;
        INIT_LIST_HEAD(&entry->list);
        list_add_tail(&entry->list, &dir->entries);
        
        printk(KERN_INFO "fs_open: created file '%s'\n", path);
    }
    
    if (!file) {
        spin_unlock_irqrestore(&g_file_system.lock, flags_lock);
        return -ENOENT;
    }
    
    /* 增加引用计数 */
    file->ref_count++;
    
    spin_unlock_irqrestore(&g_file_system.lock, flags_lock);
    
    /* 添加到文件描述符表 */
    fd = file_descriptor_table_add(&g_global_fdt, file, flags);
    if (fd < 0) {
        spin_lock_irqsave(&g_file_system.lock, flags_lock);
        file->ref_count--;
        spin_unlock_irqrestore(&g_file_system.lock, flags_lock);
        return fd;
    }
    
    printk(KERN_INFO "fs_open: opened file '%s' with fd %d\n", path, fd);
    return fd;
}

/* 关闭文件 */
int fs_close(int fd) {
    struct file_descriptor *fd_entry;
    struct file *file;
    unsigned long flags_lock;
    
    if (!g_file_system.initialized) {
        return -EINVAL;
    }
    
    /* 查找文件描述符 */
    fd_entry = file_descriptor_table_find(&g_global_fdt, fd);
    if (!fd_entry) {
        return -EBADF;
    }
    
    file = fd_entry->file;
    
    /* 从文件描述符表中移除 */
    if (file_descriptor_table_remove(&g_global_fdt, fd) != 0) {
        return -EINVAL;
    }
    
    /* 减少引用计数 */
    spin_lock_irqsave(&g_file_system.lock, flags_lock);
    file->ref_count--;
    
    /* 如果引用计数为0，释放文件 */
    if (file->ref_count == 0) {
        struct directory *dir = g_file_system.root;
        struct dir_entry *entry, *tmp;
        struct list_head *pos, *q;
        
        /* 从目录项中移除 */
        list_for_each_safe(pos, q, &dir->entries) {
            entry = list_entry(pos, struct dir_entry, list);
            if (entry->file == file) {
                list_del(&entry->list);
                if (entry->name) kfree(entry->name);
                kfree(entry);
                break;
            }
        }
        
        /* 从文件链表中移除 */
        list_del(&file->list);
        if (file->name) kfree(file->name);
        if (file->path) kfree(file->path);
        kfree(file);
        
        printk(KERN_INFO "fs_close: freed file with fd %d\n", fd);
    }
    
    spin_unlock_irqrestore(&g_file_system.lock, flags_lock);
    
    printk(KERN_INFO "fs_close: closed fd %d\n", fd);
    return 0;
}

/* 读取文件 */
ssize_t fs_read(int fd, void *buf, size_t count) {
    struct file_descriptor *fd_entry;
    
    if (!g_file_system.initialized || !buf) {
        return -EINVAL;
    }
    
    /* 查找文件描述符 */
    fd_entry = file_descriptor_table_find(&g_global_fdt, fd);
    if (!fd_entry) {
        return -EBADF;
    }
    
    /* 简化实现：返回0字节读取 */
    printk(KERN_INFO "fs_read: read %zu bytes from fd %d\n", count, fd);
    return 0;
}

/* 写入文件 */
ssize_t fs_write(int fd, const void *buf, size_t count) {
    struct file_descriptor *fd_entry;
    struct file *file;
    unsigned long flags_lock;
    
    if (!g_file_system.initialized || !buf) {
        return -EINVAL;
    }
    
    /* 查找文件描述符 */
    fd_entry = file_descriptor_table_find(&g_global_fdt, fd);
    if (!fd_entry) {
        return -EBADF;
    }
    
    file = fd_entry->file;
    
    /* 更新文件大小 */
    spin_lock_irqsave(&g_file_system.lock, flags_lock);
    if (fd_entry->offset + count > file->size) {
        file->size = fd_entry->offset + count;
    }
    
    /* 更新修改时间 */
    struct timespec64 ts;
    ktime_get_real_ts64(&ts);
    file->mtime = ts.tv_sec;
    
    spin_unlock_irqrestore(&g_file_system.lock, flags_lock);
    
    /* 更新文件偏移量 */
    fd_entry->offset += count;
    
    printk(KERN_INFO "fs_write: wrote %zu bytes to fd %d\n", count, fd);
    return count;
}

/* 定位文件指针 */
off_t fs_lseek(int fd, off_t offset, int whence) {
    struct file_descriptor *fd_entry;
    struct file *file;
    off_t new_offset;
    
    if (!g_file_system.initialized) {
        return -EINVAL;
    }
    
    /* 查找文件描述符 */
    fd_entry = file_descriptor_table_find(&g_global_fdt, fd);
    if (!fd_entry) {
        return -EBADF;
    }
    
    file = fd_entry->file;
    
    /* 计算新的偏移量 */
    switch (whence) {
        case SEEK_SET:
            new_offset = offset;
            break;
        case SEEK_CUR:
            new_offset = fd_entry->offset + offset;
            break;
        case SEEK_END:
            new_offset = file->size + offset;
            break;
        default:
            return -EINVAL;
    }
    
    /* 检查偏移量是否有效 */
    if (new_offset < 0) {
        return -EINVAL;
    }
    
    fd_entry->offset = new_offset;
    printk(KERN_INFO "fs_lseek: fd %d seeked to %lld\n", fd, (long long)new_offset);
    return new_offset;
}

/* 获取文件状态 */
int fs_stat(const char *path, struct file_stat *statbuf) {
    struct directory *dir = g_file_system.root;
    struct dir_entry *entry;
    struct file *file = NULL;
    
    if (!g_file_system.initialized || !path || !statbuf) {
        return -EINVAL;
    }
    
    /* 查找文件 */
    list_for_each_entry(entry, &dir->entries, list) {
        if (strcmp(entry->name, path) == 0) {
            file = entry->file;
            break;
        }
    }
    
    if (!file) {
        return -ENOENT;
    }
    
    /* 填充状态信息 */
    statbuf->st_dev = 0;
    statbuf->st_ino = (ino_t)file;
    statbuf->st_mode = file->mode;
    statbuf->st_nlink = 1;
    statbuf->st_uid = file->uid;
    statbuf->st_gid = file->gid;
    statbuf->st_rdev = 0;
    statbuf->st_size = file->size;
    statbuf->st_atime = file->atime;
    statbuf->st_mtime = file->mtime;
    statbuf->st_ctime = file->ctime;
    
    printk(KERN_INFO "fs_stat: stat'ed file '%s'\n", path);
    return 0;
}

/* 创建目录 */
int fs_mkdir(const char *path, mode_t mode) {
    struct directory *new_dir;
    struct directory *parent_dir = g_file_system.root;
    struct dir_entry *entry;
    unsigned long flags_lock;
    
    if (!g_file_system.initialized || !path) {
        return -EINVAL;
    }
    
    spin_lock_irqsave(&g_file_system.lock, flags_lock);
    
    /* 检查目录是否已存在 */
    list_for_each_entry(entry, &parent_dir->entries, list) {
        if (strcmp(entry->name, path) == 0 && entry->type == FILE_TYPE_DIR) {
            spin_unlock_irqrestore(&g_file_system.lock, flags_lock);
            return -EEXIST;
        }
    }
    
    /* 创建新目录 */
    new_dir = kmalloc(sizeof(struct directory), GFP_KERNEL);
    if (!new_dir) {
        spin_unlock_irqrestore(&g_file_system.lock, flags_lock);
        return -ENOMEM;
    }
    
    new_dir->name = kmalloc(strlen(path) + 1, GFP_KERNEL);
    if (!new_dir->name) {
        kfree(new_dir);
        spin_unlock_irqrestore(&g_file_system.lock, flags_lock);
        return -ENOMEM;
    }
    strcpy(new_dir->name, path);
    
    new_dir->path = kmalloc(strlen(path) + 1, GFP_KERNEL);
    if (!new_dir->path) {
        kfree(new_dir->name);
        kfree(new_dir);
        spin_unlock_irqrestore(&g_file_system.lock, flags_lock);
        return -ENOMEM;
    }
    strcpy(new_dir->path, path);
    
    INIT_LIST_HEAD(&new_dir->entries);
    new_dir->parent = parent_dir;
    INIT_LIST_HEAD(&new_dir->list);
    
    /* 添加到目录链表 */
    list_add_tail(&new_dir->list, &g_file_system.directories);
    
    /* 添加到父目录的目录项 */
    entry = kmalloc(sizeof(struct dir_entry), GFP_KERNEL);
    if (!entry) {
        list_del(&new_dir->list);
        kfree(new_dir->path);
        kfree(new_dir->name);
        kfree(new_dir);
        spin_unlock_irqrestore(&g_file_system.lock, flags_lock);
        return -ENOMEM;
    }
    
    entry->name = kmalloc(strlen(path) + 1, GFP_KERNEL);
    if (!entry->name) {
        kfree(entry);
        list_del(&new_dir->list);
        kfree(new_dir->path);
        kfree(new_dir->name);
        kfree(new_dir);
        spin_unlock_irqrestore(&g_file_system.lock, flags_lock);
        return -ENOMEM;
    }
    strcpy(entry->name, path);
    entry->type = FILE_TYPE_DIR;
    entry->file = NULL;
    INIT_LIST_HEAD(&entry->list);
    list_add_tail(&entry->list, &parent_dir->entries);
    
    spin_unlock_irqrestore(&g_file_system.lock, flags_lock);
    
    printk(KERN_INFO "fs_mkdir: created directory '%s'\n", path);
    return 0;
}

/* 删除目录 */
int fs_rmdir(const char *path) {
    struct directory *parent_dir = g_file_system.root;
    struct dir_entry *entry, *tmp;
    struct directory *dir = NULL;
    struct list_head *pos, *q;
    unsigned long flags_lock;
    
    if (!g_file_system.initialized || !path) {
        return -EINVAL;
    }
    
    spin_lock_irqsave(&g_file_system.lock, flags_lock);
    
    /* 查找目录 */
    list_for_each_entry(entry, &parent_dir->entries, list) {
        if (strcmp(entry->name, path) == 0 && entry->type == FILE_TYPE_DIR) {
            /* 查找对应的目录结构 */
            list_for_each_entry(dir, &g_file_system.directories, list) {
                if (strcmp(dir->name, path) == 0) {
                    break;
                }
            }
            break;
        }
    }
    
    if (!dir) {
        spin_unlock_irqrestore(&g_file_system.lock, flags_lock);
        return -ENOENT;
    }
    
    /* 检查目录是否为空 */
    if (!list_empty(&dir->entries)) {
        spin_unlock_irqrestore(&g_file_system.lock, flags_lock);
        return -ENOTEMPTY;
    }
    
    /* 从父目录中移除 */
    list_for_each_safe(pos, q, &parent_dir->entries) {
        tmp = list_entry(pos, struct dir_entry, list);
        if (strcmp(tmp->name, path) == 0) {
            list_del(&tmp->list);
            if (tmp->name) kfree(tmp->name);
            kfree(tmp);
            break;
        }
    }
    
    /* 从目录链表中移除 */
    list_del(&dir->list);
    if (dir->name) kfree(dir->name);
    if (dir->path) kfree(dir->path);
    kfree(dir);
    
    spin_unlock_irqrestore(&g_file_system.lock, flags_lock);
    
    printk(KERN_INFO "fs_rmdir: removed directory '%s'\n", path);
    return 0;
}

/* 删除文件 */
int fs_unlink(const char *path) {
    struct directory *dir = g_file_system.root;
    struct dir_entry *entry, *tmp;
    struct file *file = NULL;
    struct list_head *pos, *q;
    unsigned long flags_lock;
    
    if (!g_file_system.initialized || !path) {
        return -EINVAL;
    }
    
    spin_lock_irqsave(&g_file_system.lock, flags_lock);
    
    /* 查找文件 */
    list_for_each_entry(entry, &dir->entries, list) {
        if (strcmp(entry->name, path) == 0 && entry->type == FILE_TYPE_REGULAR) {
            file = entry->file;
            break;
        }
    }
    
    if (!file) {
        spin_unlock_irqrestore(&g_file_system.lock, flags_lock);
        return -ENOENT;
    }
    
    /* 检查文件是否被打开 */
    if (file->ref_count > 0) {
        spin_unlock_irqrestore(&g_file_system.lock, flags_lock);
        return -EBUSY;
    }
    
    /* 从目录项中移除 */
    list_for_each_safe(pos, q, &dir->entries) {
        tmp = list_entry(pos, struct dir_entry, list);
        if (tmp->file == file) {
            list_del(&tmp->list);
            if (tmp->name) kfree(tmp->name);
            kfree(tmp);
            break;
        }
    }
    
    /* 从文件链表中移除 */
    list_del(&file->list);
    if (file->name) kfree(file->name);
    if (file->path) kfree(file->path);
    kfree(file);
    
    spin_unlock_irqrestore(&g_file_system.lock, flags_lock);
    
    printk(KERN_INFO "fs_unlink: removed file '%s'\n", path);
    return 0;
}

/* 重命名文件 */
int fs_rename(const char *oldpath, const char *newpath) {
    struct directory *dir = g_file_system.root;
    struct dir_entry *entry, *tmp;
    struct file *file = NULL;
    struct list_head *pos, *q;
    unsigned long flags_lock;
    
    if (!g_file_system.initialized || !oldpath || !newpath) {
        return -EINVAL;
    }
    
    spin_lock_irqsave(&g_file_system.lock, flags_lock);
    
    /* 查找旧文件 */
    list_for_each_entry(entry, &dir->entries, list) {
        if (strcmp(entry->name, oldpath) == 0) {
            file = entry->file;
            break;
        }
    }
    
    if (!file) {
        spin_unlock_irqrestore(&g_file_system.lock, flags_lock);
        return -ENOENT;
    }
    
    /* 检查新文件名是否已存在 */
    list_for_each_entry(entry, &dir->entries, list) {
        if (strcmp(entry->name, newpath) == 0) {
            spin_unlock_irqrestore(&g_file_system.lock, flags_lock);
            return -EEXIST;
        }
    }
    
    /* 重命名文件 */
    list_for_each_safe(pos, q, &dir->entries) {
        tmp = list_entry(pos, struct dir_entry, list);
        if (tmp->file == file) {
            /* 更新目录项名称 */
            kfree(tmp->name);
            tmp->name = kmalloc(strlen(newpath) + 1, GFP_KERNEL);
            if (!tmp->name) {
                spin_unlock_irqrestore(&g_file_system.lock, flags_lock);
                return -ENOMEM;
            }
            strcpy(tmp->name, newpath);
            
            /* 更新文件名 */
            kfree(file->name);
            file->name = kmalloc(strlen(newpath) + 1, GFP_KERNEL);
            if (!file->name) {
                spin_unlock_irqrestore(&g_file_system.lock, flags_lock);
                return -ENOMEM;
            }
            strcpy(file->name, newpath);
            
            /* 更新文件路径 */
            kfree(file->path);
            file->path = kmalloc(strlen(newpath) + 1, GFP_KERNEL);
            if (!file->path) {
                spin_unlock_irqrestore(&g_file_system.lock, flags_lock);
                return -ENOMEM;
            }
            strcpy(file->path, newpath);
            
            break;
        }
    }
    
    spin_unlock_irqrestore(&g_file_system.lock, flags_lock);
    
    printk(KERN_INFO "fs_rename: renamed '%s' to '%s'\n", oldpath, newpath);
    return 0;
}

/* 修改文件权限 */
int fs_chmod(const char *path, mode_t mode) {
    struct directory *dir = g_file_system.root;
    struct dir_entry *entry;
    struct file *file = NULL;
    unsigned long flags_lock;
    
    if (!g_file_system.initialized || !path) {
        return -EINVAL;
    }
    
    spin_lock_irqsave(&g_file_system.lock, flags_lock);
    
    /* 查找文件 */
    list_for_each_entry(entry, &dir->entries, list) {
        if (strcmp(entry->name, path) == 0) {
            file = entry->file;
            break;
        }
    }
    
    if (!file) {
        spin_unlock_irqrestore(&g_file_system.lock, flags_lock);
        return -ENOENT;
    }
    
    /* 修改权限 */
    file->mode = mode;
    
    spin_unlock_irqrestore(&g_file_system.lock, flags_lock);
    
    printk(KERN_INFO "fs_chmod: changed mode of '%s' to %o\n", path, mode);
    return 0;
}

/* 修改文件所有者 */
int fs_chown(const char *path, uid_t owner, gid_t group) {
    struct directory *dir = g_file_system.root;
    struct dir_entry *entry;
    struct file *file = NULL;
    unsigned long flags_lock;
    
    if (!g_file_system.initialized || !path) {
        return -EINVAL;
    }
    
    spin_lock_irqsave(&g_file_system.lock, flags_lock);
    
    /* 查找文件 */
    list_for_each_entry(entry, &dir->entries, list) {
        if (strcmp(entry->name, path) == 0) {
            file = entry->file;
            break;
        }
    }
    
    if (!file) {
        spin_unlock_irqrestore(&g_file_system.lock, flags_lock);
        return -ENOENT;
    }
    
    /* 修改所有者 */
    file->uid = owner;
    file->gid = group;
    
    spin_unlock_irqrestore(&g_file_system.lock, flags_lock);
    
    printk(KERN_INFO "fs_chown: changed owner of '%s' to %u:%u\n", path, owner, group);
    return 0;
}

/* 初始化文件描述符表 */
int file_descriptor_table_init(struct file_descriptor_table *fdt) {
    if (!fdt) {
        return -EINVAL;
    }
    
    INIT_LIST_HEAD(&fdt->descriptors);
    fdt->next_fd = 3; /* 0,1,2 分别是 stdin, stdout, stderr */
    spin_lock_init(&fdt->lock);
    
    return 0;
}

/* 清理文件描述符表 */
void file_descriptor_table_cleanup(struct file_descriptor_table *fdt) {
    struct file_descriptor *fd_entry, *tmp;
    struct list_head *pos, *q;
    
    if (!fdt) {
        return;
    }
    
    list_for_each_safe(pos, q, &fdt->descriptors) {
        fd_entry = list_entry(pos, struct file_descriptor, list);
        list_del(&fd_entry->list);
        kfree(fd_entry);
    }
}

/* 添加文件描述符 */
int file_descriptor_table_add(struct file_descriptor_table *fdt, struct file *file, int flags) {
    struct file_descriptor *fd_entry;
    int fd;
    unsigned long flags_lock;
    
    if (!fdt || !file) {
        return -EINVAL;
    }
    
    fd_entry = kmalloc(sizeof(struct file_descriptor), GFP_KERNEL);
    if (!fd_entry) {
        return -ENOMEM;
    }
    
    spin_lock_irqsave(&fdt->lock, flags_lock);
    
    fd = fdt->next_fd++;
    if (fdt->next_fd < 0) {
        fdt->next_fd = 3;
    }
    
    fd_entry->fd = fd;
    fd_entry->file = file;
    fd_entry->offset = 0;
    fd_entry->flags = flags;
    INIT_LIST_HEAD(&fd_entry->list);
    
    list_add_tail(&fd_entry->list, &fdt->descriptors);
    
    spin_unlock_irqrestore(&fdt->lock, flags_lock);
    
    return fd;
}

/* 移除文件描述符 */
int file_descriptor_table_remove(struct file_descriptor_table *fdt, int fd) {
    struct file_descriptor *fd_entry, *tmp;
    struct list_head *pos, *q;
    unsigned long flags_lock;
    int found = 0;
    
    if (!fdt) {
        return -EINVAL;
    }
    
    spin_lock_irqsave(&fdt->lock, flags_lock);
    
    list_for_each_safe(pos, q, &fdt->descriptors) {
        fd_entry = list_entry(pos, struct file_descriptor, list);
        if (fd_entry->fd == fd) {
            list_del(&fd_entry->list);
            kfree(fd_entry);
            found = 1;
            break;
        }
    }
    
    spin_unlock_irqrestore(&fdt->lock, flags_lock);
    
    return found ? 0 : -EBADF;
}

/* 查找文件描述符 */
struct file_descriptor *file_descriptor_table_find(struct file_descriptor_table *fdt, int fd) {
    struct file_descriptor *fd_entry;
    unsigned long flags_lock;
    
    if (!fdt) {
        return NULL;
    }
    
    spin_lock_irqsave(&fdt->lock, flags_lock);
    
    list_for_each_entry(fd_entry, &fdt->descriptors, list) {
        if (fd_entry->fd == fd) {
            spin_unlock_irqrestore(&fdt->lock, flags_lock);
            return fd_entry;
        }
    }
    
    spin_unlock_irqrestore(&fdt->lock, flags_lock);
    return NULL;
}
