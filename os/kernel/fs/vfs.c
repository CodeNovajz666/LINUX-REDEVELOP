#include "novix.h"
#include "vfs.h"
#include "task.h"
#include "heap.h"
#include "lib.h"

/*
 * VFS core: open-file handling on top of inode/file operations, plus the
 * per-task fd table. Everything here is filesystem-agnostic -- ramfs and
 * devfs both plug in through f_op/i_op.
 */

static struct file *file_new(struct inode *ino, uint16_t flags) {
    struct file *f = kmalloc(sizeof(*f));
    if (!f) return NULL;
    f->inode = ino;
    f->pos = 0;
    f->flags = flags;
    f->f_count = 1;
    return f;
}

/* v0.9.0: the Unix permission decision. uid 0 bypasses everything (root);
 * otherwise exactly one of the owner/group/world bit triples applies --
 * owner wins over group wins over world, per POSIX.
 * v1.1.7: the ids-parameterised core lets access(2) answer with the REAL
 * ids while every other check keeps using the effective pair. */
int vfs_permitted_ids(const struct inode *ino, uint16_t want,
                      uint16_t uid, uint16_t gid) {
    if (!ino) return 0;
    if (uid == 0) return 1;
    uint16_t bits;
    if (ino->uid == uid)          bits = (ino->mode >> 6) & 7;
    else if (ino->gid == gid)     bits = (ino->mode >> 3) & 7;
    else                          bits = ino->mode & 7;
    return (bits & want) == want;
}

int vfs_permitted(const struct inode *ino, uint16_t want) {
    struct task *cur = current_task();
    return vfs_permitted_ids(ino, want,
                             cur ? cur->uid : 0,        /* boot ctx: root */
                             cur ? cur->gid : 0);
}

struct file *vfs_open(const char *path, uint16_t flags) {
    struct inode *ino = namei(path);
    if (!ino) {
        if (!(flags & O_CREAT)) return NULL;
        char last[32];
        struct inode *parent = namei_parent(path, last, sizeof(last));
        if (!parent || !parent->i_op || !parent->i_op->create) return NULL;
        /* v0.9.0: creating a name needs write+search on the parent
         * directory (the walk only X-checked the ancestors) */
        if (!vfs_permitted(parent, VFS_PERM_W | VFS_PERM_X)) return NULL;
        ino = parent->i_op->create(parent, last, S_IFREG | 0644);
        if (!ino) return NULL;
        /* new files belong to their creator (root where no task context) */
        struct task *cur = current_task();
        ino->uid = cur ? cur->uid : 0;
        ino->gid = cur ? cur->gid : 0;
    }

    if ((ino->mode & S_IFMT) == S_IFDIR) return NULL;   /* directories are not openable */

    uint16_t acc = flags & O_ACCMODE;       /* 0=RD 1=WR 2=RDWR */
    /* v0.9.0: the access mode is checked against the file's permission bits
     * at open time (later reads/writes ride on the fd's mode, like Unix) */
    if ((acc == O_RDONLY || acc == O_RDWR) && !vfs_permitted(ino, VFS_PERM_R))
        return NULL;
    if ((acc == O_WRONLY || acc == O_RDWR) && !vfs_permitted(ino, VFS_PERM_W))
        return NULL;
    if ((flags & (O_TRUNC | O_APPEND)) && !vfs_permitted(ino, VFS_PERM_W))
        return NULL;

    if (acc == O_RDONLY && (!ino->f_op || !ino->f_op->read)) return NULL;
    if (acc != O_RDONLY && (!ino->f_op || !ino->f_op->write)) return NULL;

    if ((flags & O_TRUNC) && acc != O_RDONLY && ino->i_op && ino->i_op->truncate)
        ino->i_op->truncate(ino);

    struct file *f = file_new(ino, flags);
    if (!f) return NULL;
    if (flags & O_APPEND) f->pos = ino->size;
    return f;
}

