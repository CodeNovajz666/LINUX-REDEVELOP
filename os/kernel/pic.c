#include "novix.h"
#include "pic.h"

/* 8259 PIC: remap IRQs to vectors 32..47 */
#define PIC1_CMD 0x20
#define PIC1_DAT 0x21
#define PIC2_CMD 0xA0
#define PIC2_DAT 0xA1
#define ICW1_INIT 0x11

void pic_init(void) {
    outb(PIC1_CMD, ICW1_INIT); io_wait();
    outb(PIC2_CMD, ICW1_INIT); io_wait();
    outb(PIC1_DAT, 32);        io_wait();   /* offset 32 */
    outb(PIC2_DAT, 40);        io_wait();   /* offset 40 */
    outb(PIC1_DAT, 0x04);      io_wait();   /* cascade on IRQ2 */
    outb(PIC2_DAT, 0x02);      io_wait();
    outb(PIC1_DAT, 0x01);      io_wait();   /* 8086 mode */
    outb(PIC2_DAT, 0x01);      io_wait();
    outb(PIC1_DAT, 0x00);                   /* unmask all */
    outb(PIC2_DAT, 0x00);
}

void pic_eoi(uint32_t int_no) {
    if (int_no >= 40) outb(PIC2_CMD, 0x20);
    outb(PIC1_CMD, 0x20);
}

void pic_mask_irq(uint32_t irq, int masked) {
    uint16_t port = irq < 8 ? PIC1_DAT : PIC2_DAT;
    uint8_t bit = 1u << (irq & 7);
    uint8_t v = inb(port);
    if (masked) v |= bit; else v &= ~bit;
    outb(port, v);
}
