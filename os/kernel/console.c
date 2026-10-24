#include "novix.h"
#include "console.h"
#include "lib.h"
#include "spinlock.h"

/* ---- VGA text mode ---- */
#define VGA_MEM   ((volatile uint16_t *)0xB8000)
#define VGA_COLS  80
#define VGA_ROWS  25
#define VGA_ATTR  0x0F00u   /* white on black */

static int cur_row, cur_col;

/* v0.7.0: character-granularity output lock. Without it two CPUs writing
 * "concurrently" interleave at the cursor-update level and both the VGA
 * cursor and scroll state go insane. Leaf lock: panic() prints through
 * here, so console code itself must never lock anything else. */
static spinlock_t console_lock = SPINLOCK_INIT;

/* v0.7.0: panic mode. A CPU that faults INSIDE a locked putchar would
 * deadlock its own dump at the very first kprintf (and every other CPU
 * with it) -- the classic "hang instead of panic". Once the machine is
 * dumping, ownership of the console is void: steal the lock, never take
 * it again, and accept interleaved output (better garbled than gone). */
static volatile bool console_panicking;

/* v0.9.0: tty echo. On by default; the console device read echoes printable
 * characters and newline while enabled (a one-flag line discipline). */
static volatile bool console_echo = true;

void console_set_echo(bool on) { console_echo = on; }
bool console_echo_enabled(void) { return console_echo; }

void console_enter_panic(void) {
    console_panicking = true;
    console_lock.locked = 0;            /* force-release any dead holder */
}

/* ---- serial COM1 ---- */
#define COM1 0x3F8

static void serial_init(void) {
    outb(COM1 + 1, 0x00);    /* disable interrupts */
    outb(COM1 + 3, 0x80);    /* DLAB on */
    outb(COM1 + 0, 0x01);    /* divisor 1 -> 115200 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);    /* 8N1 */
    outb(COM1 + 2, 0xC7);    /* FIFO on, clear, 14-byte RX trigger */
    outb(COM1 + 4, 0x0B);    /* RTS/DSR */
}

static void serial_putc(char c) {
    if (c == '\n') serial_putc('\r');
    for (int i = 0; i < 100000; i++) {
        if (inb(COM1 + 5) & 0x20) break;   /* transmit holding empty */
    }
    outb(COM1 + 0, (uint8_t)c);
}

static void vga_scroll(void) {
    for (int r = 1; r < VGA_ROWS; r++)
        for (int c = 0; c < VGA_COLS; c++)
            VGA_MEM[(r - 1) * VGA_COLS + c] = VGA_MEM[r * VGA_COLS + c];
    for (int c = 0; c < VGA_COLS; c++)
        VGA_MEM[(VGA_ROWS - 1) * VGA_COLS + c] = VGA_ATTR | ' ';
}

static void vga_update_cursor(void) {
    uint16_t pos = cur_row * VGA_COLS + cur_col;
    outb(0x3D4, 0x0F);
    outb(0x3D5, pos & 0xFF);
    outb(0x3D4, 0x0E);
    outb(0x3D5, (pos >> 8) & 0xFF);
}

void console_init(void) {
    serial_init();
    cur_row = cur_col = 0;
    console_clear();
}

void console_clear(void) {
    irq_flags_t f;
    spin_lock_irqsave(&console_lock, &f);
    for (int i = 0; i < VGA_COLS * VGA_ROWS; i++)
        VGA_MEM[i] = VGA_ATTR | ' ';
    cur_row = cur_col = 0;
    vga_update_cursor();
    spin_unlock_irqrestore(&console_lock, f);
}

/* Emit [s, s+n) in ONE locked pass: a whole kprintf message lands
 * contiguously in the serial log even when the caller is preempted or a
 * second CPU prints concurrently. */
void console_write(const char *s, size_t n) {
    irq_flags_t f;
    if (!console_panicking) spin_lock_irqsave(&console_lock, &f);
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        serial_putc(c);
        if (c == '\n') {
            cur_col = 0;
            if (++cur_row >= VGA_ROWS) { vga_scroll(); cur_row = VGA_ROWS - 1; }
        } else if (c == '\r') {
            cur_col = 0;
        } else if (c == '\t') {
            cur_col = (cur_col + 8) & ~7;
            if (cur_col >= VGA_COLS) { cur_col = 0; if (++cur_row >= VGA_ROWS) { vga_scroll(); cur_row = VGA_ROWS - 1; } }
        } else if (c == '\b') {
            if (cur_col > 0) cur_col--;           /* erase sequences: "\b \b" */
        } else {
            if (cur_col >= VGA_COLS) { cur_col = 0; if (++cur_row >= VGA_ROWS) { vga_scroll(); cur_row = VGA_ROWS - 1; } }
            VGA_MEM[cur_row * VGA_COLS + cur_col] = VGA_ATTR | (uint8_t)c;
            cur_col++;
        }
    }
    vga_update_cursor();
    if (!console_panicking) spin_unlock_irqrestore(&console_lock, f);
}

