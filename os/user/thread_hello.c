#include "ulib.h"

/* thread_hello (v1.2.0): two pthreads on a shared address space.
 *
 * The demo has two jobs:
 *   1. correctness -- both threads hammer one shared counter through the
 *      pthread mutex; the total must be exact (CLONE_VM made the .bss
 *      common, the mutex kept the read-modify-writes from racing).
 *   2. cross-cpu evidence -- the threads start staggered (slot 1 sleeps
 *      first) and sleep 20 ms between chunks, so each wake parks a wide
 *      runnable window that the OTHER cpu's tick adopts; each thread
 *      samples the kernel `ps` CPU column for its own tid and the
 *      COMBINED mask must light >= 2 bits.
 *
 * Exit 0 = combined mask >= 2 cpus AND the counter is exact; every
 * failure mode prints its own line and exits 1. */

#define NTHREADS   2
#define CHUNKS     40
#define CHUNK_INC  250            /* CHUNKS * CHUNK_INC * NTHREADS = 20000 */
#define SAMPLES    200            /* ps polls per thread (cpus seen) */
#define PS_BUF     2048

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static volatile long counter;
static volatile unsigned cpu_mask[NTHREADS];
static volatile int slot_tid[NTHREADS];     /* slot -> tid, set by the worker */

/* the pid row's CPU column from the kernel ps table (field 4 of
 * "PID PPID STATE PRI CPU UID ...") */
static int ps_cpu_of(int pid, char *buf, int cap) {
    int n = ups(buf, cap - 1);
    if (n <= 0) return -1;
    buf[n] = 0;
    char want[16];
    {
        unsigned u = (unsigned)pid;
        char tmp[12];
        int t = 0, len = 0;
        do { tmp[t++] = (char)('0' + u % 10); u /= 10; } while (u);
        while (t > 0) want[len++] = tmp[--t];
        want[len++] = ' ';
        want[len] = 0;
    }
    for (char *p = buf; *p; ) {
        char *e = p;
        while (*e && *e != '\n') e++;
        if (ustrncmp(p, want, (unsigned long)ustrlen(want)) == 0) {
            int field = 0;
            char *q = p;
            while (*q && *q != '\n' && field <= 4) {
                while (*q == ' ') q++;
                if (field == 4) {
                    int cpu = 0;
                    while (*q >= '0' && *q <= '9') cpu = cpu * 10 + (*q++ - '0');
                    return cpu;
                }
                while (*q && *q != ' ' && *q != '\n') q++;
                field++;
            }
            return -1;
        }
        p = *e ? e + 1 : e;
    }
    return -1;
}

static void *worker(void *arg) {
    int slot = (int)(long)arg;
    char buf[PS_BUF];
    slot_tid[slot] = ugetpid();
    if (slot == 1) usleep(300);           /* stagger: overlap with slot 0 */
    for (int c = 0; c < CHUNKS; c++) {
        for (int i = 0; i < CHUNK_INC; i++) {
            pthread_mutex_lock(&lock);
            counter++;
            pthread_mutex_unlock(&lock);
        }
        /* a short sleep parks the thread with a WIDE runnable window --
         * whoever wakes it may be the other CPU, and an idle CPU adopts
         * parked tasks at its next tick. A pure yield keeps both threads
         * on the quantum clock of one CPU and migration stays a coin
         * flip; the sleep makes the cross-cpu evidence deterministic. */
        usleep(20);
        for (int s = 0; s < SAMPLES / CHUNKS; s++) {
            int cpu = ps_cpu_of(ugetpid(), buf, sizeof(buf));
            if (cpu >= 0 && cpu < 8) cpu_mask[slot] |= 1u << cpu;
        }
    }
    return (void *)(long)(slot + 1);          /* distinctive exit codes */
}

static int slot_of(int tid) {
    for (int j = 0; j < NTHREADS; j++)
        if (slot_tid[j] == tid) return j;
    return -1;
}

int main(void) {
    pthread_t t[NTHREADS];
    counter = 0;
    for (int i = 0; i < NTHREADS; i++) { cpu_mask[i] = 0; slot_tid[i] = 0; }

    uprintf("thread_hello: pid %d spawning %d threads\n", ugetpid(), NTHREADS);
    for (int i = 0; i < NTHREADS; i++)
        if (pthread_create(&t[i], worker, (void *)(long)i) != 0) {
            uputs("thread_hello: pthread_create failed\n");
            return 1;
        }
    for (int i = 0; i < NTHREADS; i++) {
        int code = -1;
        if (pthread_join(t[i], &code) != 0) {
            uprintf("thread_hello: join %d failed\n", t[i]);
            return 1;
        }
        uprintf("thread_hello: joined tid %d (exit %d) cpu mask 0x%x\n",
                t[i], code, slot_of(t[i]) >= 0 ? cpu_mask[slot_of(t[i])] : 0);
    }
    long want = (long)CHUNKS * CHUNK_INC * NTHREADS;
    uprintf("thread_hello: shared counter %ld (want %ld)\n", counter, want);
    if (counter != want) return 1;

    unsigned both = cpu_mask[0] | cpu_mask[1];
    int bits = 0;
    for (int b = 0; b < 8; b++) if (both & (1u << b)) bits++;
    uprintf("thread_hello: combined cpu mask 0x%x (%d cpu%s)\n",
            both, bits, bits == 1 ? "" : "s");
    if (bits < 2) {
        uputs("thread_hello: WARNING -- never on two cpus at once\n");
        return 1;
    }
    uputs("thread_hello: cross-cpu evidence OK (threads ran in parallel)\n");
    return 0;
}
