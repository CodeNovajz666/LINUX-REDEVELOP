#ifndef KLOG_H
#define KLOG_H

/* Leveled kernel log: "[  12.34] LEVEL: message" over the same console
 * channel as kprintf. Levels are for CI greppability, not filtering. */

void klog_err(const char *fmt, ...);
void klog_warn(const char *fmt, ...);
void klog_info(const char *fmt, ...);

#endif /* KLOG_H */
