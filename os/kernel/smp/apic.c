#include "novix.h"
#include "apic.h"
#include "smp.h"
#include "pic.h"
#include "lib.h"
#include "spinlock.h"

/*
 * LAPIC (0xFEE00000) + IOAPIC (0xFEC00000), both identity-mapped by
 * paging_init (supervisor 2 MiB pages -- the same trick as the kernel low
 * map, copied into every user PDPT so the registers are reachable from
 * any CR3). QEMU maps APIC MMIO uncacheably in hardware and TCG does not
 * emulate cache coherence, so a WB mapping is functionally fine here.
 */

#define LAPIC_BASE_PA   0xFEE00000u
#define IOAPIC_BASE_PA  0xFEC00000u
#define MSR_IA32_APICBASE 0x1B

#define LAPIC_ID        0x020
#define LAPIC_EOI       0x0B0
#define LAPIC_LDR       0x0D0
#define LAPIC_DFR       0x0E0
#define LAPIC_SIVR      0x0F0
#define LAPIC_ISR0      0x100
#define LAPIC_ESR       0x280
#define LAPIC_ICR_LOW   0x300
#define LAPIC_ICR_HIGH  0x310
#define LAPIC_LVT_TIMER 0x320
#define LAPIC_LVT_LINT0 0x350
#define LAPIC_LVT_LINT1 0x360
#define LAPIC_LVT_ERR   0x370
#define LAPIC_TIMER_ICR 0x380   /* initial count */
#define LAPIC_TIMER_CCR 0x390   /* current count */
#define LAPIC_TIMER_DIV 0x3E0

#define IOAPIC_REGSEL   0x00
#define IOAPIC_WIN      0x10
#define IOAPIC_MAX_IRQ  23

static volatile uint32_t *const lapic = (volatile uint32_t *)LAPIC_BASE_PA;
static volatile uint32_t *const ioapic = (volatile uint32_t *)IOAPIC_BASE_PA;

/* set by mp.c */
extern bool mp_imcr_present;

static inline uint32_t lapic_read(uint32_t reg) {
    return lapic[reg >> 2];
}
static inline void lapic_write(uint32_t reg, uint32_t v) {
    lapic[reg >> 2] = v;
}

static uint32_t ioapic_read(uint8_t reg) {
    ioapic[IOAPIC_REGSEL >> 2] = reg;
    return ioapic[IOAPIC_WIN >> 2];
}
static void ioapic_write(uint8_t reg, uint32_t v) {
    ioapic[IOAPIC_REGSEL >> 2] = reg;
    ioapic[IOAPIC_WIN >> 2] = v;
}

void apic_eoi(void) { lapic_write(LAPIC_EOI, 0); }

uint8_t apic_lapic_id(void) { return (uint8_t)(lapic_read(LAPIC_ID) >> 24); }

/* ---- calibration: PIT channel 2 one-shot, no interrupts needed ---- */

void apic_udelay(uint32_t us) {
    uint8_t old = inb(0x61);
    outb(0x61, (uint8_t)((old & ~0x02) | 0x01));  /* gate on, speaker off */
    outb(0x43, 0xB0);                             /* ch2, lo/hi, mode 0 */
    uint32_t count = (uint32_t)((1193182ull * us) / 1000000u);
    if (count < 2) count = 2;
    if (count > 65535) count = 65535;
    outb(0x42, count & 0xFF);
    outb(0x42, (count >> 8) & 0xFF);
    while (!(inb(0x61) & 0x20)) cpu_pause();      /* OUT pin high = done */
    outb(0x61, old);
}

static void calibrate_timer(void) {
    /* one-shot, masked LVT, divide by 16; measure the countdown over 50 ms */
    lapic_write(LAPIC_TIMER_DIV, 0x3);
    lapic_write(LAPIC_LVT_TIMER, 0x10000);       /* masked */
    lapic_write(LAPIC_TIMER_ICR, 0xFFFFFFFFu);
    apic_udelay(1000);                           /* let it start counting */
    uint32_t before = lapic_read(LAPIC_TIMER_CCR);
    apic_udelay(50000);                          /* 50 ms */
    uint32_t after = lapic_read(LAPIC_TIMER_CCR);
    uint32_t elapsed = before - after;           /* counts down */
    /* ticks per 10 ms interrupt period (TICK_HZ = 100) */
    uint32_t per_tick = elapsed / (50000u / (1000000u / TICK_HZ)) / 2u;
    if (per_tick < 16) per_tick = 16;            /* keep it sane */
    cpus[0].lapic_ticks = per_tick;
}

/* ---- IOAPIC routing ---- */

static void ioapic_set_irq(uint8_t irq, uint8_t vector, uint8_t dest_apic, bool masked) {
    /* entry = (dest << 24) : (mask<<16 | trigger<<15 | polarity<<13 |
     *          destmode<<11 | delivmode<<8 | vector) -- physical, edge, fixed */
    uint32_t high = (uint32_t)dest_apic << 24;
    uint32_t low = vector | ((masked ? 1u : 0u) << 16);
    ioapic_write((uint8_t)(0x10 + irq * 2), low);
    ioapic_write((uint8_t)(0x11 + irq * 2), high);
}

