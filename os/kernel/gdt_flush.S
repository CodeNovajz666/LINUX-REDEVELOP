/* GDT reload from long mode, then LTR.
 * SysV AMD64 args: rdi = gdt pointer, esi = entry count, edx = tss selector.
 * CS is already 0x08 with identical attributes in the new table, but reload
 * it anyway via far return -- cheap and keeps this routine self-contained. */
.text
.code64
.global gdt_flush
gdt_flush:
    mov   %esi, %ecx
    shl   $3, %ecx             /* byte limit = entries*8 - 1 */
    dec   %ecx
    sub   $10, %rsp
    mov   %cx, (%rsp)          /* build the 10-byte gdtdesc on the stack */
    mov   %rdi, 2(%rsp)
    lgdt  (%rsp)
    add   $10, %rsp

    mov   $0x10, %ax
    mov   %ax, %ds
    mov   %ax, %es
    mov   %ax, %fs
    mov   %ax, %ss
    /* %gs is deliberately untouched: IA32_GS_BASE (per-CPU anchor, v0.7.0)
     * aliases the descriptor base, and any "mov to %gs" here would zero it */

    pushq $0x08
    leaq  1f(%rip), %rax
    pushq %rax
    lretq
1:
    mov   %edx, %eax
    ltr   %ax
    ret