int vfs_read(struct file *f, void *buf, uint32_t len) {
    if (!f || !f->inode || !f->inode->f_op || !f->inode->f_op->read) return -1;
    if (f->flags & O_WRONLY) return -1;     /* not opened for reading */
    return f->inode->f_op->read(f, buf, len);
}

int vfs_write(struct file *f, const void *buf, uint32_t len) {
    if (!f || !f->inode || !f->inode->f_op || !f->inode->f_op->write) return -1;
    if ((f->flags & O_ACCMODE) == O_RDONLY) return -1;
    return f->inode->f_op->write(f, buf, len);
}

int vfs_close(struct file *f) {
    if (!f) return -1;
    /* fork shares the struct (Unix open-file description semantics: one
     * shared file offset); the last closer releases it (v0.4.0) */
    if (f->f_count > 1) {
        f->f_count--;
        return 0;
    }
    if (f->inode && f->inode->f_op && f->inode->f_op->close)
        f->inode->f_op->close(f);
    kfree(f);
    return 0;
}

int vfs_lseek(struct file *f, int32_t off, int whence) {
    if (!f || !f->inode) return -1;
    if ((f->inode->mode & S_IFMT) != S_IFREG) return -1;   /* pipes/devices: no seek */
    uint32_t size = f->inode->size;
    uint32_t np;
    switch (whence) {
    case SEEK_SET: np = (uint32_t)off; break;
    case SEEK_CUR: np = f->pos + (uint32_t)off; break;
    case SEEK_END: np = size + (uint32_t)off; break;
    default: return -1;
    }
    if (np > size) return -1;               /* no seeking past EOF (yet) */
    f->pos = np;
    return (int)np;
}

int vfs_stat(const char *path, struct stat *st) {
    struct inode *ino = namei(path);
    if (!ino || !st) return -1;
    st->st_ino = ino->ino;
    st->st_mode = ino->mode;
    st->st_size = ino->size;
    return 0;
}

int vfs_mkdir(const char *path) {
    char last[32];
    struct inode *parent = namei_parent(path, last, sizeof(last));
    if (!parent || !parent->i_op || !parent->i_op->mkdir) return -1;
    /* v0.9.0: like create, a new subdirectory needs write+search on the
     * parent (the walk only X-checked the ancestors) */
    if (!vfs_permitted(parent, VFS_PERM_W | VFS_PERM_X)) return -1;
    if (parent->i_op->mkdir(parent, last) != 0) return -1;
    /* v1.1.2: the new directory belongs to its creator -- the same rule
     * vfs_open applies to created files (before, mkdir'd dirs stayed owned
     * by root and a mortal could not create anything inside a directory
     * it had just made; rmdir still worked, so tests never caught it) */
    struct inode *n = namei(path);
    if (n && (n->mode & S_IFMT) == S_IFDIR) {
        struct task *cur = current_task();
        n->uid = cur ? cur->uid : 0;
        n->gid = cur ? cur->gid : 0;
    }
    return 0;
}

int vfs_unlink(const char *path) {
    char last[32];
    struct inode *parent = namei_parent(path, last, sizeof(last));
    if (!parent || !parent->i_op || !parent->i_op->unlink) return -1;
    /* v0.9.0: removing a name needs write+search on the parent directory */
    if (!vfs_permitted(parent, VFS_PERM_W | VFS_PERM_X)) return -1;
    return parent->i_op->unlink(parent, last);
}

/* v1.1.2 rename(2): both parents need W+X (the name is destroyed in one
 * and created in the other); the operation resolves on the OLD parent's
 * fs, and implementations reject a new parent on another instance. The
 * target name must not exist (Unix rename would clobber it; teaching
 * scope returns -1 instead -- no data is ever silently destroyed). */
