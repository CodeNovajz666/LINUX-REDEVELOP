#ifndef ULIB_H
#define ULIB_H

#include <stdbool.h>                     /* user land gets bool/true/false */

/* open(2) flags subset -- values match the kernel's vfs.h */
#define U_O_RDONLY   0x0000
#define U_O_WRONLY   0x0001
#define U_O_RDWR     0x0002
#define U_O_CREAT    0x0100
#define U_O_TRUNC    0x0200
#define U_O_APPEND   0x0400

/* lseek whence */
#define U_SEEK_SET   0
#define U_SEEK_CUR   1
#define U_SEEK_END   2

/* stat layout -- must match kernel struct stat exactly */
struct ustat {
    unsigned int   st_ino;
    unsigned short st_mode;
    unsigned int   st_size;
};

int uprintf(const char *fmt, ...);
int uwrite(const char *buf, int len);
void uputs(const char *s);
int ugetpid(void);
int uyield(void);
int usleep(int ms);
int uspawn(const char *name);
int uwaitpid(int pid);
int ugetchar(void);
void uexit(int status) __attribute__((noreturn));

/* fork/exec (v0.4.0) */
int ufork(void);                       /* 0 in the child, child pid in the parent, -1 on error */
int uexec(const char *path);           /* replaces the image; returns -1 only on failure */
int ufreepages(void);                  /* debug: kernel free page count */

/* signals / heap / pipes (v0.5.0) -- numbers match the kernel's novix.h */
#define U_SIGINT   2
#define U_SIGILL   4
#define U_SIGFPE   8
#define U_SIGKILL  9
#define U_SIGSEGV  11
#define U_SIGTERM  15
#define U_WNOHANG  1

/* usigprocmask how values (match the kernel's novix.h) */
#define U_SIG_BLOCK    0
#define U_SIG_UNBLOCK  1
#define U_SIG_SETMASK  2

typedef void (*usighandler_t)(int);

int ukill(int pid, int sig);                      /* 0 ok, -1 no such task */
int usignal(int sig, usighandler_t h);            /* h = 0 restores default */
int usigprocmask(int how, const unsigned int *set, unsigned int *oldset);
int uwaitpid_nohang(int pid);   /* WNOHANG: >0 reaped pid, 0 = kids alive, -1 = none */
long usbrk(long increment);                       /* old break, or -1 */
int upipe(int fds[2]);                            /* 0 ok */

void *umalloc(unsigned long n);                   /* NULL on failure */
void ufree(void *ptr);                            /* NULL is a no-op */

/* file syscalls (v0.2.0) */
int uopen(const char *path, int flags);
int uread(int fd, void *buf, int len);
int uwrite_fd(int fd, const void *buf, int len);
int uclose(int fd);
int udup2(int oldfd, int newfd);             /* v1.0.0: osh redirection */
int ulseek(int fd, int off, int whence);
int ustat(const char *path, struct ustat *st);

/* anonymous mmap (v0.6.0): kernel picks the address, pages fault in zeroed */
void *ummap(unsigned long len);              /* NULL on failure */
int umunmap(void *va, unsigned long len);    /* whole mappings only */

/* ---- users & login (v0.9.0) -- numbers match the kernel's syscall.h ---- */
int ugetuid(void);                           /* effective uid */
int ugetruid(void);                          /* real uid (survives SUID exec) */
int ugetgid(void);
int usetuid(int uid);                        /* root: anyone; mortal: own uid only */
int usetgid(int gid);
int uchdir(const char *path);
int ugetcwd(char *buf, int len);             /* strlen, or -1 */
int umkdir(const char *path);
int uchown(const char *path, int uid, int gid);   /* root only */
unsigned long uuptime(void);                 /* ms since boot (salt entropy) */
void upoweroff(void) __attribute__((noreturn));

/* debug reports (v1.0.0): kernel-formatted ps/cpus/mem tables, appended
 * truncating into buf; return bytes stored, -1 on a bad buffer/len */
int ups(char *buf, int len);
int ucpus(char *buf, int len);
int umeminfo(char *buf, int len, int verbose);

/* v1.1.0 -- user-space toolbox: the ls/rm/date/clear plumbing. ls listings
 * and the datetime string are kernel-formatted (truncating append, return
 * bytes stored, -1 on error); unlink refuses directories. */
int ugetdents(const char *path, char *buf, int len);
int uunlink(const char *path);
int urename(const char *oldpath, const char *newpath);   /* v1.1.2 */
int urmdir(const char *path);                            /* v1.1.2 */
int usetreuid(int ruid, int euid);            /* v1.1.7: -1 = unchanged */
int usetregid(int rgid, int egid);
int uaccess(const char *path, int mode);      /* real-id permission probe */
#define U_R_OK 4
#define U_W_OK 2
#define U_X_OK 1
int udatetime(char *buf, int len);
int uclear(void);                       /* ioctl TCCLEAR on stdout */

