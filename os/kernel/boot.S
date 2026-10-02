/* Novix boot: Multiboot 1 a.out-kludge header + long-mode trampoline.
 *
 * Loading (spike-verified, v0.3.0 phase 0): QEMU -kernel takes the
 * a.out-kludge path -- no ELF parsing, file bytes [0, load_end-load_addr)
 * are copied linearly to physical [load_addr, load_end). linker.ld keeps
 * one linear layout so vaddr == 0x100000 + file_off - DELTA; build.py
 * fills the real DELTA into _mb_load_addr with a two-pass link.
 *
 * Loader handoff: 32-bit protected mode, paging off, flat segments,
 * eax = multiboot magic, ebx = multiboot info pointer. QEMU 11.1 passes
 * 0x2BADB003 where the spec says 0x2BADB002 -- normalized here so the C
 * side stays spec-clean.
 *
 * Long-mode map: static PML4/PDPT/PD in .bss identity-map the low 1 GiB
 * with 2 MiB pages. This is the boot-time incarnation of the "user page
 * dirs share the kernel low map" design; paging.rs takes over in
 * milestone C.
 */

.set MB_MAGIC,    0x1BADB002
.set MB_FLAGS,    (1 << 0) | (1 << 1) | (1 << 16)  /* ALIGN | MEMINFO | KLUDGE */
.set MB_CHECKSUM, -(MB_MAGIC + MB_FLAGS)

.set MB_BOOT_MAGIC,      0x2BADB002
.set MB_BOOT_MAGIC_QEMU, 0x2BADB003  /* QEMU 11.1 observed value */

.set SEL_BOOT_CODE32, 0x18          /* trampoline-only selector */

/* ---- multiboot header: must sit in the first 8 KiB of the loaded image */
.section .mbhdr, "a"
.align 4
mb_header:
    .long MB_MAGIC
    .long MB_FLAGS
    .long MB_CHECKSUM
    .long mb_header         /* header_addr: where the header lands (0x100000) */
    .long _mb_load_addr     /* load_addr: 0x100000 - DELTA */
    .long _mb_load_end      /* load_end_addr: end of loaded bytes (vaddr) */
    .long _mb_bss_end       /* bss_end_addr */
    .long _start            /* entry_addr */

/* ---- bootstrap GDT; selector numbers match novix.h (KCODE=08 KDATA=10) */
.section .rodata
.align 8
boot_gdt:
    .quad 0x0000000000000000          /* 0x00: null */
    .quad 0x00AF9A000000FFFF          /* 0x08: code64 (L=1) */
    .quad 0x00CF92000000FFFF          /* 0x10: data */
    .quad 0x00CF9A000000FFFF          /* 0x18: code32 (trampoline only) */
boot_gdt_end:
.align 8
boot_gdt_ptr:
    .word boot_gdt_end - boot_gdt - 1
    .quad boot_gdt

lm_fail_msg:
    .asciz "\nboot: PANIC -- CPU has no 64-bit long mode\n"

/* ---- boot stack + static page tables (zeroed by the trampoline) */
.section .bss
.align 16
.global boot_stack_bottom
boot_stack_bottom:
    .space 16384
.global boot_stack_top
boot_stack_top:

.align 4096
.global boot_pml4
boot_pml4:   .space 4096
.global boot_pdpt
boot_pdpt:   .space 4096
.global boot_pd
boot_pd:     .space 4096
/* v0.7.0: [3GiB, 4GiB) supervisor 2 MiB map for the APIC MMIO region
 * (IOAPIC 0xFEC00000, LAPIC 0xFEE00000), linked as PDPT[3]. Shared into
 * every user directory so apic_eoi() works on any CR3; leafs stay
 * supervisor-only, so ring 3 still faults on them. */
.global boot_pd_mmio
boot_pd_mmio:.space 4096

/* COM1 puts -- used only on the fatal "no long mode" path (pre-console) */
.section .text
.code32
.type boot_com1_puts, @function
boot_com1_puts:                  /* esi = NUL-terminated string */
1:  lodsb
    testb %al, %al
    jz    2f
    movb  %al, %bl
    movw  $0x3FD, %dx
3:  inb   %dx, %al               /* poll LSR bit 5: THR empty */
    testb $0x20, %al
    jz    3b
    movb  %bl, %al
    movw  $0x3F8, %dx
    outb  %al, %dx
    jmp   1b
2:  ret
.size boot_com1_puts, . - boot_com1_puts

