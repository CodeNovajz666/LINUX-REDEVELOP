/*
 * AP startup trampoline (v0.7.0).
 *
 * The BSP copies this blob to physical 0x8000 (below the kernel at 1 MiB,
 * above the EBDA) and launches each AP with a SIPI whose vector byte is
 * 0x08 (target CS:IP = 0x0800:0000). The code is fully position
 * independent: the linked image keeps it at ~1 MiB, so every data access
 * goes through a base register (BX/EBX/RBX = trampoline physical base)
 * plus link-time constant offsets (label - ap_trampoline). The BSP fills
 * the data words (pml4/entry/stack/cpu) in the 0x8000 copy and patches
 * the mini-GDT descriptor's base before sending the SIPI.
 *
 * Stages: real mode -> 32-bit protected -> 64-bit long mode -> ap_entry().
 * It reuses the boot.S long-mode recipe: PAE, CR3 = kernel PML4 (identity
 * low map covers both the trampoline and the kernel image), EFER.LME, PG.
 */

.set TRAMP_SEL_CODE32, 0x08
.set TRAMP_SEL_DATA,   0x10
.set TRAMP_SEL_CODE64, 0x18

.section .trampoline, "ax"
.align 16
.code16
.global ap_trampoline
ap_trampoline:
    cli
    cld
    xorw  %ax, %ax
    movw  %ax, %ds
    movw  %ax, %es
    movw  %ax, %ss
    movw  $0x8000, %sp               /* transient stack below the code */
    /* BX = CS << 4 = our physical base (0x8000) */
    movw  %cs, %ax
    movw  %ax, %bx
    shlw  $4, %bx
    /* our mini GDT (base field patched by the BSP) */
    movw  $(tramp_gdt_ptr - ap_trampoline), %si
    lgdt  (%bx,%si)
    /* protected mode */
    movl  %cr0, %eax
    orl   $1, %eax
    movl  %eax, %cr0
    /* far jump: offset = base + (pm32 - ap_trampoline), under 64 KiB */
    movw  $(pm32 - ap_trampoline), %ax
    addw  %bx, %ax
    pushw $TRAMP_SEL_CODE32
    pushw %ax
    lretw

.code32
pm32:
    movzwl %bx, %ebx                 /* EBX = base, upper bits clear */
    movw  $TRAMP_SEL_DATA, %ax
    movw  %ax, %ds
    movw  %ax, %es
    movw  %ax, %ss
    /* PAE on */
    movl  %cr4, %eax
    orl   $(1 << 5), %eax
    movl  %eax, %cr4
    /* CR3 = kernel PML4 (BSP wrote it) */
    movl  (tramp_pml4-ap_trampoline)(%ebx), %eax
    movl  %eax, %cr3
    /* EFER.LME = 1 */
    movl  $0xC0000080, %ecx
    rdmsr
    orl   $(1 << 8), %eax
    wrmsr
    /* enable paging -> long mode */
    movl  %cr0, %eax
    orl   $(1 << 31), %eax
    movl  %eax, %cr0
    /* far jump into 64-bit code */
    movl  $(lm64 - ap_trampoline), %eax
    addl  %ebx, %eax
    pushl $TRAMP_SEL_CODE64
    pushl %eax
    lretl

.code64
lm64:
    movw  $TRAMP_SEL_DATA, %ax       /* flat data segs (C reloads its own GDT) */
    movw  %ax, %ds
    movw  %ax, %es
    movw  %ax, %ss
    movq  (tramp_stack-ap_trampoline)(%rbx), %rsp
    movl  (tramp_cpu-ap_trampoline)(%rbx), %edi   /* cpu index -> arg */
    movq  (tramp_entry-ap_trampoline)(%rbx), %rax
    jmp   *%rax

/* ---- data block: every word written by the BSP before SIPI ---- */
.balign 8
.global tramp_pml4
tramp_pml4:  .quad 0                  /* kernel PML4 physical address */
.global tramp_entry
tramp_entry: .quad 0                  /* ap_entry() address */
.global tramp_stack
tramp_stack: .quad 0                  /* per-AP boot stack top */
.global tramp_cpu
tramp_cpu:   .quad 0                  /* logical cpu index */

.balign 8
.global tramp_gdt_ptr
tramp_gdt_ptr:
    .word tramp_gdt_end - tramp_gdt - 1
    .quad 0                           /* base: BSP writes the 0x8000-copy GDT addr */
.global tramp_gdt
tramp_gdt:
    .quad 0x0000000000000000          /* 0x00 null */
    .quad 0x00CF9A000000FFFF          /* 0x08 code32 */
    .quad 0x00CF92000000FFFF          /* 0x10 data */
    .quad 0x00AF9A000000FFFF          /* 0x18 code64 (L=1) */
tramp_gdt_end:
.global ap_trampoline_end
ap_trampoline_end:

/* ---- ap_context_enter (runs from the linked kernel image, NOT the copy) ----
 *
 * rdi = kesp pointing at a struct pt_regs. Pops it exactly the way the isr.S
 * epilogue would and "returns" into the frame -- how a booting AP adopts its
 * idle task without ever having taken an interrupt. Never returns. */
.section .text
.code64
.global ap_context_enter
ap_context_enter:
    movq  %rdi, %rsp
    popq  %rax
    popq  %rcx
    popq  %rdx
    popq  %rbx
    popq  %rbp
    popq  %rsi
    popq  %rdi
    popq  %r8
    popq  %r9
    popq  %r10
    popq  %r11
    popq  %r12
    popq  %r13
    popq  %r14
    popq  %r15
    addq  $16, %rsp             /* int_no, err_code */
    iretq
