#include "novix.h"
#include "vfs.h"
#include "task.h"
#include "sched.h"
#include "timer.h"
#include "heap.h"
#include "spinlock.h"
#include "lib.h"
#include "smp.h"

/*
 * Pipes (v0.5.0). A pipe has no path and no inode in the tree: sys_pipe
 * mints an anonymous inode whose f_op is pipe_fops and hands out two struct
 * files sharing it -- one O_RDONLY, one O_WRONLY. The vfs flags checks in
 * vfs_read/vfs_write already route the two ends the right way, so one f_op
 * with both methods is enough (exactly how Linux's pipefs hides under a
 * "virtual" dentry).
 *
 * Blocking: on an empty pipe the reader sleeps one tick and returns
 * -EAGAIN_RETRY (-2) -- ulib's read/write wrappers spin on it, which gives
 * Unix blocking semantics with tick-granularity latency. The writer pokes
 * a sleeping peer immediately, the tick timeout is only a lost-wakeup
 * safety net. EOF (no writers left) reads 0; a write with no readers is
 * EPIPE (-1).
 *
 * v0.7.0: SMP. rpos/wpos and the waiter pointers are guarded by p->lock
 * (leaf). Waits happen by sleeping one tick OUTSIDE the lock -- a peer
 * must be able to take the lock to deliver data and poke us. A peer that
 * wakes runs on any CPU: task_wake + smp_resched_others() broadcasts the
 * hint, so latency no longer depends on which CPU the sleeper parked on.
 */

#define PIPE_BUF_SZ   4096
#define EAGAIN_RETRY  (-2)     /* internal: caller (ulib) retries */

struct pipe {
    spinlock_t lock;
    uint8_t  buf[PIPE_BUF_SZ];
    uint32_t rpos, wpos;       /* monotonic; index modulo PIPE_BUF_SZ */
    int readers;               /* live read ends (files, not processes) */
    int writers;               /* live write ends */
    struct task *rd_waiter;    /* reader blocked for data */
    struct task *wr_waiter;    /* writer blocked for space */
};

static uint32_t pipe_used(const struct pipe *p) {
    return p->wpos - p->rpos;
}
static uint32_t pipe_space(const struct pipe *p) {
    return PIPE_BUF_SZ - pipe_used(p);
}

/* take a waiter pointer and clear it; called with p->lock HELD */
static struct task *pipe_take_waiter(struct task **slot) {
    struct task *w = *slot;
    *slot = NULL;
    return w;
}

static int pipe_read(struct file *f, void *buf, uint32_t len) {
    struct pipe *p = f->inode->i_priv;
    irq_flags_t f2;
    spin_lock_irqsave(&p->lock, &f2);
    uint32_t used = pipe_used(p);
    if (used == 0) {
        if (p->writers == 0) { spin_unlock_irqrestore(&p->lock, f2); return 0; }
        p->rd_waiter = current_task();
        spin_unlock_irqrestore(&p->lock, f2);
        task_sleep_until(current_task(), timer_ticks() + 1);
        sched_kick();
        return EAGAIN_RETRY;                    /* ulib retries */
    }
    uint32_t n = len < used ? len : used;
    uint8_t *dst = buf;
    uint32_t r = p->rpos % PIPE_BUF_SZ;
    uint32_t first = PIPE_BUF_SZ - r < n ? PIPE_BUF_SZ - r : n;
    memcpy(dst, p->buf + r, first);
    memcpy(dst + first, p->buf, n - first);     /* wrapped tail */
    p->rpos += n;
    struct task *wr = pipe_take_waiter(&p->wr_waiter);  /* space opened up */
    spin_unlock_irqrestore(&p->lock, f2);
    if (wr && task_wake(wr)) smp_resched_others();
    return (int)n;
}

