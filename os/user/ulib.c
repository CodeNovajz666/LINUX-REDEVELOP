#include "ulib.h"

#define SYS_EXIT        1
#define SYS_WRITE       2
#define SYS_GETPID      3
#define SYS_YIELD       4
#define SYS_SLEEP       5
#define SYS_SPAWN       6
#define SYS_WAITPID     7
#define SYS_GETCHAR     8
#define SYS_SET_POLICY  9
#define SYS_UPTIME      10
#define SYS_OPEN        11
#define SYS_READ        12
#define SYS_CLOSE       13
#define SYS_LSEEK       14
#define SYS_STAT        15
#define SYS_FORK        16
#define SYS_EXEC        17
#define SYS_FREEPAGES   18
#define SYS_SBRK        19
#define SYS_KILL        20
#define SYS_SIGACTION   21
#define SYS_SIGRETURN   22
#define SYS_PIPE        23
#define SYS_SIGPROCMASK 24
#define SYS_MMAP        25
#define SYS_MUNMAP      26

/* pipe read/write "blocking, retry" marker (see kernel fs/pipe.c) */
#define EAGAIN_RETRY (-2L)

/* syscall ABI (v0.3.0, x86_64): rax = number, rdi/rsi/rdx = args,
 * result in rax (sign-extended -1 on error) */
static inline long _sys3(long nr, long a, long b, long c) {
    long ret;
    register long rdi __asm__("rdi") = a;
    register long rsi __asm__("rsi") = b;
    register long rdx __asm__("rdx") = c;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(nr), "r"(rdi), "r"(rsi), "r"(rdx)
                     : "memory");
    return ret;
}

int uwrite(const char *buf, int len) { return (int)_sys3(SYS_WRITE, 1, (long)buf, len); }
int ugetpid(void) { return (int)_sys3(SYS_GETPID, 0, 0, 0); }
int uyield(void) { return (int)_sys3(SYS_YIELD, 0, 0, 0); }
int usleep(int ms) { return (int)_sys3(SYS_SLEEP, ms, 0, 0); }
int uspawn(const char *name) { return (int)_sys3(SYS_SPAWN, (long)name, 0, 0); }
int uwaitpid(int pid) { return (int)_sys3(SYS_WAITPID, pid, 0, 0); }
int ugetchar(void) { return (int)_sys3(SYS_GETCHAR, 0, 0, 0); }
void uexit(int status) { _sys3(SYS_EXIT, status, 0, 0); for (;;) __asm__ volatile("hlt"); }

int ufork(void) { return (int)_sys3(SYS_FORK, 0, 0, 0); }

/* ---- signals / heap / pipes (v0.5.0) ---- */

int ukill(int pid, int sig) { return (int)_sys3(SYS_KILL, pid, sig, 0); }
long usbrk(long inc) { return _sys3(SYS_SBRK, inc, 0, 0); }
int upipe(int fds[2]) { return (int)_sys3(SYS_PIPE, (long)fds, 0, 0); }
int uwaitpid_nohang(int pid) { return (int)_sys3(SYS_WAITPID, pid, U_WNOHANG, 0); }

/* sigreturn trampoline: the kernel pushes this address as the handler's
 * return target; the handler's `ret` lands here and we re-enter the kernel
 * to have the pre-signal frame (saved on the user stack) restored. */
__asm__(
    ".globl __u_sigreturn\n"
    "__u_sigreturn:\n"
    "   mov $22, %rax\n"        /* SYS_SIGRETURN */
    "   int $0x80\n"
    "   hlt\n"                  /* kernel restores us or kills us */
);
extern void __u_sigreturn(void);

int usignal(int sig, usighandler_t h) {
    return (int)_sys3(SYS_SIGACTION, sig, (long)h, (long)&__u_sigreturn);
}

int usigprocmask(int how, const unsigned int *set, unsigned int *oldset) {
    return (int)_sys3(SYS_SIGPROCMASK, how, (long)set, (long)oldset);
}

/* pipe blocking semantics live here: the kernel returns -2 when the caller
 * must wait (empty pipe with writers alive / full pipe with readers alive);
 * retrying in user space yields Unix blocking reads/writes with tick-level
 * latency. Non-pipe files never return -2, so behavior is unchanged. */
int uread(int fd, void *buf, int len) {
    for (;;) {
        long n = _sys3(SYS_READ, fd, (long)buf, len);
        if (n != EAGAIN_RETRY) return (int)n;
    }
}

int uwrite_fd(int fd, const void *buf, int len) {
    for (;;) {
        long n = _sys3(SYS_WRITE, fd, (long)buf, len);
        if (n != EAGAIN_RETRY) return (int)n;
    }
}

/* ---- malloc/free: first-fit free list with coalescing ----
 *
 * Chunk layout (K&R style): every block starts with a size header that
 * includes the header itself. Free blocks additionally hold the free-list
 * link. The list is kept sorted by address so coalescing is a matter of
 * comparing adjacent chunk ends. */
typedef struct chunk {
    unsigned long size;         /* payload + header, multiple of 16 */
    struct chunk *next;         /* only meaningful while free */
} chunk_t;

static chunk_t *free_list;

