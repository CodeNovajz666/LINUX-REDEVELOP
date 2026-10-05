#include "novix.h"
#include "vfs.h"
#include "task.h"
#include "lib.h"

/*
 * Path resolution. v0.9.0: relative paths resolve against the current
 * task's cwd (kernel threads carry "/", the boot context resolves from the
 * root). "." is skipped, ".." climbs to the parent but never above the
 * root, and any component that fails lookup aborts the walk. Every step
 * into a directory requires search (X) permission -- the walk is the
 * permission checkpoint Unix calls "path traversal".
 *
 * v0.8.0: a small mount table overlays other filesystems onto this tree
 * (the FAT32 root over the ramfs root, ramfs /dev & /tmp under the FAT
 * root). Mount points win over whatever the parent directory itself
 * would answer for that name.
 */

static struct inode *root_inode_holder;     /* see vfs.c setter/getter */
void vfs_set_root(struct inode *root) { root_inode_holder = root; }
struct inode *vfs_get_root(void) { return root_inode_holder; }

/* ---- mount table (v0.8.0) ---- */

#define VFS_MAX_MOUNTS 8

struct vfs_mount {
    struct inode *parent;
    char name[16];
    struct inode *root;
};
static struct vfs_mount mounts[VFS_MAX_MOUNTS];
static int nmounts;

int vfs_mount(struct inode *parent, const char *name, struct inode *fs_root) {
    if (!parent || !name || !name[0] || !fs_root) return -1;
    if (strlen(name) >= sizeof(mounts[0].name)) return -1;
    for (int i = 0; i < nmounts; i++)
        if (mounts[i].parent == parent && strcmp(mounts[i].name, name) == 0)
            return -1;                          /* already mounted here */
    if (nmounts >= VFS_MAX_MOUNTS) return -1;

    struct vfs_mount *m = &mounts[nmounts++];
    m->parent = parent;
    strcpy(m->name, name);
    m->root = fs_root;
    fs_root->i_parent = parent;                 /* ".." climbs out */
    return 0;
}

/* undo one mount (the power-cycle test unmounts the disk root's pins
 * before dropping the FAT32 instance they point into) */
int vfs_unmount(struct inode *parent, const char *name) {
    for (int i = 0; i < nmounts; i++) {
        if (mounts[i].parent == parent && strcmp(mounts[i].name, name) == 0) {
            mounts[i] = mounts[nmounts - 1];
            nmounts--;
            return 0;
        }
    }
    return -1;
}

/* the mount over (dir, name), or NULL */
static struct inode *mounted_at(struct inode *dir, const char *name) {
    for (int i = 0; i < nmounts; i++)
        if (mounts[i].parent == dir && strcmp(mounts[i].name, name) == 0)
            return mounts[i].root;
    return NULL;
}

/* mounts attached to dir: how many, and name by index -- readdir lists them
 * (a mountpoint is visible in its parent directory, like every real OS) */
int vfs_mount_count(struct inode *dir) {
    int n = 0;
    for (int i = 0; i < nmounts; i++)
        if (mounts[i].parent == dir) n++;
    return n;
}

const char *vfs_mount_name_at(struct inode *dir, int idx) {
    for (int i = 0; i < nmounts; i++) {
        if (mounts[i].parent != dir) continue;
        if (idx-- == 0) return mounts[i].name;
    }
    return NULL;
}

static struct inode *walk_component(struct inode *dir, const char *comp, size_t len) {
    if (!dir || (dir->mode & S_IFMT) != S_IFDIR)
        return NULL;
    /* v0.9.0: descending into a name is search permission on the directory
     * (root bypasses inside vfs_permitted) */
    if (!vfs_permitted(dir, VFS_PERM_X))
        return NULL;
    char name[32];
    if (len == 0 || len >= sizeof(name)) return NULL;
    memcpy(name, comp, len);
    name[len] = 0;

    struct inode *mounted = mounted_at(dir, name);
    if (mounted) return mounted;                /* mounts shadow the fs */

    if (!dir->i_op || !dir->i_op->lookup)
        return NULL;
    return dir->i_op->lookup(dir, name);
}

/* copy the next '/'-delimited component into comp[]; returns its length,
 * or -1 at end of string. Advances *pp past the component and its slash. */
static int next_component(const char **pp, const char **comp) {
    const char *p = *pp;
    while (*p == '/') p++;
    if (!*p) return -1;
    *comp = p;
    while (*p && *p != '/') p++;
    *pp = p;
    return (int)(p - *comp);
}

