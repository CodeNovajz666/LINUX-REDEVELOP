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
#define SYS_PS          39
#define SYS_CPUS        40
#define SYS_MEMINFO     41
/* v1.1.0 -- user-space toolbox */
#define SYS_GETDENTS    42
#define SYS_UNLINK      43
#define SYS_DATETIME    44
#define SYS_RENAME      45
#define SYS_RMDIR       46
#define SYS_SETREUID    47
#define SYS_SETREGID    48
#define SYS_ACCESS      49
#define TCGETS          0
#define TCSETS          1
#define TCCLEAR         2

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
/* v1.0.1: the kernel returns the reaped child's pid and hands the exit
 * status out through a pointer; uwaitpid keeps its historical contract
 * (returns the status, or -1) so its callers don't change. */
int uwaitpid(int pid) {
    int st = 0;
    if (_sys3(SYS_WAITPID, pid, 0, (long)&st) < 0) return -1;
    return st;
}
int ugetchar(void) { return (int)_sys3(SYS_GETCHAR, 0, 0, 0); }
void uexit(int status) { _sys3(SYS_EXIT, status, 0, 0); for (;;) __asm__ volatile("hlt"); }

int ufork(void) { return (int)_sys3(SYS_FORK, 0, 0, 0); }

/* ---- signals / heap / pipes (v0.5.0) ---- */

int ukill(int pid, int sig) { return (int)_sys3(SYS_KILL, pid, sig, 0); }
long usbrk(long inc) { return _sys3(SYS_SBRK, inc, 0, 0); }
int upipe(int fds[2]) { return (int)_sys3(SYS_PIPE, (long)fds, 0, 0); }
/* WNOHANG poll (v1.0.1): >0 = reaped that child's pid, 0 = kids still
 * alive, -1 = no such child. The reaped side used to return the exit
 * status itself, so "exited 0" was indistinguishable from "alive". */
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

/* debug reports (v1.0.0): kernel-formatted ps/cpus/mem tables, appended
 * truncating into buf; return bytes stored, -1 on a bad buffer/len */
int ups(char *buf, int len) { return (int)_sys3(SYS_PS, (long)buf, len, 0); }
int ucpus(char *buf, int len) { return (int)_sys3(SYS_CPUS, (long)buf, len, 0); }
int umeminfo(char *buf, int len, int verbose) {
    return (int)_sys3(SYS_MEMINFO, (long)buf, len, verbose);
}

/* v1.1.0 -- user-space toolbox: the /bin programs (ls/rm/date/clear) are
 * thin clients of these; the kernel formats listings and the date line. */
