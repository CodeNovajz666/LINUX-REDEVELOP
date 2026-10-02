#include "novix.h"
#include "lib.h"
#include "gdt.h"
#include "percpu.h"

/* 5 descriptors (null, kcode64, kdata, ucode64, udata) + the 16-byte TSS64
 * descriptor pair -- one private copy per CPU (v0.7.0), so RSP0 follows
 * whatever that CPU is about to run. */

#define GDT_ENTRIES 7

static void gdt_build(struct cpu *c) {
    memset(c->gdt, 0, sizeof(c->gdt));
    memset(&c->tss, 0, sizeof(c->tss));
    c->tss.iomap_base = sizeof(struct tss64);
    c->tss.rsp0 = 0;                      /* task.c owns RSP0 from milestone D */

    c->gdt[1] = 0x00AF9A000000FFFFull;    /* 0x08 kernel code64 (L=1) */
    c->gdt[2] = 0x00CF92000000FFFFull;    /* 0x10 kernel data */
    c->gdt[3] = 0x00AFFA000000FFFFull;    /* 0x1B user code64 (rpl3) */
    c->gdt[4] = 0x00CFF2000000FFFFull;    /* 0x23 user data (rpl3) */

    /* TSS64 descriptor: two consecutive GDT slots */
    uint64_t base = (uint64_t)(uintptr_t)&c->tss;
    uint32_t limit = sizeof(struct tss64) - 1;
    c->gdt[5] = ((uint64_t)(limit & 0xFFFF))
              | ((base & 0xFFFFFF) << 16)
              | ((uint64_t)0x89 << 40)           /* present, 64-bit TSS */
              | ((uint64_t)((limit >> 16) & 0x0F) << 48)
              | (((base >> 24) & 0xFF) << 56);
    c->gdt[6] = base >> 32;
}

extern void gdt_flush(uint64_t *gdt, int entries, int tss_sel);   /* gdt_flush.S */

void gdt_init_cpu(struct cpu *c) {
    gdt_build(c);
    gdt_flush(c->gdt, GDT_ENTRIES, SEL_TSS);
}

void gdt_init(void) {
    /* cpus[0] exists and has its GS base wired by percpu_init() */
    gdt_init_cpu(&cpus[0]);
}

void tss_set_kstack(uint64_t rsp0) {
    this_cpu()->tss.rsp0 = rsp0;
}
