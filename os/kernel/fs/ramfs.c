#include "novix.h"
#include "ramfs.h"
#include "heap.h"
#include "lib.h"

/*
 * ramfs: a real (if small) filesystem whose storage is the kernel heap.
 * Nodes form a parent/child/sibling tree; each node embeds a VFS inode.
 * Data buffers grow by allocate-copy-free (no in-place realloc in the heap).
 *
 * Locking: tree mutations run under irq_save so a preempting syscall can
 * never observe a half-linked node. Lookups run unlocked -- on this single
 * core they cannot interleave with a mutation because they never block.
 */

struct ramfs_node {
    char name[32];
    uint16_t mode;                          /* S_IFREG / S_IFDIR / S_IFCHR */
    uint8_t *data;                          /* regular files */
    uint32_t size;
    uint32_t cap;
    struct ramfs_node *parent;
    struct ramfs_node *children;            /* first child */
    struct ramfs_node *sibling;             /* next sibling */
    struct inode inode;                     /* embedded VFS inode */
};

static struct ramfs_node *root;
static uint32_t next_ino = 1;

static struct ramfs_node *node_of(struct inode *ino) {
    return (struct ramfs_node *)ino->i_priv;
}

static void inode_sync(struct ramfs_node *n) {
    n->inode.size = n->size;
    n->inode.mode = n->mode;
}

static struct ramfs_node *child_find(struct ramfs_node *dir, const char *name) {
    for (struct ramfs_node *c = dir->children; c; c = c->sibling)
        if (strcmp(c->name, name) == 0) return c;
    return NULL;
}

static void child_link(struct ramfs_node *dir, struct ramfs_node *n) {
    n->parent = dir;
    n->sibling = dir->children;
    dir->children = n;
    n->inode.i_parent = &dir->inode;
}

static void child_unlink(struct ramfs_node *dir, struct ramfs_node *n) {
    struct ramfs_node **pp = &dir->children;
    while (*pp && *pp != n) pp = &(*pp)->sibling;
    if (*pp) *pp = n->sibling;
}

/* ---- regular-file operations ---- */

static int ramfs_f_read(struct file *f, void *buf, uint32_t len) {
    struct ramfs_node *n = node_of(f->inode);
    if (!n || (n->mode & S_IFMT) != S_IFREG) return -1;
    if (f->pos >= n->size) return 0;        /* EOF */
    uint32_t avail = n->size - f->pos;
    if (len > avail) len = avail;
    memcpy(buf, n->data + f->pos, len);
    f->pos += len;
    return (int)len;
}

static int ramfs_f_write(struct file *f, const void *buf, uint32_t len) {
    struct ramfs_node *n = node_of(f->inode);
    if (!n || (n->mode & S_IFMT) != S_IFREG) return -1;

    uint32_t end = f->pos + len;
    if (end > n->cap) {
        /* grow: allocate-copy-free */
        uint32_t newcap = n->cap ? n->cap : 64;
        while (newcap < end) newcap *= 2;
        uint8_t *nd = kmalloc(newcap);
        if (!nd) return -1;
        if (n->size) memcpy(nd, n->data, n->size);
        if (n->data) kfree(n->data);
        n->data = nd;
        n->cap = newcap;
    }
    if (f->pos > n->size)                   /* sparse gap: zero-fill */
        memset(n->data + n->size, 0, f->pos - n->size);
    memcpy(n->data + f->pos, buf, len);
    f->pos += len;
    if (f->pos > n->size) n->size = f->pos;
    inode_sync(n);
    return (int)len;
}

static const struct file_operations ramfs_file_fops = {
    .read = ramfs_f_read,
    .write = ramfs_f_write,
};

/* ---- directory operations ---- */

static struct inode *ramfs_i_lookup(struct inode *dir, const char *name);

static struct ramfs_node *node_new(const char *name, uint16_t mode);

static struct inode *ramfs_i_create(struct inode *dir, const char *name, uint16_t mode) {
    if ((dir->mode & S_IFMT) != S_IFDIR || !name[0] || strlen(name) >= 32)
        return NULL;
    if (child_find(node_of(dir), name)) return NULL;    /* already exists */
    irq_flags_t fl = irq_save();
    struct ramfs_node *n = node_new(name, mode);
    if (!n) { irq_restore(fl); return NULL; }
    if ((mode & S_IFMT) == S_IFDIR)
        n->inode.f_op = NULL;              /* directories are not openable */
    child_link(node_of(dir), n);
    irq_restore(fl);
    return &n->inode;
}

