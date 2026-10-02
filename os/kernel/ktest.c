#include "novix.h"
#include "ktest.h"
#include "console.h"
#include "pmm.h"
#include "heap.h"
#include "paging.h"
#include "task.h"
#include "sched.h"
#include "timer.h"
#include "elf.h"
#include "vfs.h"
#include "mm.h"
#include "percpu.h"
#include "smp.h"
#include "spinlock.h"
#include "lib.h"

/*
 * In-kernel test suite. Unlike the old module's test programs (which printed
 * "completed" without a single assertion), every check here either passes or
 * names its line.
 */

static int ran, failed, cur_failed;

#define KT_ASSERT(cond)                                                        \
    do {                                                                       \
        if (!(cond)) {                                                         \
            kprintf("  ASSERT FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            cur_failed = 1;                                                    \
            return;                                                            \
        }                                                                      \
    } while (0)

struct ktest_case {
    const char *name;
    void (*fn)(void);
};

int ktest_failed_count(void) { return failed; }
int ktest_total_count(void) { return ran; }

static void run_cases(const char *phase, const struct ktest_case *cases, int n) {
    kprintf("== %s tests ==\n", phase);
    for (int i = 0; i < n; i++) {
        kprintf("[TEST] %-24s ", cases[i].name);
        cur_failed = 0;
        cases[i].fn();
        ran++;
        if (cur_failed) { failed++; kprintf("FAILED\n"); }
        else kprintf("ok\n");
    }
}

/* reap a child and verify its exit status */
static void reap_expect(struct task *t, int expect) {
    int st = task_waitpid(t->pid, 0);
    if (st != expect) {
        kprintf("  exit status: got %d want %d (%s:%d)\n", st, expect, __FILE__, __LINE__);
        cur_failed = 1;
    }
}

/* ---------------- early tests (before multitasking) ---------------- */

static void test_pmm(void) {
    uint32_t base_free = pmm_free_pages();
    uint64_t pages[8];
    for (int i = 0; i < 8; i++) {
        pages[i] = pmm_alloc();
        KT_ASSERT(pages[i] != 0);
        KT_ASSERT((pages[i] & (PAGE_SIZE - 1)) == 0);
        for (int j = 0; j < i; j++)
            KT_ASSERT(pages[i] != pages[j]);
    }
    KT_ASSERT(pmm_free_pages() == base_free - 8);
    for (int i = 0; i < 8; i += 2)
        pmm_free(pages[i]);
    KT_ASSERT(pmm_free_pages() == base_free - 4);
    uint64_t again = pmm_alloc();
    KT_ASSERT(again != 0);
    KT_ASSERT(pmm_free_pages() == base_free - 5);
    pmm_free(again);
    for (int i = 1; i < 8; i += 2)
        pmm_free(pages[i]);
    KT_ASSERT(pmm_free_pages() == base_free);
}

static void test_heap(void) {
    void *p1 = kmalloc(1000);
    KT_ASSERT(p1 != NULL);
    memset(p1, 0xAB, 1000);
    KT_ASSERT(((unsigned char *)p1)[0] == 0xAB && ((unsigned char *)p1)[999] == 0xAB);
    kfree(p1);
    void *p2 = kmalloc(1000);
    KT_ASSERT(p2 == p1);                       /* first-fit reuses freed block */
    kfree(p2);
    KT_ASSERT(kmalloc(0) == NULL);
    KT_ASSERT(kmalloc(HEAP_END - HEAP_START + 4096) == NULL);
    void *ps[16];
    for (int i = 0; i < 16; i++) {
        ps[i] = kmalloc(32 + i * 128);
        KT_ASSERT(ps[i] != NULL);
    }
    for (int i = 0; i < 16; i++) kfree(ps[i]);
    size_t f0 = heap_free_bytes();
    void *big = kmalloc(1 << 20);
    KT_ASSERT(big != NULL);
    kfree(big);
    KT_ASSERT(heap_free_bytes() == f0);        /* fully coalesced back */
}

static void test_paging(void) {
    uint32_t free0 = pmm_free_pages();
    uint64_t pd = paging_create_user_dir();
    KT_ASSERT(pd != 0);
    /* kernel low map shared into the user dir: walk PML4[0] -> PDPT[0] ->
     * PD[0] and find the supervisor 2 MiB entries (the i386 test checked the
     * shared 4 MiB PD entries; same assertion one level deeper) */
    uint64_t *pml4 = (uint64_t *)(uintptr_t)pd;
    KT_ASSERT((pml4[0] & 0x001) != 0);
    uint64_t *pdpt = (uint64_t *)(uintptr_t)(pml4[0] & ~0xFFFULL);
    KT_ASSERT((pdpt[0] & 0x001) != 0);
    uint64_t *kpd = (uint64_t *)(uintptr_t)(pdpt[0] & ~0xFFFULL);
    KT_ASSERT((kpd[0] & 0x080) != 0);          /* 2 MiB kernel entry shared */

    uint64_t pa = pmm_alloc_zero();
    KT_ASSERT(pa != 0);
    KT_ASSERT(paging_map_user(pd, USER_CODE_BASE, pa, 1) == 0);
    KT_ASSERT(paging_map_user(pd, 0x00001000, pa, 1) == -1);   /* kernel range refused */

    uint64_t saved = paging_cur_cr3();
    paging_load_cr3(pd);
    volatile uint32_t *u = (volatile uint32_t *)(uintptr_t)USER_CODE_BASE;
    *u = 0xDEADBEEFu;
    uint32_t rd = *u;
    paging_load_cr3(saved);
    KT_ASSERT(rd == 0xDEADBEEFu);

    paging_destroy_user_dir(pd);
    KT_ASSERT(pmm_free_pages() == free0);      /* everything reclaimed */
}

static const struct ktest_case early_cases[] = {
    { "pmm", test_pmm },
    { "heap", test_heap },
    { "paging", test_paging },
};

int ktest_run_early(void) {
    run_cases("early", early_cases, sizeof(early_cases) / sizeof(early_cases[0]));
    return failed;
}

/* ---------------- multitasking tests ---------------- */

static char run_order[8];
static int run_idx;

static void order_thread(void *arg) {
    run_order[run_idx++] = (char)(intptr_t)arg;
    task_exit(0);
}

static void test_priority_order(void) {
    sched_set_policy(SCHED_PRIORITY);
    run_idx = 0;
    struct task *h = task_create_kernel("tp-h", order_thread, (void *)(intptr_t)'H', 9);
    struct task *m = task_create_kernel("tp-m", order_thread, (void *)(intptr_t)'M', 5);
    struct task *l = task_create_kernel("tp-l", order_thread, (void *)(intptr_t)'L', 1);
    KT_ASSERT(h && m && l);
    reap_expect(h, 0); reap_expect(m, 0); reap_expect(l, 0);
    KT_ASSERT(run_idx == 3);
    KT_ASSERT(run_order[0] == 'H' && run_order[1] == 'M' && run_order[2] == 'L');
}

static volatile int rr_count[2];
static void rr_thread(void *arg) {
    int id = (int)(intptr_t)arg;
    for (int i = 0; i < 5; i++) {
        rr_count[id]++;
        kyield();
    }
    task_exit(0);
}

static void test_round_robin(void) {
    sched_set_policy(SCHED_ROUND_ROBIN);
    rr_count[0] = rr_count[1] = 0;
    struct task *a = task_create_kernel("rr-a", rr_thread, (void *)(intptr_t)0, 5);
    struct task *b = task_create_kernel("rr-b", rr_thread, (void *)(intptr_t)1, 5);
    KT_ASSERT(a && b);
    reap_expect(a, 0); reap_expect(b, 0);
    KT_ASSERT(rr_count[0] == 5 && rr_count[1] == 5);   /* both made progress */
}

static void test_fcfs_order(void) {
    sched_set_policy(SCHED_FCFS);
    run_idx = 0;
    /* A created first with LOW priority, B second with HIGH priority:
     * FCFS must run A first -- creation order beats priority. */
    struct task *a = task_create_kernel("fc-a", order_thread, (void *)(intptr_t)'A', 2);
    struct task *b = task_create_kernel("fc-b", order_thread, (void *)(intptr_t)'B', 9);
    KT_ASSERT(a && b);
    reap_expect(a, 0); reap_expect(b, 0);
    KT_ASSERT(run_idx == 2);
    KT_ASSERT(run_order[0] == 'A' && run_order[1] == 'B');
}

static void test_edf_order(void) {
    run_idx = 0;
    /* created under PRIORITY so the test task is not preempted mid-setup */
    struct task *t1 = task_create_kernel("edf-1", order_thread, (void *)(intptr_t)'1', 5);
    struct task *t2 = task_create_kernel("edf-2", order_thread, (void *)(intptr_t)'2', 5);
    KT_ASSERT(t1 && t2);
    t1->deadline = (uint32_t)timer_ticks() + 30;   /* later deadline, created FIRST */
    t2->deadline = (uint32_t)timer_ticks() + 10;   /* earlier deadline, created SECOND */
    sched_set_policy(SCHED_EDF);                   /* earliest deadline must win */
    reap_expect(t2, 0); reap_expect(t1, 0);
    KT_ASSERT(run_idx == 2);
    KT_ASSERT(run_order[0] == '2' && run_order[1] == '1');
}

static volatile uint64_t woke_at[2];
static void sleeper_thread(void *arg) {
    int id = (int)(intptr_t)arg;
    timer_sleep_ms(id == 0 ? 50 : 100);
    woke_at[id] = timer_uptime_ms();
    task_exit(id == 0 ? 11 : 22);
}

static void test_sleep_wake(void) {
    sched_set_policy(SCHED_PRIORITY);
    woke_at[0] = woke_at[1] = 0;
    uint64_t t0 = timer_uptime_ms();
    struct task *s1 = task_create_kernel("sl-50", sleeper_thread, (void *)(intptr_t)0, 5);
    struct task *s2 = task_create_kernel("sl-100", sleeper_thread, (void *)(intptr_t)1, 5);
    KT_ASSERT(s1 && s2);
    reap_expect(s1, 11); reap_expect(s2, 22);
    KT_ASSERT(woke_at[0] <= woke_at[1]);           /* ordering */
    KT_ASSERT(woke_at[1] - t0 >= 100);             /* not woken early */
}

static volatile int low_ran;
static void hog_thread(void *arg) {
    (void)arg;
    for (volatile uint64_t i = 0; i < 100000000ull && !low_ran; i++)
        ;
    task_exit(0);
}
static void low_thread(void *arg) {
    (void)arg;
    low_ran = 1;
    task_exit(0);
}

static void test_aging_antistarvation(void) {
    sched_set_policy(SCHED_PRIORITY);
    low_ran = 0;
    struct task *hog = task_create_kernel("aging-hog", hog_thread, NULL, 9);
    struct task *low = task_create_kernel("aging-low", low_thread, NULL, 1);
    KT_ASSERT(hog && low);
    reap_expect(hog, 0); reap_expect(low, 0);
    KT_ASSERT(low_ran == 1);                      /* aging let it run */
}

static void test_syscall_userproc(void) {
    sched_set_policy(SCHED_PRIORITY);
    struct task *t = task_spawn_path("/bin/hello");
    if (!t) {
        kprintf("  (/bin/hello not found in ramfs -- build error)\n");
        cur_failed = 1;
        return;
    }
    reap_expect(t, 42);

    struct task *sp = task_spawn_path("/bin/spawner");
    KT_ASSERT(sp != NULL);
    reap_expect(sp, 7);
}

static void test_fault_div(void) {
    sched_set_policy(SCHED_PRIORITY);
    struct task *t = task_spawn_path("/bin/fault_div");
    if (!t) {
        kprintf("  (/bin/fault_div not found in ramfs -- build error)\n");
        cur_failed = 1;
        return;
    }
    /* divide-by-zero in ring3 must kill the process (128+SIGFPE), not panic */
    reap_expect(t, 128 + SIGFPE);
    KT_ASSERT(t->state == TASK_UNUSED);           /* fully reaped */
}

static void test_fault_segv(void) {
    sched_set_policy(SCHED_PRIORITY);
    struct task *t = task_spawn_path("/bin/fault_segv");
    if (!t) {
        kprintf("  (/bin/fault_segv not found in ramfs -- build error)\n");
        cur_failed = 1;
        return;
    }
    /* near-null dereference must kill the process (128+SIGSEGV), not panic */
    reap_expect(t, 128 + SIGSEGV);
    KT_ASSERT(t->kill_signo == SIGSEGV);          /* cause recorded */
}

static void test_kill_other_task(void) {
    sched_set_policy(SCHED_PRIORITY);
    struct task *s = task_create_kernel("killme", sleeper_thread, (void *)(intptr_t)0, 5);
    KT_ASSERT(s != NULL);
    task_kill_task(s, SIGTERM);                   /* kill while SLEEPING */
    reap_expect(s, 128 + SIGTERM);
    KT_ASSERT(s->kill_signo == SIGTERM);
}

/* ---------------- fork/exec/COW tests (v0.4.0) ---------------- */

/* test 19: COW isolation -- the child's write must not leak to the parent */
static void test_cow_isolation(void) {
    sched_set_policy(SCHED_PRIORITY);
    struct task *t = task_spawn_path("/bin/forker");
    if (!t) {
        kprintf("  (/bin/forker not found in ramfs -- build error)\n");
        cur_failed = 1;
        return;
    }
    reap_expect(t, 0);                            /* forker self-checks */
    KT_ASSERT(t->state == TASK_UNUSED);
}

/* test 20: COW page accounting -- forkacct measures free pages around
 * quiet and writing children and self-checks; status 0 = all held */
static void test_cow_accounting(void) {
    sched_set_policy(SCHED_PRIORITY);
    struct task *t = task_spawn_path("/bin/forkacct");
    if (!t) {
        kprintf("  (/bin/forkacct not found in ramfs -- build error)\n");
        cur_failed = 1;
        return;
    }
    reap_expect(t, 0);
    KT_ASSERT(t->state == TASK_UNUSED);
}

/* test 21: exec replaces the image -- execer becomes /bin/hello and the
 * process exits with hello's status 42, not execer's 99 */
static void test_exec_replace(void) {
    sched_set_policy(SCHED_PRIORITY);
    struct task *t = task_spawn_path("/bin/execer");
    if (!t) {
        kprintf("  (/bin/execer not found in ramfs -- build error)\n");
        cur_failed = 1;
        return;
    }
    reap_expect(t, 42);
    KT_ASSERT(t->state == TASK_UNUSED);
}

/* test 22: pid reuse -- a reaped slot hands its pid back; an unreaped
 * zombie holds its pid hostage (pid == slot since v0.4.0) */
static void test_pid_reuse(void) {
    sched_set_policy(SCHED_PRIORITY);

    /* reaped: same pid comes back */
    struct task *t1 = task_spawn_path("/bin/hello");
    KT_ASSERT(t1 != NULL);
    int p1 = t1->pid;
    reap_expect(t1, 42);
    struct task *t2 = task_spawn_path("/bin/hello");
    KT_ASSERT(t2 != NULL);
    if (t2->pid != p1) {
        kprintf("  pid not reused after reap: %d then %d\n", p1, t2->pid);
        cur_failed = 1;
        reap_expect(t2, 42);
        return;
    }
    reap_expect(t2, 42);

    /* zombie: pid must NOT be reused while the parent has not waited */
    struct task *z = task_spawn_path("/bin/hello");
    KT_ASSERT(z != NULL);
    int zp = z->pid;
    uint64_t until = timer_ticks() + 800;         /* 8 s ceiling */
    while (task_find(zp) && task_find(zp)->state != TASK_ZOMBIE
           && timer_ticks() < until) {
        /* we run at PRIO_MAX and z at 5, so a bare kyield() re-picks us
         * instantly and starves z -- sleep 20 ms instead: while we are
         * SLEEPING, z is the only runnable task and must make progress */
        task_sleep_until(current_task(), timer_ticks() + 2);
        kyield();
    }
    KT_ASSERT(task_find(zp) && task_find(zp)->state == TASK_ZOMBIE);

    struct task *w = task_spawn_path("/bin/hello");
    KT_ASSERT(w != NULL);
    if (w->pid == zp) {
        kprintf("  pid %d reused while zombie still holds it\n", zp);
        cur_failed = 1;
    }
    reap_expect(z, 42);
    reap_expect(w, 42);
}

/* test 23: user heap -- malloc round-trip, free coalescing (malloctest
 * verifies the coalescing via the kernel free-page count: a 1 MiB malloc
 * after freeing 256 chunks must not trigger a new sbrk) */
static void test_malloc_heap(void) {
    sched_set_policy(SCHED_PRIORITY);
    struct task *t = task_spawn_path("/bin/malloctest");
    if (!t) {
        kprintf("  (/bin/malloctest not found in ramfs -- build error)\n");
        cur_failed = 1;
        return;
    }
    reap_expect(t, 55);
}

/* test 24: kill a SLEEPING process -- default-action termination of a task
 * parked in usleep must reap cleanly with 128+SIGKILL and leak nothing
 * (gate D re-checks the page account for the whole suite) */
static void test_kill_sleeping(void) {
    sched_set_policy(SCHED_PRIORITY);
    struct task *t = task_spawn_path("/bin/hello");
    KT_ASSERT(t != NULL);
    /* let hello run into its first usleep(200ms) */
    task_sleep_until(current_task(), timer_ticks() + 10);
    kyield();
    kprintf("  (hello state before kill: %s)\n", task_state_name(t->state));
    task_kill_task(t, SIGKILL);
    reap_expect(t, 128 + SIGKILL);
}

/* test 25: a caught SIGTERM runs its handler and the program survives to a
 * normal exit -- the frame-rewrite/sigreturn machinery end to end */
static void test_sig_catch_resume(void) {
    sched_set_policy(SCHED_PRIORITY);
    struct task *t = task_spawn_path("/bin/sigtest");
    if (!t) {
        kprintf("  (/bin/sigtest not found in ramfs -- build error)\n");
        cur_failed = 1;
        return;
    }
    reap_expect(t, 77);
}

/* test 26: pipes -- 1 KiB both ways between fork relatives, EOF after the
 * writer closes, exit status through waitpid */
static void test_pipe_eof(void) {
    sched_set_policy(SCHED_PRIORITY);
    struct task *t = task_spawn_path("/bin/pipetest");
    if (!t) {
        kprintf("  (/bin/pipetest not found in ramfs -- build error)\n");
        cur_failed = 1;
        return;
    }
    reap_expect(t, 66);
}

/* test 27: signal masking -- a blocked SIGTERM stays pending across yields
 * (handler does not run), SIGKILL is refused by the mask, and unblocking
 * delivers the pending signal out of the sigprocmask syscall itself */
static void test_sig_mask(void) {
    sched_set_policy(SCHED_PRIORITY);
    struct task *t = task_spawn_path("/bin/masktest");
    if (!t) {
        kprintf("  (/bin/masktest not found in ramfs -- build error)\n");
        cur_failed = 1;
        return;
    }
    reap_expect(t, 44);
}

static const struct ktest_case mt_cases[] = {
    { "priority_order", test_priority_order },
    { "round_robin", test_round_robin },
    { "fcfs_order", test_fcfs_order },
    { "edf_order", test_edf_order },
    { "sleep_wake", test_sleep_wake },
    { "aging_anti_starvation", test_aging_antistarvation },
    { "syscall_userproc", test_syscall_userproc },
    { "fault_div_isolation", test_fault_div },
    { "fault_segv_isolation", test_fault_segv },
    { "kill_other_task", test_kill_other_task },
    { "cow_isolation", test_cow_isolation },
    { "cow_accounting", test_cow_accounting },
    { "exec_replace", test_exec_replace },
    { "pid_reuse", test_pid_reuse },
    { "malloc_heap", test_malloc_heap },
    { "kill_sleeping", test_kill_sleeping },
    { "sig_catch_resume", test_sig_catch_resume },
    { "pipe_eof", test_pipe_eof },
    { "sig_mask", test_sig_mask },
};

int ktest_run_mt(void) {
    run_cases("multitasking", mt_cases, sizeof(mt_cases) / sizeof(mt_cases[0]));
    sched_set_policy(SCHED_PRIORITY);
    return failed;
}

/* ---------------- filesystem tests (ramfs/VFS/fd, init task context) ---- */

static void ensure_dir(const char *path) {
    struct stat st;
    if (vfs_stat(path, &st) != 0) vfs_mkdir(path);
}

static void test_vfs_create_rw(void) {
    ensure_dir("/tmp");
    const char *p = "/tmp/ktest-rw.txt";

    struct file *f = vfs_open(p, O_WRONLY | O_CREAT | O_TRUNC);
    KT_ASSERT(f != NULL);
    KT_ASSERT(vfs_write(f, "hello ramfs", 11) == 11);
    KT_ASSERT(vfs_close(f) == 0);

    /* read back through a fresh open */
    struct file *g = vfs_open(p, O_RDONLY);
    KT_ASSERT(g != NULL);
    char buf[32];
    int n = vfs_read(g, buf, sizeof(buf));
    KT_ASSERT(n == 11);
    KT_ASSERT(memcmp(buf, "hello ramfs", 11) == 0);
    KT_ASSERT(vfs_read(g, buf, sizeof(buf)) == 0);   /* EOF */
    KT_ASSERT(vfs_close(g) == 0);

    /* writing through an O_RDONLY file must fail */
    struct file *ro = vfs_open(p, O_RDONLY);
    KT_ASSERT(ro != NULL);
    KT_ASSERT(vfs_write(ro, "x", 1) == -1);
    vfs_close(ro);

    struct stat st;
    KT_ASSERT(vfs_stat(p, &st) == 0);
    KT_ASSERT((st.st_mode & S_IFMT) == S_IFREG);
    KT_ASSERT(st.st_size == 11);

    /* unlink removes it everywhere */
    KT_ASSERT(vfs_unlink(p) == 0);
    KT_ASSERT(vfs_stat(p, &st) == -1);
    KT_ASSERT(vfs_open(p, O_RDONLY) == NULL);
}

static void test_vfs_lseek(void) {
    const char *p = "/tmp/ktest-seek";
    struct file *f = vfs_open(p, O_RDWR | O_CREAT | O_TRUNC);
    KT_ASSERT(f != NULL);
    KT_ASSERT(vfs_write(f, "0123456789", 10) == 10);

    KT_ASSERT(vfs_lseek(f, 0, SEEK_SET) == 0);
    char buf[16];
    KT_ASSERT(vfs_read(f, buf, 4) == 4);
    KT_ASSERT(memcmp(buf, "0123", 4) == 0);

    KT_ASSERT(vfs_lseek(f, 2, SEEK_CUR) == 6);       /* 4 + 2 */
    KT_ASSERT(vfs_read(f, buf, 8) == 4);             /* only "6789" left */
    KT_ASSERT(memcmp(buf, "6789", 4) == 0);

    KT_ASSERT(vfs_lseek(f, -2, SEEK_END) == 8);
    KT_ASSERT(vfs_read(f, buf, 8) == 2);
    KT_ASSERT(memcmp(buf, "89", 2) == 0);

    KT_ASSERT(vfs_lseek(f, 99, SEEK_SET) == -1);     /* past EOF refused */
    KT_ASSERT(vfs_lseek(f, 0, 99) == -1);            /* bad whence */
    vfs_close(f);
    vfs_unlink(p);
}

static void test_vfs_mkdir_readdir(void) {
    KT_ASSERT(vfs_mkdir("/tmp/kdir") == 0);
    struct stat st;
    KT_ASSERT(vfs_stat("/tmp/kdir", &st) == 0);
    KT_ASSERT((st.st_mode & S_IFMT) == S_IFDIR);

    /* directories are not openable */
    KT_ASSERT(vfs_open("/tmp/kdir", O_RDONLY) == NULL);

    /* three entries inside */
    for (int i = 0; i < 3; i++) {
        char p[] = "/tmp/kdir/f0";
        p[11] = (char)('0' + i);
        struct file *f = vfs_open(p, O_WRONLY | O_CREAT | O_TRUNC);
        KT_ASSERT(f != NULL);
        vfs_close(f);
    }
    bool saw[3] = { false, false, false };
    char name[32];
    int n = 0;
    for (int i = 0; vfs_readdir("/tmp/kdir", i, name, sizeof(name)) == 0; i++) {
        n++;
        for (int j = 0; j < 3; j++)
            if (name[0] == 'f' && name[1] == (char)('0' + j) && name[2] == 0)
                saw[j] = true;
    }
    KT_ASSERT(n == 3);
    KT_ASSERT(saw[0] && saw[1] && saw[2]);

    /* unlink one entry: it disappears from the listing */
    KT_ASSERT(vfs_unlink("/tmp/kdir/f1") == 0);
    n = 0;
    bool saw_f1 = false;
    for (int i = 0; vfs_readdir("/tmp/kdir", i, name, sizeof(name)) == 0; i++) {
        n++;
        if (strcmp(name, "f1") == 0) saw_f1 = true;
    }
    KT_ASSERT(n == 2 && !saw_f1);

    /* directories cannot be unlinked yet (no rmdir) */
    KT_ASSERT(vfs_unlink("/tmp/kdir") == -1);

    /* cleanup: files first, the empty dir stays (ramfs keeps it) */
    vfs_unlink("/tmp/kdir/f0");
    vfs_unlink("/tmp/kdir/f2");
}

static void test_fd_exhaust(void) {
    const char *p = "/tmp/ktest-fd";
    struct file *f = vfs_open(p, O_WRONLY | O_CREAT | O_TRUNC);
    KT_ASSERT(f != NULL);
    KT_ASSERT(vfs_write(f, "x", 1) == 1);
    vfs_close(f);

    /* fd 0/1/2 are the console: fill everything above them */
    int fds[TASK_MAX_FD];
    int n = 0;
    for (int i = 0; i < TASK_MAX_FD; i++) {
        struct file *g = vfs_open(p, O_RDONLY);
        KT_ASSERT(g != NULL);
        int fd = fd_alloc();
        if (fd < 0) { vfs_close(g); break; }     /* table full */
        KT_ASSERT(fd == 3 + n);                  /* lowest free first */
        KT_ASSERT(fd_install(fd, g) == 0);
        fds[n++] = fd;
    }
    KT_ASSERT(n == TASK_MAX_FD - 3);
    KT_ASSERT(fd_alloc() == -1);                 /* exhausted */

    /* an installed fd reads through the table */
    KT_ASSERT(fd_get(fds[0]) != NULL);
    char c;
    KT_ASSERT(vfs_read(fd_get(fds[0]), &c, 1) == 1 && c == 'x');

    /* closing one frees exactly that slot */
    fd_close(fds[0]);
    KT_ASSERT(fd_alloc() == fds[0]);
    struct file *g2 = vfs_open(p, O_RDONLY);
    KT_ASSERT(g2 != NULL);
    KT_ASSERT(fd_install(fds[0], g2) == 0);

    for (int i = 0; i < n; i++) fd_close(fds[i]);
    KT_ASSERT(fd_alloc() == 3);                  /* everything freed */
    vfs_unlink(p);
}

static void test_devfs(void) {
    /* /dev/null swallows writes, has no read, refuses lseek */
    struct file *n = vfs_open("/dev/null", O_WRONLY);
    KT_ASSERT(n != NULL);
    KT_ASSERT(vfs_write(n, "garbage", 7) == 7);
    KT_ASSERT(vfs_lseek(n, 0, SEEK_SET) == -1);
    KT_ASSERT(vfs_open("/dev/null", O_RDONLY) == NULL);
    vfs_close(n);

    /* /dev/console: a char device, openable read-write */
    struct stat st;
    KT_ASSERT(vfs_stat("/dev/console", &st) == 0);
    KT_ASSERT((st.st_mode & S_IFMT) == S_IFCHR);
    struct file *c = vfs_open("/dev/console", O_RDWR);
    KT_ASSERT(c != NULL);
    KT_ASSERT(vfs_write(c, "", 0) == 0);         /* zero-length, no output */
    vfs_close(c);

    /* initrd boot files are real ramfs inodes */
    KT_ASSERT(vfs_stat("/bin/hello", &st) == 0);
    KT_ASSERT((st.st_mode & S_IFMT) == S_IFREG);
    KT_ASSERT(st.st_size > 0);

    /* / contains at least bin and dev */
    bool saw_bin = false, saw_dev = false;
    char name[32];
    for (int i = 0; vfs_readdir("/", i, name, sizeof(name)) == 0; i++) {
        if (strcmp(name, "bin") == 0) saw_bin = true;
        if (strcmp(name, "dev") == 0) saw_dev = true;
    }
    KT_ASSERT(saw_bin && saw_dev);

    /* missing paths fail cleanly */
    KT_ASSERT(vfs_open("/bin/definitely-missing", O_RDONLY) == NULL);
    KT_ASSERT(vfs_stat("/no/such/file", &st) == -1);
}

static const struct ktest_case fs_cases[] = {
    { "vfs_create_rw", test_vfs_create_rw },
    { "vfs_lseek", test_vfs_lseek },
    { "vfs_mkdir_readdir", test_vfs_mkdir_readdir },
    { "fd_exhaust", test_fd_exhaust },
    { "devfs_initrd", test_devfs },
};

int ktest_run_fs(void) {
    run_cases("filesystem", fs_cases, sizeof(fs_cases) / sizeof(fs_cases[0]));
    return failed;
}

/* ---------------- vm tests (vma / demand paging / swap, init task) -------
 *
 * The suite relies on gate ordering: C/D/E ran first, so at this point
 * no other user task exists -- whatever kswapd evicts under the forced
 * watermark belongs to the program we spawned. */

/* test 28: registration-only ELF loading. hello runs to exit 42 with all
 * of its text/data arriving through the #PF file path and its stack
 * through the anon path -- if the loader had mapped anything eagerly,
 * the counters would not move. */
static void test_demand_file_paging(void) {
    sched_set_policy(SCHED_PRIORITY);
    uint32_t ff0 = g_mmstat.fault_file;
    struct task *t = task_spawn_path("/bin/hello");
    KT_ASSERT(t != NULL);
    reap_expect(t, 42);
    if (g_mmstat.fault_file == ff0) {
        kprintf("  no file pages faulted in (loader still eager?)\n");
        cur_failed = 1;
    }
}

/* test 29: anonymous mmap round trip at the default watermark -- 256
 * pages fault in zeroed, hold their pattern, and no page ever leaves
 * RAM (swap is idle while free memory is plentiful). */
static void test_mmap_anon(void) {
    sched_set_policy(SCHED_PRIORITY);
    uint32_t fa0 = g_mmstat.fault_anon, so0 = g_mmstat.swap_out;
    struct task *t = task_spawn_path("/bin/memhog");
    if (!t) {
        kprintf("  (/bin/memhog not found in ramfs -- build error)\n");
        cur_failed = 1;
        return;
    }
    reap_expect(t, 88);
    if (g_mmstat.fault_anon - fa0 < 256) {
        kprintf("  only %u anon faults for 256 mmap pages\n",
                g_mmstat.fault_anon - fa0);
        cur_failed = 1;
    }
    if (g_mmstat.swap_out != so0) {
        kprintf("  %u pages swapped out with no memory pressure\n",
                g_mmstat.swap_out - so0);
        cur_failed = 1;
    }
}

/* test 30: the swap round trip. The watermark is forced above the free
 * count so kswapd scans every 20 ms while memhog sleeps; the verify pass
 * faults the evicted pages back in and checks the bytes. The watermark
 * is restored before any assertion can bail out. */
static void test_swap_roundtrip(void) {
    sched_set_policy(SCHED_PRIORITY);
    uint32_t so0 = g_mmstat.swap_out, in0 = g_mmstat.fault_swapin;
    struct task *t = task_spawn_path("/bin/memhog");
    KT_ASSERT(t != NULL);
    swap_set_watermark(pmm_free_pages() + 64);   /* always scan */
    reap_expect(t, 88);
    swap_set_watermark(0);                       /* default, before asserts */
    if (g_mmstat.swap_out == so0) {
        kprintf("  kswapd evicted nothing under forced pressure\n");
        cur_failed = 1;
    }
    if (g_mmstat.fault_swapin == in0) {
        kprintf("  no swap-in faults (verify pass saw no evicted page)\n");
        cur_failed = 1;
    }
}

static const struct ktest_case vm_cases[] = {
    { "demand_file_paging", test_demand_file_paging },
    { "mmap_anon", test_mmap_anon },
    { "swap_roundtrip", test_swap_roundtrip },
};

int ktest_run_vm(void) {
    run_cases("vm", vm_cases, sizeof(vm_cases) / sizeof(vm_cases[0]));
    swap_set_watermark(0);                       /* belt and braces */
    return failed;
}

/* ---------------- smp tests (v0.7.0) ---------------------------------------
 *
 * Runs after smp_release(): the APs are online and pulling from the same
 * global runqueue while the suite counts on them, so -- like gate F's
 * sibling comment -- no free-page equality snapshot is possible here
 * (kswapd and the other CPUs legitimately move pages mid-suite). The
 * suite's own invariants are arithmetic, not accounting.
 *
 * On a 1-CPU machine (build.py -smp 1) the suite degrades to the
 * single-core regression: exact counting still applies, the cross-CPU
 * assertions relax to "exactly one CPU was ever seen". */

/* test 31: every discovered CPU came up fully wired -- online flag, its
 * own idle task, sane id -- and this CPU's %gs anchor agrees with
 * current_task() (the one thing every other SMP test silently leans on). */
static void test_smp_topology(void) {
    int discovered = smp_cpu_count();
    KT_ASSERT(discovered >= 1);
    int online = smp_cpus_online();
    if (online != discovered) {
        kprintf("  %d cpu(s) discovered, only %d online\n", discovered, online);
        cur_failed = 1;
        return;
    }
    for (int i = 0; i < discovered; i++) {
        if (!cpus[i].online || !cpus[i].idle || cpus[i].id != i) {
            kprintf("  cpu%d not fully wired (online=%d idle=%p id=%d)\n",
                    i, cpus[i].online ? 1 : 0, cpus[i].idle, cpus[i].id);
            cur_failed = 1;
            return;
        }
    }
    KT_ASSERT(this_cpu()->id >= 0 && this_cpu()->id < discovered);
    KT_ASSERT(this_cpu()->current == current_task());
}

/* test 32: the lock-correctness classic -- four threads hammer one counter
 * through a spinlock from every CPU at once. Exact arithmetic, no timing:
 * any lost update means the mutual exclusion (or the irqsave discipline
 * around it) is broken. */
static spinlock_t smp_cnt_lock = SPINLOCK_INIT;
static volatile int smp_shared_counter;

static void smp_count_thread(void *arg) {
    (void)arg;
    for (int i = 0; i < 25000; i++) {
        irq_flags_t f;
        spin_lock_irqsave(&smp_cnt_lock, &f);
        smp_shared_counter++;
        spin_unlock_irqrestore(&smp_cnt_lock, f);
    }
    task_exit(0);
}

static void test_smp_spinlock_contention(void) {
    smp_shared_counter = 0;
    struct task *ts[4];
    for (int i = 0; i < 4; i++) {
        ts[i] = task_create_kernel("smp-cnt", smp_count_thread, NULL, 5);
        KT_ASSERT(ts[i] != NULL);
    }
    for (int i = 0; i < 4; i++) reap_expect(ts[i], 0);
    if (smp_shared_counter != 4 * 25000) {
        kprintf("  counter: got %d want %d (lost updates!)\n",
                smp_shared_counter, 4 * 25000);
        cur_failed = 1;
    }
}

/* test 33: proof the APs really execute tasks. Six threads yield in a
 * loop, recording which CPU each resumption ran on; with ncpu >= 2 the
 * union must span more than one CPU (an idle AP pulls work within one
 * 10 ms timer tick while the threads yield thousands of times). Every
 * resample re-derives the thread's own pid through %gs: if two CPUs ever
 * shared a GS base, a thread scheduled next to another would observe
 * someone else's pid. */
static volatile uint8_t smp_cpu_seen[SMP_MAX];
static volatile uint8_t smp_pid_seen[TASK_MAX];
static volatile int smp_gs_broken;

static void smp_migrate_thread(void *arg) {
    (void)arg;
    int pid = current_task()->pid;              /* first look through MY %gs */
    smp_pid_seen[pid] = 1;
    for (int i = 0; i < 400; i++) {
        if (current_task()->pid != pid) smp_gs_broken = 1;
        smp_cpu_seen[this_cpu()->id] = 1;
        kyield();
    }
    task_exit(0);
}

static void test_smp_cross_cpu_run(void) {
    memset((void *)smp_cpu_seen, 0, sizeof(smp_cpu_seen));
    memset((void *)smp_pid_seen, 0, sizeof(smp_pid_seen));
    smp_gs_broken = 0;

    struct task *ts[6];
    int pids[6];
    for (int i = 0; i < 6; i++) {
        ts[i] = task_create_kernel("smp-mig", smp_migrate_thread, NULL, 5);
        KT_ASSERT(ts[i] != NULL);
        pids[i] = ts[i]->pid;
    }
    for (int i = 0; i < 6; i++) reap_expect(ts[i], 0);

    KT_ASSERT(smp_gs_broken == 0);
    for (int i = 0; i < 6; i++)
        if (!smp_pid_seen[pids[i]]) {
            kprintf("  pid %d never observed itself (shared GS base?)\n", pids[i]);
            cur_failed = 1;
        }

    int ncpu_seen = 0;
    for (int i = 0; i < SMP_MAX; i++) ncpu_seen += smp_cpu_seen[i];
    if (ncpu < 2) {
        if (ncpu_seen != 1) {
            kprintf("  %d distinct CPUs seen on a 1-CPU machine\n", ncpu_seen);
            cur_failed = 1;
        }
    } else if (ncpu_seen < 2) {
        kprintf("  tasks never left one CPU (APs not pulling work?)\n");
        cur_failed = 1;
    }
}

/* test 34: TLB shootdown under real eviction. Same forced-watermark round
 * trip as the vm suite, but with every CPU scheduling: kswapd rewrites
 * PTEs from whichever CPU it lands on and each rewrite broadcasts the
 * shootdown IPI. Data integrity is memhog's own verification (exit 88);
 * the IPI counter is the plumbing proof. */
static void test_smp_tlb_shootdown(void) {
    uint32_t ipis0 = smp_tlb_ipis_sent();
    struct task *t = task_spawn_path("/bin/memhog");
    KT_ASSERT(t != NULL);
    swap_set_watermark(pmm_free_pages() + 64);   /* always scan */
    reap_expect(t, 88);
    swap_set_watermark(0);                       /* before any assert bails */
    if (ncpu >= 2 && smp_tlb_ipis_sent() == ipis0) {
        kprintf("  eviction happened but no TLB-shootdown IPI was sent\n");
        cur_failed = 1;
    }
}

static const struct ktest_case smp_cases[] = {
    { "smp_topology", test_smp_topology },
    { "spinlock_contention", test_smp_spinlock_contention },
    { "cross_cpu_run", test_smp_cross_cpu_run },
    { "tlb_shootdown_ipi", test_smp_tlb_shootdown },
};

int ktest_run_smp(void) {
    run_cases("smp", smp_cases, sizeof(smp_cases) / sizeof(smp_cases[0]));
    sched_set_policy(SCHED_PRIORITY);
    return failed;
}