/* v0.9.0: where a path starts walking -- the root for absolute paths, the
 * current task's cwd (resolved recursively; it is always absolute) for
 * relative ones. Kernel threads and the boot context both start at "/". */
static struct inode *path_start(const char *path) {
    if (path[0] == '/') return vfs_get_root();
    struct task *t = current_task();
    const char *cwd = (t && t->cwd[0]) ? t->cwd : "/";
    if (cwd[0] == '/' && cwd[1] == 0) return vfs_get_root();
    return namei(cwd);
}

struct inode *namei(const char *path) {
    if (!path || !path[0]) return NULL;
    struct inode *cur = path_start(path);
    if (!cur) return NULL;

    const char *p = path;
    const char *comp;
    int len;
    while ((len = next_component(&p, &comp)) >= 0) {
        if (len == 1 && comp[0] == '.') continue;
        if (len == 2 && comp[0] == '.' && comp[1] == '.') {
            if (!cur->i_parent) return NULL;    /* fs without tree parents */
            cur = cur->i_parent;                /* root's parent is root */
            continue;
        }
        cur = walk_component(cur, comp, (size_t)len);
        if (!cur) return NULL;
    }
    return cur;
}

struct inode *namei_parent(const char *path, char *last, size_t last_max) {
    if (!path || !path[0]) return NULL;
    struct inode *cur = path_start(path);
    if (!cur) return NULL;

    const char *p = path;
    const char *comp;
    int len;
    while ((len = next_component(&p, &comp)) >= 0) {
        bool is_last = true;
        const char *q = p;
        while (*q == '/') q++;
        if (*q) is_last = false;

        if (is_last) {
            /* "."/".." name the directory itself (POSIX: EEXIST/EINVAL) --
             * never a creatable name; handing them to the fs would slip
             * past the exists-check (walkers skip the real ./.. entries)
             * and mint a second dot dirent */
            if (len == 1 && comp[0] == '.') return NULL;
            if (len == 2 && comp[0] == '.' && comp[1] == '.') return NULL;
            if ((size_t)len >= last_max) return NULL;
            memcpy(last, comp, (size_t)len);
            last[len] = 0;
            return cur;
        }
        if (len == 1 && comp[0] == '.') continue;
        if (len == 2 && comp[0] == '.' && comp[1] == '.') {
            if (!cur->i_parent) return NULL;
            cur = cur->i_parent;
            continue;
        }
        cur = walk_component(cur, comp, (size_t)len);
        if (!cur) return NULL;
    }
    return NULL;                            /* path was "/" or empty tail */
}

/* v0.9.0: lexical canonicalisation of `path` against `base` -- no symlinks
 * exist, so resolving "." and ".." by text is exact. Returns 0 and writes
 * an absolute path into out[], or -1 when it cannot fit. */
int vfs_canonicalize(const char *base, const char *path, char *out, size_t out_max) {
    if (!path || !path[0] || out_max < 2) return -1;

    char tmp[168];
    size_t n = 0;
    if (path[0] != '/') {
        const char *b = (base && base[0]) ? base : "/";
        while (*b && n < sizeof(tmp)) tmp[n++] = *b++;
        if (n == 0 || tmp[n - 1] != '/')
            if (n < sizeof(tmp)) tmp[n++] = '/';
    }
    for (const char *p = path; *p && n < sizeof(tmp); p++) tmp[n++] = *p;
    if (n >= sizeof(tmp)) return -1;

    size_t w = 1;                          /* out[0] == '/' */
    out[0] = '/';                          /* first component lands at out[1] */
    const char *p = tmp;
    const char *comp;
    int len;
    while ((len = next_component(&p, &comp)) >= 0) {
        if (len == 1 && comp[0] == '.') continue;
        if (len == 2 && comp[0] == '.' && comp[1] == '.') {
            while (w > 1 && out[w - 1] != '/') w--;   /* drop last comp */
            if (w > 1) w--;                            /* and its slash */
            continue;
        }
        if ((size_t)len + 1 >= out_max - w) return -1; /* + '/' + NUL */
        if (w > 1) out[w++] = '/';
        memcpy(out + w, comp, (size_t)len);
        w += len;
    }
    if (w == 0 || w >= out_max) return -1;
    out[w] = 0;
    return 0;
}
