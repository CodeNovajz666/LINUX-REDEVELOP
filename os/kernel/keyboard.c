#include "novix.h"
#include "keyboard.h"
#include "timer.h"
#include "sched.h"

#define KQ_SIZE 64
static char kqueue[KQ_SIZE];
static int q_head, q_count;
static spinlock_t kq_lock = SPINLOCK_INIT;   /* v0.7.0: IRQ4 (BSP) vs readers */

/* i8042 status port bits */
#define KST_OBF 0x01            /* output buffer full */
#define KST_IBF  0x02            /* input buffer full */
#define KST_AUX  0x20            /* byte came from the aux (mouse) port */

static void i8042_flush(void) {
    for (int i = 0; i < 64; i++) {
        if ((inb(0x64) & KST_OBF) == 0) return;
        (void)inb(0x60);
    }
}

static void i8042_wait_ibf(void) {
    for (int i = 0; i < 100000; i++)
        if ((inb(0x64) & KST_IBF) == 0) return;
}

void keyboard_init(void) {
    q_head = q_count = 0;
    outb(0x3F9, 0x01);      /* COM1 IER: enable received-data interrupt */

    /* i8042: QEMU's BIOS happens to leave the controller scanning with
     * set-1 translation; VMware (and real boards) may power up with the
     * keyboard clock gated and/or translation off, leaving port 0x60
     * silent forever.  Program the state we rely on instead of hoping
     * the firmware agrees with us. */
    i8042_flush();
    outb(0x64, 0xAD);       /* disable kbd port while reconfiguring */
    outb(0x64, 0xA7);       /* disable aux port */
    i8042_flush();
    i8042_wait_ibf();
    outb(0x64, 0x60);       /* write controller command byte */
    i8042_wait_ibf();
    outb(0x60, 0x65);       /* IRQ1 on, IRQ12 off, system flag, kbd clock
                               enabled, aux clock gated, set-1 translation on */
    outb(0x64, 0xAE);       /* re-enable kbd port */
    i8042_wait_ibf();
    outb(0x60, 0xF4);       /* kbd: enable scanning */
    i8042_flush();          /* drain the 0xFA ACK (and anything stale) */
}

static void kq_put(char c) {
    if (q_count == KQ_SIZE) return;
    kqueue[(q_head + q_count) % KQ_SIZE] = c;
    q_count++;
}

static int kq_get(void) {
    if (q_count == 0) return -1;
    char c = kqueue[q_head];
    q_head = (q_head + 1) % KQ_SIZE;
    q_count--;
    return (uint8_t)c;
}

/* scancode set 1 -> ascii (unshifted) */
static const char sc_to_ascii[128] = {
    0,   27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,  'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,  '\\','z','x','c','v','b','n','m',',','.','/', 0,
    '*', 0, ' ', 0,
};

static void kbd_push_scancode(uint8_t sc) {
    if (sc & 0x80) return;              /* break code */
    /* v1.1.1: arrows/Home/End deliver the SAME logical key codes the
     * serial escape parser below produces -- one grammar for readers */
    static const uint8_t nav[16] = {
        /*0x46*/ 0,    /*0x47*/ KEY_HOME, /*0x48*/ KEY_UP,   /*0x49*/ 0,
        /*0x4A*/ 0,    /*0x4B*/ KEY_LEFT, /*0x4C*/ 0,        /*0x4D*/ KEY_RIGHT,
        /*0x4E*/ 0,    /*0x4F*/ KEY_END,  /*0x50*/ KEY_DOWN, /*0x51*/ 0,
    };
    irq_flags_t f;
    spin_lock_irqsave(&kq_lock, &f);
    if (sc >= 0x46 && sc <= 0x51 && nav[sc - 0x46])
        kq_put(nav[sc - 0x46]);
    else if (sc != 0x01) {              /* bare Esc has no consumer: drop */
        char c = sc < 128 ? sc_to_ascii[sc] : 0;
        if (c) kq_put(c);
    }
    spin_unlock_irqrestore(&kq_lock, f);
}

void keyboard_irq(void) {
    kbd_push_scancode(inb(0x60));
}

/* PS/2 poll-drain: mirrors the serial LSR poll below.  Catches keys even
 * when IRQ1 routing is absent -- VMware's MP/IOAPIC tables differ from
 * QEMU's, so the keyboard_irq path cannot be assumed to fire. */
static void keyboard_ps2_drain(void) {
    for (int i = 0; i < 16; i++) {
        uint8_t st = inb(0x64);
        if (!(st & KST_OBF)) return;
        uint8_t d = inb(0x60);
        if (st & KST_AUX) continue;     /* mouse byte: not ours, drop */
        kbd_push_scancode(d);
    }
}

/* COM1 receive: terminal input under QEMU -serial stdio arrives here.
 * irq_save keeps the poll-drain and the IRQ4 handler from racing on the queue.
 *
 * v1.1.1: the ANSI escape parse lives HERE, once, for every reader. The
 * three-byte sequences terminals send for arrows/Home/End (CSI: ESC [ x,
 * SS3: ESC O H/F) collapse into the KEY_* logical bytes the PS/2 path also
 * produces; anything else after ESC is dropped (nothing consumes a lone
 * Escape). State survives across drains -- the bytes of one sequence may
 * arrive in different IRQs. */
static int esc_state;                   /* 0 none, 1 saw ESC, 2 saw CSI/SS3 */

static void serial_parse(char c) {
    if (esc_state == 0) {
        if (c == 0x1b) { esc_state = 1; return; }
        kq_put(c);
        return;
    }
    if (esc_state == 1) {
        if (c == '[' || c == 'O') esc_state = 2;
        else esc_state = 0;             /* lone ESC + other: not a sequence */
        return;
    }
    /* final byte of the sequence */
    esc_state = 0;
    switch (c) {
    case 'A': kq_put(KEY_UP); break;
    case 'B': kq_put(KEY_DOWN); break;
    case 'C': kq_put(KEY_RIGHT); break;
    case 'D': kq_put(KEY_LEFT); break;
    case 'H': kq_put(KEY_HOME); break;
    case 'F': kq_put(KEY_END); break;
    default: break;                     /* unknown sequence: swallowed */
    }
}

void keyboard_serial_drain(void) {
    irq_flags_t f; spin_lock_irqsave(&kq_lock, &f);
    while (inb(0x3FD) & 0x01) {         /* LSR: data ready */
        char c = (char)inb(0x3F8);
        if (c == '\r') c = '\n';
        if (c) serial_parse(c);
    }
    spin_unlock_irqrestore(&kq_lock, f);
}

void keyboard_serial_irq(void) {
    keyboard_serial_drain();
}

int keyboard_trychar(void) {
    keyboard_serial_drain();            /* poll RX: catches bytes even if an IRQ raced */
    keyboard_ps2_drain();               /* poll PS/2: catches keys even if IRQ1 is unrouted */
    irq_flags_t f; spin_lock_irqsave(&kq_lock, &f);
    int c = kq_get();
    spin_unlock_irqrestore(&kq_lock, f);
    return c;
}

int keyboard_getchar(void) {
    for (;;) {
        int c = keyboard_trychar();
        if (c >= 0) return c;
        task_sleep_until(current_task(), timer_ticks() + 1);   /* 10ms poll */
        kyield();
    }
}
