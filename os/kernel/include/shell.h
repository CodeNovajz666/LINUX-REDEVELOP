#ifndef SHELL_H
#define SHELL_H

void shell_run(void);          /* never returns */

/* v1.0.0: streaming sink shared by the debug reports. The kernel debug
 * shell prints through shell_emit_console; the sys_ps/sys_cpus/
 * sys_meminfo syscalls append the very same lines into a user buffer. */
typedef void (*shell_emit_t)(const char *s, int len, void *ctx);
void shell_emit_console(const char *s, int len, void *ctx);

/* v1.0.0: the `mem` report shared by the kernel shell and sys_meminfo */
void shell_report_mem(shell_emit_t emit, void *ctx, int verbose);

/* v1.1.0: the `ls` report shared by the kernel shell and sys_getdents --
 * streams the full listing (per-entry type lines + the "N entries" tail).
 * The caller decides who may see it: sys_getdents checks VFS_PERM_R on the
 * directory at the syscall boundary, the kernel shell (root) does not. */
void shell_report_dir(shell_emit_t emit, void *ctx, const char *path);

/* v1.1.0: the `date` report shared by the kernel shell and sys_datetime.
 * Returns 0, or -1 when the RTC is not sane (nothing is emitted then). */
int shell_report_date(shell_emit_t emit, void *ctx);

#endif /* SHELL_H */
