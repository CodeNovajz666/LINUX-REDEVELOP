#include "novix.h"
#include "isr.h"
#include "isr_stubs.h"

struct idt_gate {
    uint16_t offset_lo;     /* handler bits 0..15 */
    uint16_t selector;
    uint8_t  ist;           /* IST index, 0 = none */
    uint8_t  type_attr;     /* 0x8E = 64-bit interrupt gate, DPL 0 */
    uint16_t offset_mid;    /* handler bits 16..31 */
    uint32_t offset_hi;     /* handler bits 32..63 */
    uint32_t zero;
} __attribute__((packed));

static struct idt_gate idt[256];
static struct { uint16_t limit; uint64_t base; } __attribute__((packed)) idtp;

static void idt_set(int n, void (*handler)(void), uint8_t dpl) {
    uint64_t off = (uint64_t)(uintptr_t)handler;
    idt[n].offset_lo = off & 0xFFFF;
    idt[n].offset_mid = (off >> 16) & 0xFFFF;
    idt[n].offset_hi = (uint32_t)(off >> 32);
    idt[n].selector = SEL_KCODE;
    idt[n].ist = 0;
    idt[n].zero = 0;
    idt[n].type_attr = 0x8E | (dpl << 5);
}

void idt_load(void) { __asm__ volatile("lidt %0" : : "m"(idtp)); }

void idt_init(void) {
    memset(idt, 0, sizeof(idt));
    for (int n = 0; n < 256; n++) {
        if (isr_stub_table[n])
            idt_set(n, isr_stub_table[n], n == 0x80 ? 3 : 0);
    }
    idtp.limit = sizeof(idt) - 1;
    idtp.base = (uint64_t)(uintptr_t)idt;
    idt_load();
}
