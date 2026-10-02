#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdarg.h>

void console_init(void);
void console_putchar(char c);
void console_puts(const char *s);
void console_clear(void);
void console_enter_panic(void);   /* unlock the console for the dump path */
void kprintf(const char *fmt, ...);
void kvprintf(const char *fmt, va_list ap);

#endif /* CONSOLE_H */
