#include "novix.h"
#include "keyboard.h"
#include "timer.h"
#include "sched.h"

#define KQ_SIZE 64
static char kqueue[KQ_SIZE];
static int q_head, q_count;
static spinlock_t kq_lock = SPINLOCK_INIT;   /* v0.7.0: IRQ4 (BSP) vs readers */

void keyboard_init(void) {
    q_head = q_count = 0;
    outb(0x3F9, 0x01);      /* COM1 IER: enable received-data interrupt */
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

void keyboard_irq(void) {
    uint8_t sc = inb(0x60);
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
