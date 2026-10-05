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
/* v0.9.0 -- users & login */
#define SYS_GETUID      27
#define SYS_SETUID      28
#define SYS_GETGID      29
#define SYS_SETGID      30
#define SYS_CHDIR       31
#define SYS_GETCWD      32
#define SYS_IOCTL       33
#define SYS_POWEROFF    34
#define SYS_MKDIR       35
#define SYS_CHOWN       36
#define SYS_GETRUID     37
#define SYS_DUP2        38
#define TCGETS          0
#define TCSETS          1

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
void uputs(const char *s) { uwrite(s, (int)ustrlen(s)); }
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
int udup2(int oldfd, int newfd) { return (int)_sys3(SYS_DUP2, oldfd, newfd, 0); }
int ulseek(int fd, int off, int whence) { return (int)_sys3(SYS_LSEEK, fd, off, whence); }
int ustat(const char *path, struct ustat *st) { return (int)_sys3(SYS_STAT, (long)path, (long)st, 0); }

void *ummap(unsigned long len) {
    long va = _sys3(SYS_MMAP, (long)len, 0, 0);
    return va < 0 ? (void *)0 : (void *)va;
}

int umunmap(void *va, unsigned long len) {
    return (int)_sys3(SYS_MUNMAP, (long)va, (long)len, 0);
}

/* ---- users & login (v0.9.0) ---- */

int ugetuid(void) { return (int)_sys3(SYS_GETUID, 0, 0, 0); }
int ugetruid(void) { return (int)_sys3(SYS_GETRUID, 0, 0, 0); }
int ugetgid(void) { return (int)_sys3(SYS_GETGID, 0, 0, 0); }
int usetuid(int uid) { return (int)_sys3(SYS_SETUID, uid, 0, 0); }
int usetgid(int gid) { return (int)_sys3(SYS_SETGID, gid, 0, 0); }
int uchdir(const char *path) { return (int)_sys3(SYS_CHDIR, (long)path, 0, 0); }
int ugetcwd(char *buf, int len) { return (int)_sys3(SYS_GETCWD, (long)buf, len, 0); }
int umkdir(const char *path) { return (int)_sys3(SYS_MKDIR, (long)path, 0, 0); }
int uchown(const char *path, int uid, int gid) {
    return (int)_sys3(SYS_CHOWN, (long)path, uid, gid);
}
unsigned long uuptime(void) { return (unsigned long)_sys3(SYS_UPTIME, 0, 0, 0); }
void upoweroff(void) {
    _sys3(SYS_POWEROFF, 0, 0, 0);
    for (;;) __asm__ volatile("hlt");
}

/* terminal echo: TCGETS/TCSETS on fd 0. login turns echo off before the
 * password and back on after; the kernel's console device honors the bit
 * on its read path, so typed characters come back (or don't) automatically. */
int uecho_set(int on) {
    unsigned short fl = 0;
    if (_sys3(SYS_IOCTL, 0, TCGETS, (long)&fl) != 0) return -1;
    if (on) fl |= U_TC_ECHO;
    else fl &= (unsigned short)~U_TC_ECHO;
    return (int)_sys3(SYS_IOCTL, 0, TCSETS, (long)&fl);
}

int uspawn_argv(const char *name, const char *const argv[], int argc) {
    return (int)_sys3(SYS_SPAWN, (long)name, (long)argv, argc);
}
int uexec_argv(const char *path, const char *const argv[], int argc) {
    return (int)_sys3(SYS_EXEC, (long)path, (long)argv, argc);
}

/* read one line from fd 0. The console device echoes printables and \n
 * itself while the echo flag is on (see dev_console_read); backspace
 * editing is the reader's job, hence the explicit "\b \b" here. Returns
 * the line length without the newline; the buffer is NUL-terminated. */
int ureadline(char *buf, int max) {
    int n = 0;
    for (;;) {
        char c;
        if (uread(0, &c, 1) != 1) continue;
        if (c == '\n') break;
        if (c == '\b' || c == 0x7f) {
            if (n > 0) {
                n--;
                uwrite("\b \b", 3);
            }
            continue;
        }
        if (c >= 32 && c < 127 && n < max - 1)
            buf[n++] = c;
    }
    buf[n] = 0;
    return n;
}

/* ---- freestanding string helpers ---- */

unsigned long ustrlen(const char *s) {
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

int ustrcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int ustrncmp(const char *a, const char *b, unsigned long n) {
    for (; n; n--, a++, b++) {
        if (*a != *b) return (int)(unsigned char)*a - (int)(unsigned char)*b;
        if (!*a) break;
    }
    return 0;
}

char *ustrcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++) != 0) ;
    return dst;
}

char *ustrncpy(char *dst, const char *src, unsigned long n) {
    unsigned long i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = 0;
    return dst;
}

char *ustrchr(const char *s, int c) {
    for (; *s; s++)
        if (*s == (char)c) return (char *)s;
    return c == 0 ? (char *)s : (char *)0;
}

char *ustrstr(const char *hay, const char *needle) {
    if (!*needle) return (char *)hay;
    for (; *hay; hay++) {
        const char *h = hay, *nd = needle;
        while (*h == *nd && *nd) { h++; nd++; }
        if (!*nd) return (char *)hay;
        if (!*h) break;
    }
    return (char *)0;
}

void *umemcpy(void *dst, const void *src, unsigned long n) {
    char *d = dst;
    const char *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

void *umemset(void *dst, int c, unsigned long n) {
    char *d = dst;
    while (n--) *d++ = (char)c;
    return dst;
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