static int ramfs_i_mkdir(struct inode *dir, const char *name) {
    /* v0.9.0: rwxr-xr-x -- directories are searchable by everyone,
     * writable only by their owner (root for boot-created ones) */
    return ramfs_i_create(dir, name, S_IFDIR | 0755) ? 0 : -1;
}

static int ramfs_i_unlink(struct inode *dir, const char *name) {
    struct ramfs_node *d = node_of(dir);
    if ((dir->mode & S_IFMT) != S_IFDIR) return -1;
    irq_flags_t fl = irq_save();
    struct ramfs_node *n = child_find(d, name);
    if (!n || ((n->mode & S_IFMT) == S_IFDIR)) { irq_restore(fl); return -1; }  /* no rmdir yet */
    child_unlink(d, n);
    if (n->data) kfree(n->data);
    kfree(n);
    irq_restore(fl);
    return 0;
}

static int ramfs_i_truncate(struct inode *ino) {
    struct ramfs_node *n = node_of(ino);
    if (!n || (n->mode & S_IFMT) != S_IFREG) return -1;
    irq_flags_t fl = irq_save();
    if (n->data) { kfree(n->data); n->data = NULL; }
    n->size = n->cap = 0;
    inode_sync(n);
    irq_restore(fl);
    return 0;
}

/* v1.1.2 rename: relink the node under the new parent and rename it in
 * place -- the inode (and thus every open file and stat st_ino) survives,
 * which is the whole point of rename over create+unlink. The node stays
 * put when both parents are the same directory. */
static int ramfs_i_rename(struct inode *olddir, const char *oldname,
                          struct inode *newdir, const char *newname) {
    if ((olddir->mode & S_IFMT) != S_IFDIR || (newdir->mode & S_IFMT) != S_IFDIR)
        return -1;
    if (!oldname[0] || strlen(oldname) >= 32 ||
        !newname[0] || strlen(newname) >= 32)
        return -1;
    struct ramfs_node *od = node_of(olddir), *nd = node_of(newdir);
    irq_flags_t fl = irq_save();
    int rc = -1;
    do {
        struct ramfs_node *n = child_find(od, oldname);
        if (!n) break;
        if (child_find(nd, newname)) break;         /* EEXIST: never clobber */
        if ((n->mode & S_IFMT) == S_IFDIR) {
            /* cycle guard: a directory cannot move under itself */
            bool under = false;
            for (struct ramfs_node *a = nd; a; a = a->parent)
                if (a == n) { under = true; break; }
            if (under) break;
        }
        child_unlink(od, n);
        strncpy(n->name, newname, sizeof(n->name) - 1);
        n->name[sizeof(n->name) - 1] = 0;
        child_link(nd, n);                          /* reparents i_parent */
        rc = 0;
    } while (0);
    irq_restore(fl);
    return rc;
}

/* v1.1.2 rmdir: empty directories only; a mount point is busy by
 * definition (the mounted tree would be orphaned). */
static int ramfs_i_rmdir(struct inode *dir, const char *name) {
    if ((dir->mode & S_IFMT) != S_IFDIR) return -1;
    struct ramfs_node *d = node_of(dir);
    irq_flags_t fl = irq_save();
    int rc = -1;
    do {
        struct ramfs_node *n = child_find(d, name);
        if (!n || (n->mode & S_IFMT) != S_IFDIR) break;
        if (n->children) break;                     /* ENOTEMPTY */
        if (vfs_mount_count(&n->inode) > 0) break;  /* busy: a mount lives here */
        child_unlink(d, n);
        kfree(n);
        rc = 0;
    } while (0);
    irq_restore(fl);
    return rc;
}

static int ramfs_i_readdir(struct inode *dir, int idx, char *name_out, size_t name_max) {
    if ((dir->mode & S_IFMT) != S_IFDIR) return -1;
    struct ramfs_node *c = node_of(dir)->children;
    for (int i = 0; c && i < idx; i++) c = c->sibling;
    if (!c) return 1;                       /* end of directory */
    strncpy(name_out, c->name, name_max - 1);
    name_out[name_max - 1] = 0;
    return 0;
}

static const struct inode_operations ramfs_dir_iops = {
    .lookup = ramfs_i_lookup,
    .create = ramfs_i_create,
    .mkdir = ramfs_i_mkdir,
    .unlink = ramfs_i_unlink,
    .truncate = ramfs_i_truncate,
    .readdir = ramfs_i_readdir,
    .rename = ramfs_i_rename,
    .rmdir = ramfs_i_rmdir,
};