int ugetdents(const char *path, char *buf, int len) {
    return (int)_sys3(SYS_GETDENTS, (long)path, (long)buf, len);
}
int uunlink(const char *path) {
    return (int)_sys3(SYS_UNLINK, (long)path, 0, 0);
}
int urename(const char *oldpath, const char *newpath) {
    return (int)_sys3(SYS_RENAME, (long)oldpath, (long)newpath, 0);
}
int urmdir(const char *path) {
    return (int)_sys3(SYS_RMDIR, (long)path, 0, 0);
}
int usetreuid(int ruid, int euid) {
    return (int)_sys3(SYS_SETREUID, ruid, euid, 0);
}
int usetregid(int rgid, int egid) {
    return (int)_sys3(SYS_SETREGID, rgid, egid, 0);
}
int uaccess(const char *path, int mode) {
    return (int)_sys3(SYS_ACCESS, (long)path, mode, 0);
}
int udatetime(char *buf, int len) {
    return (int)_sys3(SYS_DATETIME, (long)buf, len, 0);
}
int uclear(void) {
    return (int)_sys3(SYS_IOCTL, 1, TCCLEAR, 0);
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

/* v1.1.5: the kernel wants NULL-terminated vectors; our callers hand us
 * (argv, argc) pairs, so both wrappers build the terminated copy on the
 * stack (TASK_MAX_ARGV slots -- the kernel cap) */
#define U_ARGV_MAX 8

int uspawn_argv(const char *name, const char *const argv[], int argc) {
    if (argc < 0 || argc > U_ARGV_MAX) return -1;
    const char *tmp[U_ARGV_MAX + 1];
    for (int i = 0; i < argc; i++) tmp[i] = argv[i];
    tmp[argc] = 0;
    return (int)_sys3(SYS_SPAWN, (long)name, (long)tmp, 0);
}

/* plain exec: argv pair + the caller's own environ (Unix semantics --
 * what `environ` holds is exactly what the child sees) */
int uexec_argv(const char *path, const char *const argv[], int argc) {
    if (argc < 0 || argc > U_ARGV_MAX) return -1;
    const char *tmp[U_ARGV_MAX + 1];
    for (int i = 0; i < argc; i++) tmp[i] = argv[i];
    tmp[argc] = 0;
    return (int)_sys3(SYS_EXEC, (long)path, (long)tmp, (long)environ);
}

/* explicit environment (osh's exported-variable table) */
int uexecve(const char *path, const char *const argv[], const char *const envp[]) {
    return (int)_sys3(SYS_EXEC, (long)path, (long)argv, (long)envp);
}

/* ---- v1.1.1 line editor ----
 *
 * The edited line shares one screen row with the prompt, and the console
 * understands exactly two cursor motions ('\b' one left, printing a char one
 * right), so every operation is expressed as: back up, reprint the tail,
 * blank the leftovers, back up onto the cursor. The device echo prints each
 * typed char itself; everything the editor emits is explicit redraw.
 *
 * Column bookkeeping (pos measures columns right of the line start):
 *   insert c at pos: device already echoed c; print buf[pos+1..n), '\b'*(n-pos-1)
 *   delete d chars ending at pos: '\b'*d + tail(t bytes) + ' '*d + '\b'*(t+d)
 *   replace line (history): '\b'*pos + new line + ' '*pad + '\b'*(len+pad-pos')
 */
static void ed_emit(const char *s, int n) { uwrite_fd(1, s, n); }

static void ed_backs(int n) {
    for (int i = 0; i < n; i++) ed_emit("\b", 1);
}

static void ed_spaces(int n) {
    for (int i = 0; i < n; i++) ed_emit(" ", 1);
}

/* delete the d chars immediately left of the cursor */
static void ed_del_before(char *buf, int *n, int *pos, int d) {
    if (d <= 0 || d > *pos) return;
    int t = *n - *pos;                        /* tail right of the cursor */
    umemmove(buf + *pos - d, buf + *pos, (unsigned long)t);
    *pos -= d;
    *n -= d;
    buf[*n] = 0;
    ed_backs(d);
    ed_emit(buf + *pos, t);
    ed_spaces(d);
    ed_backs(t + d);
}

/* swap the whole edited line for s (history recall); cursor lands at end */
static void ed_set(char *buf, int *n, int *pos, int cap, const char *s) {
    ed_backs(*pos);
    int old = *n, i = 0;
    while (s[i] && i < cap - 1) { buf[i] = s[i]; i++; }
    *n = i;
    *pos = i;
    buf[i] = 0;
    ed_emit(buf, i);
    int pad = old > i ? old - i : 0;          /* blank the old tail */
    ed_spaces(pad);
    ed_backs(pad);
}

int ureadline_ex(char *buf, int max, uhist_cb hist, void *ctx) {
    int n = 0, pos = 0;                       /* line length, cursor column */
    char live[128];                           /* the half-typed line, parked
                                                 while history is browsed */
    int have_live = 0, hpos = 0;              /* browse depth; 0 = live line */
    for (;;) {
        char c;
        int r = uread(0, &c, 1);
        if (r != 1) {                         /* v1.1.1: EOF/error ends the
                                                   read (the old editor spun
                                                   forever on a closed stdin) */
            buf[n] = 0;
            return -1;
        }
        if (c == '\n') break;
        if (c == '\b' || c == 0x7f) {         /* erase the char left of pos */
            ed_del_before(buf, &n, &pos, 1);
            continue;
        }
        if (c == 0x15) {                      /* Ctrl-U: kill to line start */
            ed_del_before(buf, &n, &pos, pos);
            continue;
        }
        if (c == 0x17) {                      /* Ctrl-W: kill previous word */
            int k = pos;
            while (k > 0 && buf[k - 1] == ' ') k--;
            while (k > 0 && buf[k - 1] != ' ') k--;
            ed_del_before(buf, &n, &pos, pos - k);
            continue;
        }
        if ((unsigned char)c == UKEY_UP && hist) {
            const char *s = hist(hpos + 1, ctx);
            if (!s) continue;                 /* already at the oldest entry */
            if (hpos == 0) {                  /* park the half-typed line */
                ustrncpy(live, buf, (unsigned long)n);
                live[n] = 0;
                have_live = 1;
            }
            hpos++;
            ed_set(buf, &n, &pos, max, s);
            continue;
        }
        if ((unsigned char)c == UKEY_DOWN && hist) {
            if (hpos == 0) continue;
            hpos--;
            if (hpos == 0 && have_live) ed_set(buf, &n, &pos, max, live);
            else {
                const char *s = hist(hpos, ctx);
                if (s) ed_set(buf, &n, &pos, max, s);
            }
            continue;
        }
        if ((unsigned char)c == UKEY_LEFT)  { if (pos > 0) { ed_backs(1); pos--; } continue; }
        if ((unsigned char)c == UKEY_RIGHT) { if (pos < n) { ed_emit(" ", 1); pos++; } continue; }
        if ((unsigned char)c == UKEY_HOME)  { ed_backs(pos); pos = 0; continue; }
        if ((unsigned char)c == UKEY_END)   { ed_emit(buf + pos, n - pos); pos = n; continue; }
        if (c >= 32 && c < 127 && n < max - 1) {
            /* the device echoed c at the old cursor; reprint the tail that
             * now sits right of it, then step back onto the cursor */
            int t = n - pos;
            umemmove(buf + pos + 1, buf + pos, (unsigned long)t);
            buf[pos++] = c;
            n++;
            buf[n] = 0;
            ed_emit(buf + pos, t);
            ed_backs(t);
        }
        /* other control bytes (Ctrl-C among them) are not the editor's
         * business -- dropped, exactly as the old reader did */
    }
    buf[n] = 0;
    return n;
}

int ureadline(char *buf, int max) {
    return ureadline_ex(buf, max, 0, 0);
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
    for (unsigned long i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

/* v1.1.1: overlapping-safe copy (the line editor shifts its own buffer) */
void *umemmove(void *dst, const void *src, unsigned long n) {
    char *d = dst;
    const char *s = src;
    if (d < s) {
        for (unsigned long i = 0; i < n; i++) d[i] = s[i];
    } else if (d > s) {
        for (unsigned long i = n; i > 0; i--) d[i - 1] = s[i - 1];
    }
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
