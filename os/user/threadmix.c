#include "ulib.h"

/* threadmix (v1.2.0): the fork + thread MIX the gate F extension counts.
 *
 * The process spawns two pthreads; each thread forks a child (a COW twin
 * built from the SHARED address space -- mm_dup under the mm lock) and
 * reaps it, then bumps a shared counter through the mutex. The main thread
 * joins both and exits. Everything the exercise allocates -- two thread
 * stacks from ummap, four kstacks, one COW page directory per grandchild,
 * the pthread mutex traffic across shared pages -- must come back: ktest
 * snapshots pmm_free_pages() around this program and demands the same
 * number after the reaper finishes.
 *
 * Exit 0 iff: both joins returned the expected codes AND the shared
 * counter is exact AND both forked children were reaped with status 7. */

#define NTHREADS 2
#define BUMPS    10

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static volatile long counter;
static volatile int fork_status[NTHREADS];   /* what each thread's fork reaped */

static void *worker(void *arg) {
    int slot = (int)(long)arg;

    /* a thread forks: the child is a COW twin of the whole shared image */
    int pid = ufork();
    if (pid == 0) uexit(7);                   /* grandchild: distinctive */
    if (pid < 0) return (void *)(long)(20 + slot);
    int st = uwaitpid(pid);                   /* the thread reaps its own */
    fork_status[slot] = st;

    for (int i = 0; i < BUMPS; i++) {
        pthread_mutex_lock(&lock);
        counter++;
        pthread_mutex_unlock(&lock);
    }
    return (void *)5L;                        /* the code main expects */
}

int main(void) {
    pthread_t t[NTHREADS];
    counter = 0;
    fork_status[0] = fork_status[1] = -1;

    for (int i = 0; i < NTHREADS; i++)
        if (pthread_create(&t[i], worker, (void *)(long)i) != 0) return 1;
    for (int i = 0; i < NTHREADS; i++) {
        int code = -1;
        if (pthread_join(t[i], &code) != 0 || code != 5) return 2;
    }
    if (counter != (long)NTHREADS * BUMPS) return 3;
    if (fork_status[0] != 7 || fork_status[1] != 7) return 4;
    return 0;
}