static struct ramfs_node *node_new(const char *name, uint16_t mode) {
    struct ramfs_node *n = kmalloc(sizeof(*n));
    if (!n) return NULL;
    memset(n, 0, sizeof(*n));
    strncpy(n->name, name, sizeof(n->name) - 1);
    n->mode = mode;
    n->inode.ino = next_ino++;
    n->inode.mode = mode;
    n->inode.i_priv = n;
    n->inode.i_op = &ramfs_dir_iops;
    n->inode.f_op = &ramfs_file_fops;
    return n;
}

static struct inode *ramfs_i_lookup(struct inode *dir, const char *name) {
    struct ramfs_node *c = child_find(node_of(dir), name);
    return c ? &c->inode : NULL;
}

/* ---- mount / public api ---- */

void ramfs_init(void) {
    root = node_new("", S_IFDIR | 0755);       /* v0.9.0: world-searchable */
    root->inode.f_op = NULL;
    root->inode.i_parent = &root->inode;    /* ".." at root stays at root */
    vfs_set_root(&root->inode);
}

int ramfs_mkdir(const char *path) {
    char last[32];
    struct inode *parent = namei_parent(path, last, sizeof(last));
    if (!parent || !parent->i_op || !parent->i_op->mkdir) return -1;
    return parent->i_op->mkdir(parent, last);
}

struct inode *ramfs_mknod(const char *path, uint16_t mode,
                          const struct file_operations *f_op) {
    char last[32];
    struct inode *parent = namei_parent(path, last, sizeof(last));
    if (!parent || !parent->i_op || !parent->i_op->create) return NULL;
    struct inode *ino = parent->i_op->create(parent, last, mode);
    if (ino) ino->f_op = f_op;
    return ino;
}

/* ---- initrd image unpacking (format written by tools/mkramfs.py) ---- */

#define RAMFS_IMG_MAGIC 0x4E524653u          /* 'NRFS' */

struct ramfs_img_hdr {
    uint32_t magic;
    uint32_t node_count;
    uint32_t data_size;
} __attribute__((packed));

struct ramfs_img_entry {
    char path[64];
    uint32_t offset;
    uint32_t size;
} __attribute__((packed));

static int ensure_dir(const char *path) {
    struct inode *ino = namei(path);
    if (ino) return ((ino->mode & S_IFMT) == S_IFDIR) ? 0 : -1;
    return ramfs_mkdir(path);
}

int ramfs_mount_initrd(const void *img, uint32_t size) {
    if (size < sizeof(struct ramfs_img_hdr)) return -1;
    const struct ramfs_img_hdr *h = img;
    if (h->magic != RAMFS_IMG_MAGIC) return -1;
    if (sizeof(*h) + h->node_count * sizeof(struct ramfs_img_entry) + h->data_size > size)
        return -1;
    const struct ramfs_img_entry *e =
        (const void *)((uintptr_t)img + sizeof(*h));
    const uint8_t *blob = (const uint8_t *)img + sizeof(*h) +
                          h->node_count * sizeof(struct ramfs_img_entry);

    int count = 0;
    for (uint32_t i = 0; i < h->node_count; i++) {
        char path[80];
        strncpy(path, e[i].path, sizeof(path) - 1);
        path[sizeof(path) - 1] = 0;
        if (!path[0]) continue;

        /* create intermediate directories */
        char dir[80];
        for (char *p = path + 1; *p; p++) {
            if (*p != '/') continue;
            uint32_t dlen = (uint32_t)(p - path);
            if (dlen >= sizeof(dir)) return -1;
            memcpy(dir, path, dlen);
            dir[dlen] = 0;
            if (ensure_dir(dir) != 0) return -1;
        }

        struct inode *ino = namei(path);
        if (!ino) {
            ino = ramfs_mknod(path, S_IFREG | 0644, &ramfs_file_fops);  /* v0.9.0 mode bits */
            if (!ino) return -1;
        }
        struct ramfs_node *n = node_of(ino);
        if ((n->mode & S_IFMT) != S_IFREG) continue;

        irq_flags_t fl = irq_save();
        if (e[i].size == 0) {              /* empty file: kmalloc(0) is NULL,
                                            * not an allocation failure */
            if (n->data) kfree(n->data);
            n->data = NULL;
            n->cap = n->size = 0;
        } else {
            uint8_t *nd = kmalloc(e[i].size);
            if (!nd) { irq_restore(fl); return -1; }
            memcpy(nd, blob + e[i].offset, e[i].size);
            if (n->data) kfree(n->data);
            n->data = nd;
            n->cap = n->size = e[i].size;
        }
        inode_sync(n);
        irq_restore(fl);
        count++;
    }
    return count;
}
