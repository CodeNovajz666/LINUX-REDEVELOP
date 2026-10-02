#ifndef _FILE_SYSTEM_H
#define _FILE_SYSTEM_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/spinlock.h>

/* 文件操作标志 */
#define O_RDONLY    0x0000  /* 只读 */
#define O_WRONLY    0x0001  /* 只写 */
#define O_RDWR      0x0002  /* 读写 */
#define O_CREAT     0x0040  /* 创建文件 */
#define O_EXCL      0x0080  /* 独占创建 */
#define O_TRUNC     0x0200  /* 截断文件 */
#define O_APPEND    0x0400  /* 追加模式 */

/* 文件权限 */
#define S_IRUSR     00400  /* 所有者读权限 */
#define S_IWUSR     00200  /* 所有者写权限 */
#define S_IXUSR     00100  /* 所有者执行权限 */
#define S_IRGRP     00040  /* 组读权限 */
#define S_IWGRP     00020  /* 组写权限 */
#define S_IXGRP     00010  /* 组执行权限 */
#define S_IROTH     00004  /* 其他读权限 */
#define S_IWOTH     00002  /* 其他写权限 */
#define S_IXOTH     00001  /* 其他执行权限 */

/* 文件类型 */
typedef enum {
    FILE_TYPE_REGULAR = 0,    /* 普通文件 */
    FILE_TYPE_DIR = 1,        /* 目录 */
    FILE_TYPE_SYMLINK = 2,    /* 符号链接 */
    FILE_TYPE_DEVICE = 3,      /* 设备文件 */
    FILE_TYPE_PIPE = 4         /* 管道文件 */
} file_type_t;

/* 文件状态结构 */
struct file_stat {
    unsigned long st_dev;     /* 设备号 */
    ino_t st_ino;             /* inode号 */
    mode_t st_mode;           /* 文件模式 */
    nlink_t st_nlink;         /* 硬链接数 */
    uid_t st_uid;             /* 所有者ID */
    gid_t st_gid;             /* 组ID */
    unsigned long st_rdev;    /* 设备类型 */
    off_t st_size;            /* 文件大小 */
    time_t st_atime;          /* 访问时间 */
    time_t st_mtime;          /* 修改时间 */
    time_t st_ctime;          /* 创建时间 */
};

/* 文件描述符表项 */
struct file_descriptor {
    int fd;                   /* 文件描述符 */
    struct file *file;        /* 文件指针 */
    off_t offset;             /* 文件偏移量 */
    int flags;                /* 打开标志 */
    struct list_head list;    /* 链表节点 */
};

/* 文件结构 */
struct file {
    char *name;               /* 文件名 */
    char *path;               /* 文件路径 */
    file_type_t type;         /* 文件类型 */
    mode_t mode;              /* 文件权限 */
    uid_t uid;                /* 所有者ID */
    gid_t gid;                /* 组ID */
    off_t size;               /* 文件大小 */
    time_t atime;             /* 访问时间 */
    time_t mtime;             /* 修改时间 */
    time_t ctime;             /* 创建时间 */
    int ref_count;            /* 引用计数 */
    struct list_head list;    /* 链表节点 */
    void *private_data;       /* 私有数据 */
};

/* 目录项 */
struct dir_entry {
    char *name;               /* 目录项名称 */
    file_type_t type;         /* 文件类型 */
    struct file *file;        /* 对应文件 */
    struct list_head list;    /* 链表节点 */
};

/* 目录结构 */
struct directory {
    char *name;               /* 目录名 */
    char *path;               /* 目录路径 */
    struct list_head entries; /* 目录项链表 */
    struct directory *parent; /* 父目录 */
    struct list_head list;    /* 链表节点 */
};

/* 文件系统操作 */
typedef struct {
    int (*open)(const char *path, int flags, mode_t mode);
    int (*close)(int fd);
    ssize_t (*read)(int fd, void *buf, size_t count);
    ssize_t (*write)(int fd, const void *buf, size_t count);
    off_t (*lseek)(int fd, off_t offset, int whence);
    int (*stat)(const char *path, struct file_stat *statbuf);
    int (*mkdir)(const char *path, mode_t mode);
    int (*rmdir)(const char *path);
    int (*unlink)(const char *path);
    int (*rename)(const char *oldpath, const char *newpath);
    int (*chmod)(const char *path, mode_t mode);
    int (*chown)(const char *path, uid_t owner, gid_t group);
} file_operations_t;

/* 进程文件描述符表 */
struct file_descriptor_table {
    struct list_head descriptors; /* 文件描述符链表 */
    int next_fd;                 /* 下一个可用的文件描述符 */
    spinlock_t lock;             /* 锁 */
};

/* 文件系统API */
int file_system_init(void);
void file_system_cleanup(void);
int fs_open(const char *path, int flags, mode_t mode);
int fs_close(int fd);
ssize_t fs_read(int fd, void *buf, size_t count);
ssize_t fs_write(int fd, const void *buf, size_t count);
off_t fs_lseek(int fd, off_t offset, int whence);
int fs_stat(const char *path, struct file_stat *statbuf);
int fs_mkdir(const char *path, mode_t mode);
int fs_rmdir(const char *path);
int fs_unlink(const char *path);
int fs_rename(const char *oldpath, const char *newpath);
int fs_chmod(const char *path, mode_t mode);
int fs_chown(const char *path, uid_t owner, gid_t group);

/* 进程文件描述符表API */
int file_descriptor_table_init(struct file_descriptor_table *fdt);
void file_descriptor_table_cleanup(struct file_descriptor_table *fdt);
int file_descriptor_table_add(struct file_descriptor_table *fdt, struct file *file, int flags);
int file_descriptor_table_remove(struct file_descriptor_table *fdt, int fd);
struct file_descriptor *file_descriptor_table_find(struct file_descriptor_table *fdt, int fd);

#endif /* _FILE_SYSTEM_H */
