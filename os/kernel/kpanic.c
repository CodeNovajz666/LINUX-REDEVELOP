#include "novix.h"
#include "console.h"
#include "lib.h"
#include <stdarg.h>

void qemu_exit(int code) {
    /* isa-debug-exit device: QEMU exits with (val << 1) | 1, so write the
     * raw code: 0 -> exit 1 (pass), 1 -> exit 3 (fail), 2 -> exit 5 (panic) */
    outw(0x501, (uint16_t)code);
}

void panic(const char *fmt, ...) {
    va_list ap;
    console_enter_panic();          /* v0.7.0: print past a dead console_lock holder */
    va_start(ap, fmt);
    kprintf("\nKERNEL PANIC: ");
    kvprintf(fmt, ap);
    kprintf("\nSystem halted.\n");
    va_end(ap);
    qemu_exit(2);
    __asm__ volatile("cli");
    for (;;) __asm__ volatile("hlt");
}

/* entry point for Rust panics (paging.rs panic_handler) */
void rust_panic_msg(const char *msg) {
    panic("%s", msg);
}