static int pipe_write(struct file *f, const void *buf, uint32_t len) {
    struct pipe *p = f->inode->i_priv;
    irq_flags_t f2;
    spin_lock_irqsave(&p->lock, &f2);
    uint32_t space = pipe_space(p);
    if (space == 0) {
        if (p->readers == 0) { spin_unlock_irqrestore(&p->lock, f2); return -1; }
        p->wr_waiter = current_task();
        spin_unlock_irqrestore(&p->lock, f2);
        task_sleep_until(current_task(), timer_ticks() + 1);
        sched_kick();
        return EAGAIN_RETRY;
    }
    uint32_t n = len < space ? len : space;
    const uint8_t *src = buf;
    uint32_t w = p->wpos % PIPE_BUF_SZ;
    uint32_t first = PIPE_BUF_SZ - w < n ? PIPE_BUF_SZ - w : n;
    memcpy(p->buf + w, src, first);
    memcpy(p->buf, src + first, n - first);     /* wrapped tail */
    p->wpos += n;
    struct task *rd = pipe_take_waiter(&p->rd_waiter);  /* data arrived */
    bool no_readers = p->readers == 0;
    spin_unlock_irqrestore(&p->lock, f2);
    if (rd && task_wake(rd)) smp_resched_others();
    if (no_readers) return -1;                  /* readers vanished mid-copy */
    return (int)n;
}

static void pipe_close(struct file *f) {
    struct pipe *p = f->inode->i_priv;
    irq_flags_t f2;
    spin_lock_irqsave(&p->lock, &f2);
    if ((f->flags & O_ACCMODE) == O_RDONLY) p->readers--;
    else                                     p->writers--;

    /* the surviving side must wake up to observe EOF/EPIPE */
    struct task *rd = pipe_take_waiter(&p->rd_waiter);
    struct task *wr = pipe_take_waiter(&p->wr_waiter);
    bool dead = p->readers == 0 && p->writers == 0;
    spin_unlock_irqrestore(&p->lock, f2);

    bool woke = false;
    if (rd && task_wake(rd)) woke = true;
    if (wr && task_wake(wr)) woke = true;
    if (woke) smp_resched_others();

    if (dead) {
        kfree(p);
        kfree(f->inode);                        /* anonymous inode */
    }
}

static const struct file_operations pipe_fops = {
    .read  = pipe_read,
    .write = pipe_write,
    .close = pipe_close,
};

/* sys_pipe backend: install the two ends in the current task's fd table.
 * returns 0 with fds in rfd/wfd, or -1. */
int pipe_create(int *rfd, int *wfd) {
    struct pipe *p = kmalloc(sizeof(*p));
    struct inode *ino = kmalloc(sizeof(*ino));
    struct file *rf = kmalloc(sizeof(*rf));
    struct file *wf = kmalloc(sizeof(*wf));
    if (!p || !ino || !rf || !wf) goto fail;

    memset(p, 0, sizeof(*p));
    p->readers = 1;
    p->writers = 1;

    memset(ino, 0, sizeof(*ino));
    ino->mode = 0;                        /* not S_IFREG: lseek refuses it */
    ino->i_priv = p;
    ino->f_op = &pipe_fops;

    memset(rf, 0, sizeof(*rf));
    memset(wf, 0, sizeof(*wf));
    rf->inode = ino; rf->flags = O_RDONLY; rf->f_count = 1;
    wf->inode = ino; wf->flags = O_WRONLY; wf->f_count = 1;

    /* the read end must be installed before the write fd is allocated:
     * fd_alloc() scans for empty slots, so two back-to-back allocations
     * would return the same number and the write end would clobber the
     * read end's slot */
    int r = fd_alloc();
    if (r < 0) goto fail;
    fd_install(r, rf);
    int w = fd_alloc();
    if (w < 0) {
        fd_close(r);                /* frees rf; readers -> 0 */
        vfs_close(wf);              /* writers -> 0: pipe_close frees pipe+inode */
        return -1;
    }
    fd_install(w, wf);
    *rfd = r;
    *wfd = w;
    return 0;

fail:
    kfree(wf);
    kfree(rf);
    kfree(ino);
    kfree(p);
    return -1;
}