int vfs_rename(const char *oldpath, const char *newpath) {
    char oldn[32], newn[32];
    struct inode *olddir = namei_parent(oldpath, oldn, sizeof(oldn));
    struct inode *newdir = namei_parent(newpath, newn, sizeof(newn));
    if (!olddir || !newdir) return -1;
    if (!olddir->i_op || !olddir->i_op->rename) return -1;
    /* different inode_operations = different filesystem: dispatching a
     * ramfs rename with a FAT newdir would feed the op a foreign inode
     * (v1.1.2 lesson: the op table is the fs-type identity). Two mounts
     * of the SAME type still reach the op, which checks its own instance. */
    if (newdir->i_op != olddir->i_op) return -1;
    if (!vfs_permitted(olddir, VFS_PERM_W | VFS_PERM_X)) return -1;
    if (!vfs_permitted(newdir, VFS_PERM_W | VFS_PERM_X)) return -1;
    return olddir->i_op->rename(olddir, oldn, newdir, newn);
}

/* v1.1.2 rmdir(2): W+X on the parent; the fs itself refuses anything but
 * an empty directory (mount points count as busy in ramfs). */
int vfs_rmdir(const char *path) {
    char last[32];
    struct inode *parent = namei_parent(path, last, sizeof(last));
    if (!parent || !parent->i_op || !parent->i_op->rmdir) return -1;
    if (!vfs_permitted(parent, VFS_PERM_W | VFS_PERM_X)) return -1;
    return parent->i_op->rmdir(parent, last);
}

int vfs_readdir(const char *path, int idx, char *name_out, size_t name_max) {
    struct inode *ino = namei(path);
    if (!ino || !name_out || name_max == 0) return -1;
    /* v0.9.0: enumerating a directory is search permission on it */
    if (!vfs_permitted(ino, VFS_PERM_X)) return -1;

    /* mounts attached to this directory come first in the listing, so a
     * mountpoint is visible in its parent (ls / shows the pinned /dev and
     * /tmp under the FAT32 root, exactly as ramfs showed its real /dev) */
    int nm = vfs_mount_count(ino);
    if (idx < nm) {
        const char *n = vfs_mount_name_at(ino, idx);
        if (!n) return -1;
        strncpy(name_out, n, name_max - 1);
        name_out[name_max - 1] = 0;
        return 0;
    }
    if (!ino->i_op || !ino->i_op->readdir) return -1;
    return ino->i_op->readdir(ino, idx - nm, name_out, name_max);
}

/* ---- per-task fd table ---- */

int fd_alloc(void) {
    struct task *t = current_task();
    if (!t) return -1;
    for (int i = 0; i < TASK_MAX_FD; i++)
        if (!t->fd[i]) return i;
    return -1;
}

struct file *fd_get(int fd) {
    struct task *t = current_task();
    if (!t || fd < 0 || fd >= TASK_MAX_FD) return NULL;
    return t->fd[fd];
}

int fd_install(int fd, struct file *f) {
    struct task *t = current_task();
    if (!t || fd < 0 || fd >= TASK_MAX_FD || !f) return -1;
    t->fd[fd] = f;
    return 0;
}

void fd_close(int fd) {
    struct task *t = current_task();
    if (!t || fd < 0 || fd >= TASK_MAX_FD) return;
    if (t->fd[fd]) {
        vfs_close(t->fd[fd]);
        t->fd[fd] = NULL;
    }
}

void task_close_files(struct task *t) {
    if (!t) return;
    for (int i = 0; i < TASK_MAX_FD; i++) {
        if (t->fd[i]) {
            vfs_close(t->fd[i]);
            t->fd[i] = NULL;
        }
    }
}

/* fork's dup_files: the child shares the parent's open-file descriptions
 * (same struct file, shared offset), each with a bumped refcount */
void vfs_dup_files(struct task *dst, const struct task *src) {
    if (!dst || !src) return;
    for (int i = 0; i < TASK_MAX_FD; i++) {
        struct file *f = src->fd[i];
        dst->fd[i] = f;
        if (f) f->f_count++;
    }
}

/* open /dev/console as fd 0/1/2 -- the Unix stdio trio */
void task_open_std_fds(struct task *t) {
    if (!t) return;
    for (int i = 0; i < 3; i++) {
        struct file *f = vfs_open("/dev/console", O_RDWR);
        if (!f) return;
        t->fd[i] = f;
    }
}
