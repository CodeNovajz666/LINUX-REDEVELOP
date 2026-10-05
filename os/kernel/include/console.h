#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdarg.h>
#include <stdbool.h>

void console_init(void);
void console_putchar(char c);
void console_puts(const char *s);
void console_clear(void);
void console_enter_panic(void);   /* unlock the console for the dump path */
/* v0.9.0: tty echo flag -- the console device read path echoes what it
 * hands out while this is on; TCSETS turns it off for password input. */
void console_set_echo(bool on);
bool console_echo_enabled(void);
void kprintf(const char *fmt, ...);
void kvprintf(const char *fmt, va_list ap);

#endif /* CONSOLE_H */
