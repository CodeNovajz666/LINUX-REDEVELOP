#include "novix.h"
#include "vfs.h"
#include "lib.h"

/*
 * Path resolution. No cwd exists yet, so relative paths resolve from the
 * root ("file.c" == "/file.c"); that arrives with the login work. "." is
 * skipped, ".." climbs to the parent but never above the root, and any
 * component that fails lookup aborts the walk.
 */

static struct inode *root_inode_holder;     /* see vfs.c setter/getter */
void vfs_set_root(struct inode *root) { root_inode_holder = root; }
struct inode *vfs_get_root(void) { return root_inode_holder; }

static struct inode *walk_component(struct inode *dir, const char *comp, size_t len) {
    if (!dir || !(dir->mode & S_IFDIR) || !dir->i_op || !dir->i_op->lookup)
        return NULL;
    char name[32];
    if (len == 0 || len >= sizeof(name)) return NULL;
    memcpy(name, comp, len);
    name[len] = 0;
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

struct inode *namei(const char *path) {
    if (!path || !path[0]) return NULL;
    struct inode *cur = vfs_get_root();
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
    struct inode *cur = vfs_get_root();
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
