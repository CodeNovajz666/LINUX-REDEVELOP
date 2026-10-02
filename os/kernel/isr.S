/*
 * Novix interrupt stubs (x86_64).
 *
 * Every interrupt/exception/syscall enters through a generated stub that
 * normalizes the stack to one uniform frame (struct pt_regs):
 *
 *   stub pushes:  [err(0 if cpu didn't)] [int_no]
 *   common:       [r15..rax]  ... then calls C.
 *
 * Segments are not saved: long mode ignores base/limit for ds/es/fs/gs,
 * and the kernel never loads a non-flat value into them.
 *
 * isr_handler() returns the stack pointer of the frame to resume
 * (possibly a *different task's* frame) -- a single iretq does both
 * "return from interrupt" and "context switch" (design carried over
 * from i386; the scheduler returns with milestone D).
 */
.text

.macro STUB_NOERR n
.global isr_stub_\n
.type isr_stub_\n, @function
isr_stub_\n:
    pushq $0
    pushq $\n
    jmp isr_common
.endm

.macro STUB_ERR n
.global isr_stub_\n
.type isr_stub_\n, @function
isr_stub_\n:
    pushq $\n
    jmp isr_common
.endm

/* CPU exceptions 0..31 */
STUB_NOERR 0
STUB_NOERR 1
STUB_NOERR 2
STUB_NOERR 3
STUB_NOERR 4
STUB_NOERR 5
STUB_NOERR 6
STUB_NOERR 7
STUB_ERR   8
STUB_NOERR 9
STUB_ERR   10
STUB_ERR   11
STUB_ERR   12
STUB_ERR   13
STUB_ERR   14
STUB_NOERR 15
STUB_NOERR 16
STUB_ERR   17
STUB_NOERR 18
STUB_NOERR 19
STUB_NOERR 20
STUB_NOERR 21
STUB_NOERR 22
STUB_NOERR 23
STUB_NOERR 24
STUB_NOERR 25
STUB_NOERR 26
STUB_NOERR 27
STUB_NOERR 28
STUB_NOERR 29
STUB_NOERR 30
STUB_NOERR 31

/* hardware IRQs 0..15 mapped to vectors 32..47 */
STUB_NOERR 32
STUB_NOERR 33
STUB_NOERR 34
STUB_NOERR 35
STUB_NOERR 36
STUB_NOERR 37
STUB_NOERR 38
STUB_NOERR 39
STUB_NOERR 40
STUB_NOERR 41
STUB_NOERR 42
STUB_NOERR 43
STUB_NOERR 44
STUB_NOERR 45
STUB_NOERR 46
STUB_NOERR 47

/* v0.7.0 APIC vectors */
STUB_NOERR 48                         /* 0x30 LAPIC timer */
STUB_NOERR 80                         /* 0x50 RESCHED IPI */
STUB_NOERR 81                         /* 0x51 TLB-shootdown IPI */
STUB_NOERR 254                        /* 0xFE LAPIC error */
STUB_NOERR 255                        /* 0xFF LAPIC spurious (no EOI) */

/* int 0x80 syscall (DPL 3) */
STUB_NOERR 128

isr_common:
    /* push r15 first so rax lands at the lowest address (struct order) */
    pushq %r15
    pushq %r14
    pushq %r13
    pushq %r12
    pushq %r11
    pushq %r10
    pushq %r9
    pushq %r8
    pushq %rdi
    pushq %rsi
    pushq %rbp
    pushq %rbx
    pushq %rdx
    pushq %rcx
    pushq %rax

    movq %rsp, %rdi          /* struct pt_regs * */
    call isr_handler         /* returns new frame rsp in %rax */
    movq %rax, %rsp          /* switch stack: may resume a different task */

    popq %rax
    popq %rcx
    popq %rdx
    popq %rbx
    popq %rbp
    popq %rsi
    popq %rdi
    popq %r8
    popq %r9
    popq %r10
    popq %r11
    popq %r12
    popq %r13
    popq %r14
    popq %r15
    addq $16, %rsp           /* int_no, err_code */
    iretq                    /* long mode: always pops rip,cs,rflags,rsp,ss */

/* table of stub addresses for idt_init (C) */
.section .rodata
.global isr_stub_table
isr_stub_table:
    .quad isr_stub_0,  isr_stub_1,  isr_stub_2,  isr_stub_3
    .quad isr_stub_4,  isr_stub_5,  isr_stub_6,  isr_stub_7
    .quad isr_stub_8,  isr_stub_9,  isr_stub_10, isr_stub_11
    .quad isr_stub_12, isr_stub_13, isr_stub_14, isr_stub_15
    .quad isr_stub_16, isr_stub_17, isr_stub_18, isr_stub_19
    .quad isr_stub_20, isr_stub_21, isr_stub_22, isr_stub_23
    .quad isr_stub_24, isr_stub_25, isr_stub_26, isr_stub_27
    .quad isr_stub_28, isr_stub_29, isr_stub_30, isr_stub_31
    .quad isr_stub_32,  isr_stub_33,  isr_stub_34,  isr_stub_35
    .quad isr_stub_36,  isr_stub_37,  isr_stub_38,  isr_stub_39
    .quad isr_stub_40,  isr_stub_41,  isr_stub_42,  isr_stub_43
    .quad isr_stub_44,  isr_stub_45,  isr_stub_46,  isr_stub_47
    .quad isr_stub_48                         /* 0x30 LAPIC timer */
    .rept (80 - 49)
    .quad 0
    .endr
    .quad isr_stub_80,  isr_stub_81           /* 0x50/0x51 IPIs */
    .rept (128 - 82)
    .quad 0
    .endr
    .quad isr_stub_128
    .rept (254 - 129)
    .quad 0
    .endr
    .quad isr_stub_254, isr_stub_255          /* 0xFE/0xFF LAPIC */
