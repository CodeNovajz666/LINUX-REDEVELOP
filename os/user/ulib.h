#ifndef ULIB_H
#define ULIB_H

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
int uwaitpid_nohang(int pid);                     /* WNOHANG: 0 = kids alive */
long usbrk(long increment);                       /* old break, or -1 */
int upipe(int fds[2]);                            /* 0 ok */

void *umalloc(unsigned long n);                   /* NULL on failure */
void ufree(void *ptr);                            /* NULL is a no-op */

/* file syscalls (v0.2.0) */
int uopen(const char *path, int flags);
int uread(int fd, void *buf, int len);
int uwrite_fd(int fd, const void *buf, int len);
int uclose(int fd);
int ulseek(int fd, int off, int whence);
int ustat(const char *path, struct ustat *st);

/* anonymous mmap (v0.6.0): kernel picks the address, pages fault in zeroed */
void *ummap(unsigned long len);              /* NULL on failure */
int umunmap(void *va, unsigned long len);    /* whole mappings only */

#endif /* ULIB_H */