/* terminal echo (TCGETS/TCSETS subset over fd 0-2) */
#define U_TC_ECHO 0x0008
int uecho_set(int on);                       /* login: off before the password */

/* the environment every exec carries (crt0 fills it from the initial
 * stack; empty after an exec with no envp -- v1.1.5) */
extern char **environ;

/* argv-capable spawn/exec. v1.1.5 ABI: pointer vectors cross the kernel
 * NULL-terminated (the count variants are retired); uexec_argv exports
 * the caller's environ, uexecve takes an explicit environment. */
int uspawn_argv(const char *name, const char *const argv[], int argc);
int uexec_argv(const char *path, const char *const argv[], int argc);
int uexecve(const char *path, const char *const argv[], const char *const envp[]);

/* logical key codes -- the kernel input layer pre-parses arrow/Home/End
 * escapes (PS/2 and serial both) into these single bytes (v1.1.1) */
#define UKEY_UP     0x80
#define UKEY_DOWN   0x81
#define UKEY_RIGHT  0x82
#define UKEY_LEFT   0x83
#define UKEY_HOME   0x84
#define UKEY_END    0x85

/* ---- v1.2.0: threads & job control ---- */

#define U_CLONE_VM  0x00000100u      /* matches the kernel's CLONE_VM */

/* clone(2): CLONE_VM threads. Returns the child's tid in the parent, 0 in
 * the child (which resumes on `usp`, where pthread_create laid fn/arg),
 * -1 on error. Direct callers must know the trampoline protocol --
 * prefer pthread_create. */
int uclone(unsigned flags, void *usp);

/* process groups: fork/clone/spawn inherit; setpgid moves. pid 0 = self,
 * pgid 0 = "make the target a group leader". */
int usetpgid(int pid, int pgid);
int ugetpgrp(void);

/* console foreground group (ioctl TCSETPGRP/TCGETPGRP over fd 0): the
 * keyboard input layer routes Ctrl-C to this group as SIGINT */
int utcsetpgrp(int pgid);
int utcgetpgrp(void);

/* minimal pthread -- create/join/exit/mutex, ulib-only (the "join" is
 * waitpid: a thread is a task; the mutex is a shared-memory spinlock with
 * a yield backoff, so a preempted holder never burns the quantum). */
typedef int pthread_t;
typedef struct { volatile unsigned char locked; } pthread_mutex_t;
#define PTHREAD_MUTEX_INITIALIZER { 0 }

int pthread_create(pthread_t *tid, void *(*fn)(void *), void *arg);
int pthread_join(pthread_t tid, int *exit_code);   /* exit_code may be 0 */
void pthread_exit(int code) __attribute__((noreturn));
int pthread_mutex_lock(pthread_mutex_t *m);
int pthread_mutex_unlock(pthread_mutex_t *m);

/* history callback for ureadline_ex: return the line `back` entries before
 * the newest one entered (back = 1 -> most recent), or NULL when the caller
 * is browsing past its oldest record */
typedef const char *(*uhist_cb)(int back, void *ctx);

/* read one line from fd 0 (echoed by the console device). The v1.1.1 editor:
 * full cursor movement (Left/Right/Home/End), Backspace anywhere in the line,
 * Ctrl-U (kill to start) and Ctrl-W (kill previous word); when `hist` is
 * given, Up/Down walk it (the live line is restored on the way back down).
 * Returns the line length, buffer NUL-terminated; -1 when the input stream
 * errors (old ureadline spun forever here). */
int ureadline_ex(char *buf, int max, uhist_cb hist, void *ctx);

/* plain readline: same editor, no history browsing */
int ureadline(char *buf, int max);

/* ---- freestanding string helpers (no libc on this side of the fence) ---- */
unsigned long ustrlen(const char *s);
int ustrcmp(const char *a, const char *b);
int ustrncmp(const char *a, const char *b, unsigned long n);
char *ustrcpy(char *dst, const char *src);
char *ustrncpy(char *dst, const char *src, unsigned long n);
char *ustrchr(const char *s, int c);
char *ustrstr(const char *hay, const char *needle);   /* NULL = absent */
void *umemcpy(void *dst, const void *src, unsigned long n);
void *umemmove(void *dst, const void *src, unsigned long n);   /* overlap-safe */
void *umemset(void *dst, int c, unsigned long n);

#endif /* ULIB_H */
