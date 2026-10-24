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
    /* up/down arrows become the ESC sequences serial terminals already
     * send, so the shell parses one grammar on both input paths */
    irq_flags_t f;
    spin_lock_irqsave(&kq_lock, &f);
    if (sc == 0x48) { kq_put(0x1b); kq_put('['); kq_put('A'); spin_unlock_irqrestore(&kq_lock, f); return; }
    if (sc == 0x50) { kq_put(0x1b); kq_put('['); kq_put('B'); spin_unlock_irqrestore(&kq_lock, f); return; }
    char c = sc < 128 ? sc_to_ascii[sc] : 0;
    if (c) kq_put(c);
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
 * irq_save keeps the poll-drain and the IRQ4 handler from racing on the queue. */
void keyboard_serial_drain(void) {
    irq_flags_t f; spin_lock_irqsave(&kq_lock, &f);
    while (inb(0x3FD) & 0x01) {         /* LSR: data ready */
        char c = (char)inb(0x3F8);
        if (c == '\r') c = '\n';
        if (c) kq_put(c);
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