/* ---- IPIs ---- */

static void lapic_ipi_wait(void) {
    while (lapic_read(LAPIC_ICR_LOW) & (1u << 12)) cpu_pause();  /* delivery busy */
}

static void lapic_ipi(uint8_t dest_apic_id, uint32_t low) {
    lapic_ipi_wait();
    lapic_write(LAPIC_ICR_HIGH, (uint32_t)dest_apic_id << 24);
    lapic_write(LAPIC_ICR_LOW, low);
    lapic_ipi_wait();
}

void apic_send_init(uint8_t dest) {
    lapic_ipi(dest, 0x00000500);                 /* INIT, assert, physical */
}
void apic_send_startup(uint8_t dest, uint8_t vector) {
    lapic_ipi(dest, 0x00000600u | vector);       /* STARTUP */
}
void apic_ipi_broadcast(uint8_t vector) {
    lapic_ipi_wait();
    lapic_write(LAPIC_ICR_LOW, vector | (4u << 18));   /* all excl. self */
    lapic_ipi_wait();
}
void apic_ipi_single(uint8_t dest, uint8_t vector) {
    lapic_ipi(dest, vector);
}

/* ---- per-CPU init ---- */

void apic_percpu_init(struct cpu *c) {
    /* software enable + spurious vector (0xFF, no EOI on spurious) */
    lapic_write(LAPIC_SIVR, 0xFF | (1u << 8));
    lapic_write(LAPIC_LVT_LINT0, 0x10000);       /* PIC is masked; keep quiet */
    lapic_write(LAPIC_LVT_LINT1, 0x10000);
    lapic_write(LAPIC_LVT_ERR, 0xFE);            /* vector 0xFE */
    lapic_write(LAPIC_ESR, 0);
    lapic_read(LAPIC_ESR);

    /* periodic LAPIC timer at TICK_HZ, vector VEC_LAPIC_TIMER -- but MASKED:
     * the CPU comes up parked and only ticks after apic_timer_start(), so a
     * booting AP can not preempt into the scheduler before smp_release() */
    lapic_write(LAPIC_TIMER_DIV, 0x3);           /* divide by 16 */
    lapic_write(LAPIC_LVT_TIMER, VEC_LAPIC_TIMER | (1u << 17) | (1u << 16));
    lapic_write(LAPIC_TIMER_ICR, c->lapic_ticks ? c->lapic_ticks : 0x100000);
}

/* un-park: the timer starts firing VEC_LAPIC_TIMER at TICK_HZ from here on */
void apic_timer_start(struct cpu *c) {
    lapic_write(LAPIC_TIMER_DIV, 0x3);
    lapic_write(LAPIC_LVT_TIMER, VEC_LAPIC_TIMER | (1u << 17));  /* periodic */
    lapic_write(LAPIC_TIMER_ICR, c->lapic_ticks ? c->lapic_ticks : 0x100000);
}

/* ---- BSP boot-time init ---- */

void apic_init(void) {
    /* global enable flag in the APIC base MSR (QEMU keeps the default
     * 0xFEE00000 base -- anything else is a layout we did not map) */
    uint64_t base = rdmsr(MSR_IA32_APICBASE);
    if ((base & 0xFFFFF000u) != LAPIC_BASE_PA)
        panic("apic: unexpected LAPIC base %llx", (unsigned long long)base);
    wrmsr(MSR_IA32_APICBASE, base | (1u << 11) | (1u << 8));  /* enable | BSP */

    lapic_write(LAPIC_SIVR, 0xFF | (1u << 8));   /* needed before CCR counts */
    calibrate_timer();

    /* park the 8259 forever: mask every line, and route the PIC output
     * through the APIC when the IMCR exists (MP spec PIC mode) */
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
    if (mp_imcr_present) {
        outb(0x22, 0x70);
        outb(0x23, 0x01);
    }

    /* IOAPIC: mask all, then route the two legacy lines the kernel uses to
     * the BSP. IRQ0 stays masked -- the LAPIC timer is the tick source on
     * every CPU now (the PIT keeps running into a dead line, harmless). */
    for (int irq = 0; irq <= IOAPIC_MAX_IRQ; irq++)
        ioapic_set_irq((uint8_t)irq, 0x20 + (uint8_t)irq, (uint8_t)cpus[0].apic_id, true);
    ioapic_set_irq(IRQ_KEYBOARD, 32 + IRQ_KEYBOARD, (uint8_t)cpus[0].apic_id, false);
    ioapic_set_irq(IRQ_SERIAL,   32 + IRQ_SERIAL,   (uint8_t)cpus[0].apic_id, false);

    /* the BSP itself is just another CPU from here on -- and the only one
     * whose timer runs before smp_release() */
    apic_percpu_init(&cpus[0]);
    apic_timer_start(&cpus[0]);
}