void console_putchar(char c) { console_write(&c, 1); }

void console_puts(const char *s) { console_write(s, strlen(s)); }

/* ---- minimal printf ----
 * kvprintf formats one message into a stack buffer and emits it in a
 * single locked pass. Char-granularity locking let a preempting task (or,
 * v0.7.0, a second CPU) splice its own output into the middle of a line,
 * which garbles the serial log the test harness greps. An oversize
 * message flushes and continues -- two passes, never a truncation. */

struct putbuf {
    char b[256];
    size_t n;
    /* string-sink mode (ksnprintf): append into out[0..cap); n keeps
     * counting everything emitted, bytes past cap are dropped */
    char *out;
    size_t cap;
};

static void pb_flush(struct putbuf *pb) {
    if (pb->out) return;                        /* string sink: no flushing */
    console_write(pb->b, pb->n);
    pb->n = 0;
}

static void pb_putc(struct putbuf *pb, char c) {
    if (pb->out) {
        if (pb->n < pb->cap) pb->out[pb->n] = c;
        pb->n++;
        return;
    }
    if (pb->n == sizeof(pb->b)) pb_flush(pb);
    pb->b[pb->n++] = c;
}

static void print_uint(struct putbuf *pb, unsigned long v, unsigned base, int width, char pad, bool left) {
    char buf[32];
    const char *digits = "0123456789abcdef";
    int i = 0;
    if (v == 0) buf[i++] = '0';
    while (v) { buf[i++] = digits[v % base]; v /= base; }
    int len = i;
    if (!left) for (int p = len; p < width; p++) pb_putc(pb, pad);
    while (i > 0) pb_putc(pb, buf[--i]);
    if (left) for (int p = len; p < width; p++) pb_putc(pb, ' ');
}

static void print_int(struct putbuf *pb, long v, unsigned base, int width, char pad, bool left) {
    if (v < 0 && base == 10) {
        pb_putc(pb, '-');
        print_uint(pb, (unsigned long)(-v), base, width ? width - 1 : 0, pad, left);
    } else {
        print_uint(pb, (unsigned long)v, base, width, pad, left);
    }
}

static void print_str(struct putbuf *pb, const char *s, int width, bool left) {
    if (!s) s = "(null)";
    int len = (int)strlen(s);
    if (!left) for (int p = len; p < width; p++) pb_putc(pb, ' ');
    for (const char *q = s; *q; q++) pb_putc(pb, *q);
    if (left) for (int p = len; p < width; p++) pb_putc(pb, ' ');
}

/* the shared format engine: console sink (kprintf) or string sink
 * (ksnprintf) -- both go through the same pb_putc */
static void kfmt(struct putbuf *pb, const char *fmt, va_list ap) {
    for (; *fmt; fmt++) {
        if (*fmt != '%') { pb_putc(pb, *fmt); continue; }
        fmt++;
        bool left = false;
        int width = 0;
        char pad = ' ';
        for (;; fmt++) {
            if (*fmt == '-') { left = true; continue; }
            if (*fmt == '0') { pad = '0'; continue; }
            break;
        }
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }
        int longs = 0;
        while (*fmt == 'l') { longs++; fmt++; }
        switch (*fmt) {
        case 'd': case 'i':
            print_int(pb, longs ? va_arg(ap, long) : va_arg(ap, int), 10, width, pad, left);
            break;
        case 'u':
            print_uint(pb, longs ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int), 10, width, pad, left);
            break;
        case 'x':
            print_uint(pb, longs ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int), 16, width, pad, left);
            break;
        case 'p': pb_putc(pb, '0'); pb_putc(pb, 'x');
            print_uint(pb, (unsigned long)va_arg(ap, void *), 16, 8, '0', false); break;
        case 's': print_str(pb, va_arg(ap, const char *), width, left); break;
        case 'c': pb_putc(pb, (char)va_arg(ap, int)); break;
        case '%': pb_putc(pb, '%'); break;
        case '\0': pb_flush(pb); return;
        default: pb_putc(pb, '%'); pb_putc(pb, *fmt); break;
        }
    }
    pb_flush(pb);
}

void kvprintf(const char *fmt, va_list ap) {
    struct putbuf pb = { .n = 0, .out = NULL, .cap = 0 };
    kfmt(&pb, fmt, ap);
}

void kprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
}

/* v1.0.0: bounded formatting for the shared debug reports -- the same
 * %-syntax as kprintf, appended truncating into out[0..cap) with a NUL.
 * Returns the byte count that fit (NUL excluded). */
int ksnprintf(char *out, size_t cap, const char *fmt, ...) {
    if (!out || cap == 0) return 0;
    struct putbuf pb = { .n = 0, .out = out, .cap = cap - 1 };
    va_list ap;
    va_start(ap, fmt);
    kfmt(&pb, fmt, ap);
    va_end(ap);
    size_t n = pb.n < pb.cap ? pb.n : pb.cap;
    out[n] = 0;
    return (int)n;
}