static void free_insert(chunk_t *c) {
    chunk_t *prev = 0, *cur = free_list;
    while (cur && (unsigned long)cur < (unsigned long)c) {
        prev = cur;
        cur = cur->next;
    }
    c->next = cur;
    if (prev) prev->next = c; else free_list = c;
    if (cur && (unsigned long)c + c->size == (unsigned long)cur) {   /* merge right */
        c->size += cur->size;
        c->next = cur->next;
    }
    if (prev && (unsigned long)prev + prev->size == (unsigned long)c) { /* merge left */
        prev->size += c->size;
        prev->next = c->next;
    }
}

void *umalloc(unsigned long n) {
    if (n == 0) n = 1;
    unsigned long need = (n + sizeof(chunk_t) + 15UL) & ~15UL;

retry:
    for (chunk_t **pp = &free_list; *pp; pp = &(*pp)->next) {
        chunk_t *p = *pp;
        if (p->size < need) continue;
        if (p->size >= need + sizeof(chunk_t) + 16UL) {   /* split */
            chunk_t *rest = (chunk_t *)((unsigned long)p + need);
            rest->size = p->size - need;
            rest->next = p->next;
            *pp = rest;
            p->size = need;
        } else {                                           /* take whole */
            *pp = p->next;
        }
        return (void *)(unsigned long)(p + 1);
    }

    /* nothing fits: grow the break and retry (the insert coalesces with a
     * trailing free chunk, so repeated small mallocs stay O(1) syscalls) */
    unsigned long grow = (need + 4095UL) & ~4095UL;
    long old_brk = usbrk((long)grow);
    if (old_brk < 0) return 0;
    chunk_t *fresh = (chunk_t *)(unsigned long)old_brk;
    fresh->size = grow;
    fresh->next = 0;
    free_insert(fresh);
    goto retry;
}

void ufree(void *ptr) {
    if (!ptr) return;
    chunk_t *c = ((chunk_t *)ptr) - 1;
    free_insert(c);
}
int uexec(const char *path) { return (int)_sys3(SYS_EXEC, (long)path, 0, 0); }
int ufreepages(void) { return (int)_sys3(SYS_FREEPAGES, 0, 0, 0); }

int uopen(const char *path, int flags) { return (int)_sys3(SYS_OPEN, (long)path, flags, 0); }
int uclose(int fd) { return (int)_sys3(SYS_CLOSE, fd, 0, 0); }
int ulseek(int fd, int off, int whence) { return (int)_sys3(SYS_LSEEK, fd, off, whence); }
int ustat(const char *path, struct ustat *st) { return (int)_sys3(SYS_STAT, (long)path, (long)st, 0); }

void *ummap(unsigned long len) {
    long va = _sys3(SYS_MMAP, (long)len, 0, 0);
    return va < 0 ? (void *)0 : (void *)va;
}

int umunmap(void *va, unsigned long len) {
    return (int)_sys3(SYS_MUNMAP, (long)va, (long)len, 0);
}

static void print_uint(unsigned long v, unsigned base, int width, char pad) {
    char buf[32];
    const char *digits = "0123456789abcdef";
    int i = 0;
    if (v == 0) buf[i++] = '0';
    while (v) { buf[i++] = digits[v % base]; v /= base; }
    for (int p = i; p < width; p++) {
        char c = pad;
        uwrite(&c, 1);
    }
    while (i > 0) uwrite(&buf[--i], 1);
}

static void print_int(long v, unsigned base, int width, char pad) {
    if (v < 0 && base == 10) {
        char c = '-';
        uwrite(&c, 1);
        print_uint((unsigned long)(-v), base, width ? width - 1 : 0, pad);
    } else {
        print_uint((unsigned long)v, base, width, pad);
    }
}

int uprintf(const char *fmt, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    for (; *fmt; fmt++) {
        if (*fmt != '%') { uwrite(fmt, 1); continue; }
        fmt++;
        int width = 0;
        char pad = ' ';
        if (*fmt == '0') { pad = '0'; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }
        int is_long = 0;                         /* 'l' size modifier: %ld %lu %lx */
        if (*fmt == 'l') { is_long = 1; fmt++; }
        switch (*fmt) {
        case 'd': case 'i':
            print_int(is_long ? __builtin_va_arg(ap, long)
                              : (long)__builtin_va_arg(ap, int), 10, width, pad);
            break;
        case 'u': print_uint(is_long ? __builtin_va_arg(ap, unsigned long)
                                     : (unsigned long)__builtin_va_arg(ap, unsigned int), 10, width, pad); break;
        case 'x': print_uint(is_long ? __builtin_va_arg(ap, unsigned long)
                                     : (unsigned long)__builtin_va_arg(ap, unsigned int), 16, width, pad); break;
        case 's': { const char *s = __builtin_va_arg(ap, const char *);
                    if (!s) s = "(null)";
                    while (*s) uwrite(s++, 1); break; }
        case 'c': { char c = (char)__builtin_va_arg(ap, int); uwrite(&c, 1); break; }
        case '%': uwrite("%", 1); break;
        case '\0': goto done;
        default: uwrite("%", 1); uwrite(fmt, 1); break;
        }
    }
done:
    __builtin_va_end(ap);
    return 0;
}
