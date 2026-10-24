#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

void console_init(void);
void console_putchar(char c);
void console_puts(const char *s);
void console_write(const char *s, size_t len);
void console_clear(void);
void console_enter_panic(void);   /* unlock the console for the dump path */
/* v0.9.0: tty echo flag -- the console device read path echoes what it
 * hands out while this is on; TCSETS turns it off for password input. */
void console_set_echo(bool on);
bool console_echo_enabled(void);
void kprintf(const char *fmt, ...);
void kvprintf(const char *fmt, va_list ap);
/* v1.0.0: bounded kprintf-syntax formatting into out[0..cap), NUL ended;
 * returns the byte count that fit. Powers the shared debug reports. */
int ksnprintf(char *out, size_t cap, const char *fmt, ...);

#endif /* CONSOLE_H */
