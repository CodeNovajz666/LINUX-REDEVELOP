#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include <stddef.h>

/*
 * Novix VFS -- a teaching-sized echo of Linux's split between
 * inode_operations (naming/tree structure) and file_operations (open-file
 * behaviour). Layering: namei.c walks the tree via i_op, vfs.c implements
 * open/read/write/lseek/mkdir/unlink on top, ramfs.c/devfs.c provide the
 * concrete operations.
 */

#define TASK_MAX_FD 16

/* open(2) flags (subset) */
#define O_ACCMODE 0x0003
#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_RDWR   0x0002
#define O_CREAT  0x0100
#define O_TRUNC  0x0200
#define O_APPEND 0x0400

/* lseek whence */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* inode mode: type bits. Permission bits are stored too but only become
 * meaningful when the uid model lands (0.7.0). */
#define S_IFMT  0xF000
#define S_IFCHR 0x2000
#define S_IFDIR 0x4000
#define S_IFREG 0x8000

struct stat {
    uint32_t st_ino;
    uint16_t st_mode;
    uint32_t st_size;
};

struct file;
struct inode;
struct task;

struct file_operations {
    int (*read)(struct file *f, void *buf, uint32_t len);
    int (*write)(struct file *f, const void *buf, uint32_t len);
    void (*close)(struct file *f);          /* release file-private state */
};

struct inode_operations {
    struct inode *(*lookup)(struct inode *dir, const char *name);
    struct inode *(*create)(struct inode *dir, const char *name, uint16_t mode);
    int (*mkdir)(struct inode *dir, const char *name);
    int (*unlink)(struct inode *dir, const char *name);
    int (*truncate)(struct inode *ino);
    /* enumerate a directory by index: 0 = entry copied out, 1 = end, -1 = err */
    int (*readdir)(struct inode *dir, int idx, char *name_out, size_t name_max);
};

struct inode {
    uint32_t ino;
    uint16_t mode;                          /* S_IF* */
    uint32_t size;
    void *i_priv;                           /* filesystem-private data */
    struct inode *i_parent;                 /* tree filesystems: ".." (root -> itself) */
    const struct inode_operations *i_op;    /* directories */
    const struct file_operations *f_op;     /* open-able files/devices */
};

struct file {
    struct inode *inode;
    uint32_t pos;
    uint16_t flags;
    uint16_t f_count;                          /* open-file refs (fork shares) */
};

/* ---- root filesystem (set once by ramfs_init) ---- */
void vfs_set_root(struct inode *root);
struct inode *vfs_get_root(void);

/* ---- path resolution (fs/namei.c) ---- */
struct inode *namei(const char *path);
/* resolve all but the last component; returns parent dir inode and copies
 * the final component into last[]. NULL if the parent path does not resolve
 * to a directory. */
struct inode *namei_parent(const char *path, char *last, size_t last_max);

/* ---- vfs interface (fs/vfs.c) ---- */
struct file *vfs_open(const char *path, uint16_t flags);
int vfs_read(struct file *f, void *buf, uint32_t len);
int vfs_write(struct file *f, const void *buf, uint32_t len);
int vfs_close(struct file *f);
int vfs_lseek(struct file *f, int32_t off, int whence);
int vfs_stat(const char *path, struct stat *st);
int vfs_mkdir(const char *path);
int vfs_unlink(const char *path);
/* enumerate a directory by index: 0 = entry copied out, 1 = end, -1 = err */
int vfs_readdir(const char *path, int idx, char *name_out, size_t name_max);

/* ---- per-task fd table (fs/vfs.c, operates on current task) ---- */
int fd_alloc(void);                         /* lowest free fd or -1 */
struct file *fd_get(int fd);
int fd_install(int fd, struct file *f);
void fd_close(int fd);
void task_close_files(struct task *t);      /* close every open fd */
void task_open_std_fds(struct task *t);     /* fd 0/1/2 -> /dev/console */
void vfs_dup_files(struct task *dst, const struct task *src); /* fork: share */

/* ---- pipes (fs/pipe.c, v0.5.0) ---- */
int pipe_create(int *rfd, int *wfd);        /* sys_pipe backend */

#endif /* VFS_H */
