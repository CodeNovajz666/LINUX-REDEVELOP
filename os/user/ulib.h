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
int udatetime(char *buf, int len);
int uclear(void);                       /* ioctl TCCLEAR on stdout */

/* terminal echo (TCGETS/TCSETS subset over fd 0-2) */
#define U_TC_ECHO 0x0008
int uecho_set(int on);                       /* login: off before the password */

/* argv-capable spawn/exec (v0.9.0 ABI: rsi = argv, rdx = argc) */
int uspawn_argv(const char *name, const char *const argv[], int argc);
int uexec_argv(const char *path, const char *const argv[], int argc);

/* read one line from fd 0 (echoed by the console device; handles \b/DEL
 * editing). Returns the length, buffer NUL-terminated. */
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
void *umemset(void *dst, int c, unsigned long n);

#endif /* ULIB_H */
