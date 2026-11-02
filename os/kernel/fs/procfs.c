#include "novix.h"
#include "vfs.h"
#include "console.h"
#include "pmm.h"
#include "heap.h"
#include "timer.h"
#include "smp.h"
#include "mm.h"
#include "lib.h"

/*
 * procfs (v1.1.8): a read-only synthetic filesystem at /proc -- the
 * "fake becomes real" closing move for the original kernel module's
 * /proc/procman (that one faked a process list inside a Linux module;
 * this one is a real kernel publishing real numbers).
 *
 * Design: a static table of (name, generator) pairs; one struct inode
 * per file plus the directory, all owned by root with mode 0444/0555.
 * Reads generate the content on the fly into a stack buffer, then hand
 * out [pos, end) -- nothing is cached, every read sees the live counters.
 * The VFS mount table pins the directory under the root (FAT or ramfs);
 * there is no write path at all.
 */

struct proc_entry {
    const char *name;
    int (*gen)(char *buf, int cap);
};

static int gen_uptime(char *buf, int cap) {
    unsigned ms = (unsigned)timer_uptime_ms();
    return ksnprintf(buf, cap, "%u ms\n", ms);
}

static int gen_version(char *buf, int cap) {
    return ksnprintf(buf, cap, "Novix %s x86_64 -- a real OS, grown out "
                    "of a kernel module\n", NOVIX_VERSION);
}

static int gen_meminfo(char *buf, int cap) {
    return ksnprintf(buf, cap,
                     "pages_free %u\npages_total %u\n"
                     "heap_used %u\nheap_free %u\n"
                     "swap_slots_free %u\nswap_slots_total %u\n",
                     pmm_free_pages(), pmm_total_pages(),
                     (unsigned)(heap_used_bytes() >> 10),
                     (unsigned)(heap_free_bytes() >> 10),
                     swap_free_slots(), swap_total_slots());
}

static int gen_cpuinfo(char *buf, int cap) {
    int n = ksnprintf(buf, cap, "ncpu %d\nonline %d\n",
                      smp_cpu_count(), smp_cpus_online());
    for (int i = 0; i < SMP_MAX && n < cap - 32; i++) {
        if (!cpus[i].online && i >= smp_cpu_count()) continue;
        n += ksnprintf(buf + n, cap - n, "cpu%d apic %d online %d\n",
                       i, cpus[i].apic_id, cpus[i].online ? 1 : 0);
    }
    return n;
}

static const struct proc_entry proc_files[] = {
    { "uptime",  gen_uptime },
    { "version", gen_version },
    { "meminfo", gen_meminfo },
    { "cpuinfo", gen_cpuinfo },
};
#define PROC_NFILES (int)(sizeof(proc_files) / sizeof(proc_files[0]))

/* ---- inode table ---- */

static struct inode proc_inodes[PROC_NFILES + 1];   /* [0] is the dir */

static struct inode *proc_dir(void) { return &proc_inodes[0]; }

/* ---- file operations: generate, then serve [pos, end) ---- */

/* regenerate on every read -- cheap, and every reader sees live numbers */
static int proc_f_read(struct file *f, void *out, uint32_t len) {
    int idx = (int)(intptr_t)f->inode->i_priv;
    if (idx < 0 || idx >= PROC_NFILES) return -1;
    char buf[256];
    int n = proc_files[idx].gen(buf, sizeof(buf));
    if (n < 0) return -1;
    if (f->pos >= (uint32_t)n) return 0;              /* EOF */
    uint32_t left = (uint32_t)n - f->pos;
    if (len > left) len = left;
    memcpy(out, buf + f->pos, len);
    f->pos += len;
    return (int)len;
}

static const struct file_operations proc_fops = {
    .read = proc_f_read,
};

/* ---- directory operations: the static table is the tree ---- */

static struct inode *proc_i_lookup(struct inode *dir, const char *name) {
    (void)dir;
    for (int i = 0; i < PROC_NFILES; i++)
        if (strcmp(proc_files[i].name, name) == 0)
            return &proc_inodes[i + 1];
    return NULL;
}

static int proc_i_readdir(struct inode *dir, int idx, char *name_out,
                          size_t name_max) {
    (void)dir;
    if (idx < 0 || idx >= PROC_NFILES) return 1;
    strncpy(name_out, proc_files[idx].name, name_max - 1);
    name_out[name_max - 1] = 0;
    return 0;
}

static int proc_i_foreach(struct inode *dir, vfs_dir_cb cb, void *ctx) {
    (void)dir;
    for (int i = 0; i < PROC_NFILES; i++)
        if (cb(ctx, proc_files[i].name)) return 1;
    return 0;
}

static const struct inode_operations proc_iops = {
    .lookup = proc_i_lookup,
    .readdir = proc_i_readdir,
    .foreach = proc_i_foreach,
};

/* ---- init + mount ---- */

void procfs_init(void) {
    memset(proc_inodes, 0, sizeof(proc_inodes));
    struct inode *d = proc_dir();
    d->ino = 0xFFFF0001;
    d->mode = S_IFDIR | 0555;                 /* readable + searchable, no write */
    d->i_op = &proc_iops;
    d->i_parent = d;                          /* ".." stops here */
    for (int i = 0; i < PROC_NFILES; i++) {
        struct inode *f = &proc_inodes[i + 1];
        f->ino = (uint32_t)(0xFFFF0010u + i);
        f->mode = S_IFREG | 0444;             /* world-readable, never writable */
        f->f_op = &proc_fops;
        f->i_priv = (void *)(intptr_t)i;
        f->i_parent = d;
    }
    /* pin under the current root (FAT after the switch, ramfs otherwise);
     * a second call (there is none today) would fail the dup check */
    vfs_mount(vfs_get_root(), "proc", proc_dir());
}