.global _start
.type _start, @function
_start:
    cli
    cld
    /* QEMU quirk: normalize 0x2BADB003 -> spec magic before C sees it */
    cmpl  $MB_BOOT_MAGIC_QEMU, %eax
    jne   1f
    movl  $MB_BOOT_MAGIC, %eax
1:
    /* stash handoff args where they survive .bss zeroing (edi!) and
     * CPUID (eax-d): ebp = magic, esi = multiboot info */
    movl  %eax, %ebp
    movl  %ebx, %esi

    lgdt  boot_gdt_ptr
    ljmp  $SEL_BOOT_CODE32, $pm32
pm32:
    movw  $0x10, %ax
    movw  %ax, %ds
    movw  %ax, %es
    movw  %ax, %fs
    movw  %ax, %gs
    movw  %ax, %ss
    movl  $boot_stack_top, %esp

    /* zero .bss: the kludge loader does not reliably zero it, and the
     * static page tables must start all-zero */
    movl  $__bss_start, %edi
    movl  $__bss_end, %ecx
    subl  %edi, %ecx
    shrl  $2, %ecx
    xorl  %eax, %eax
    rep stosl

    /* require long mode (CPUID.80000001H.EDX bit 29) */
    movl  $0x80000000, %eax
    cpuid
    cmpl  $0x80000001, %eax
    jb    no_longmode
    movl  $0x80000001, %eax
    cpuid
    testl $(1 << 29), %edx
    jz    no_longmode

    /* identity-map the low 1 GiB: PD[0..511] = 2MiB pages (PS|RW|P),
     * PDPT[0] -> PD, PML4[0] -> PDPT */
    movl  $boot_pd, %edi
    movl  $0x83, %eax
    movl  $512, %ecx
2:  movl  %eax, (%edi)
    movl  $0, 4(%edi)
    addl  $0x00200000, %eax
    addl  $8, %edi
    loop  2b
    movl  $boot_pdpt, %eax
    orl   $3, %eax
    movl  %eax, boot_pml4
    movl  $0, boot_pml4+4
    movl  $boot_pd, %eax
    orl   $3, %eax
    movl  %eax, boot_pdpt
    movl  $0, boot_pdpt+4

    /* v0.7.0: map [3GiB, 4GiB) the same way; PDPT[3] -> MMIO PD.
     * Entries beyond the physical holes are never touched -- mapping them
     * is harmless and keeps the fill loop branch-free. */
    movl  $boot_pd_mmio, %edi
    movl  $0xC0000083, %eax          /* 3 GiB | PS|RW|P */
    movl  $512, %ecx
3:  movl  %eax, (%edi)
    movl  $0, 4(%edi)
    addl  $0x00200000, %eax
    addl  $8, %edi
    loop  3b
    movl  $boot_pd_mmio, %eax
    orl   $3, %eax
    movl  %eax, boot_pdpt+24         /* PDPT[3] */
    movl  $0, boot_pdpt+24+4

    /* CR4.PAE is mandatory for long mode */
    movl  %cr4, %eax
    orl   $(1 << 5), %eax
    movl  %eax, %cr4
    movl  $boot_pml4, %eax
    movl  %eax, %cr3

    /* EFER.LME = 1 (MSR 0xC0000080) */
    movl  $0xC0000080, %ecx
    rdmsr
    orl   $(1 << 8), %eax
    wrmsr

    /* enable paging and enter 64-bit code in one motion */
    movl  %cr0, %eax
    orl   $0x80000000, %eax          /* CR0.PG */
    movl  %eax, %cr0
    ljmp  $0x08, $lm64

no_longmode:
    movl  $lm_fail_msg, %esi
    call  boot_com1_puts
    movw  $0x501, %dx                /* isa-debug-exit: 2 -> QEMU exit 5 */
    movw  $2, %ax
    outw  %ax, %dx
4:  hlt
    jmp   4b
.size _start, . - _start

/* ---- 64-bit entry: data segments + stack, then hand off to C */
.code64
lm64:
    movw  $0x10, %ax
    movw  %ax, %ds
    movw  %ax, %es
    movw  %ax, %fs
    movw  %ax, %gs
    movw  %ax, %ss
    movq  $boot_stack_top, %rsp
    /* SysV AMD64: rdi = multiboot magic, rsi = multiboot info.
     * 32-bit moves zero-extend, which also canonicalizes the pointers:
     * upper GPR halves are undefined after the 32-bit trampoline. */
    movl  %ebp, %edi
    movl  %esi, %esi
    xorl  %ebp, %ebp              /* terminate the boot frame chain */
    call  kmain
5:  cli
    hlt
    jmp   5b
