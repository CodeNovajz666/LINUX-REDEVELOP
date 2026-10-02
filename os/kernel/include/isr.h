#ifndef ISR_H
#define ISR_H

#include <stdint.h>
#include "novix.h"

/*
 * Unified interrupt frame, memory layout (low -> high address),
 * matching the push order of the stubs in isr.S:
 *
 *   [rax rcx rdx rbx rbp rsi rdi r8..r15] [int_no err_code]
 *   [rip cs rflags rsp ss]
 *
 * Long mode pushes rip/cs/rflags/rsp/ss on EVERY interrupt (rsp/ss
 * unconditionally, unlike i386) -- the uniform frame holds for both
 * ring0 and ring3 origins.
 */
struct pt_regs {
    uint64_t rax, rcx, rdx, rbx, rbp, rsi, rdi;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t int_no, err_code;
    uint64_t rip, cs, rflags, rsp, ss;
};

#define EFL_IF 0x0200u

void idt_init(void);
void idt_load(void);                          /* per-CPU: APs reload the shared IDT */
uint64_t isr_handler(struct pt_regs *r);   /* defined in isr.c, called from isr.S */
void dump_regs(const char *why, struct pt_regs *r);

#endif /* ISR_H */
