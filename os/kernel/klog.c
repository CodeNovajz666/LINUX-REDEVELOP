#include "novix.h"
#include "console.h"
#include "timer.h"
#include "klog.h"
#include <stdarg.h>

static void klog_vfmt(const char *level, const char *fmt, va_list ap) {
    uint64_t ms = timer_uptime_ms();
    kprintf("[%5llu.%02llu] %s: ",
            (unsigned long long)(ms / 1000),
            (unsigned long long)((ms % 1000) / 10), level);
    kvprintf(fmt, ap);
    console_putchar('\n');
}

void klog_err(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    klog_vfmt("ERR", fmt, ap);
    va_end(ap);
}

void klog_warn(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    klog_vfmt("WRN", fmt, ap);
    va_end(ap);
}

void klog_info(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    klog_vfmt("INF", fmt, ap);
    va_end(ap);
}
