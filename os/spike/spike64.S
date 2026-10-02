/* Novix v0.3.0 阶段 0 spike —— 实测 QEMU `-kernel` 的 ELF64 加载行为
 *
 * 编译期变体（配合 run_spike.py）：
 *   1) ELF64 + multiboot1 a.out-kludge    —— 关键验证点：kludge 路径是否解析 ELF 头
 *   2) flat-bin + multiboot1 a.out-kludge —— zig objcopy -O binary，头部在文件偏移 0
 *   3) ELF64 多重引导头但无 kludge 位（-DSPIKE_NO_KLUDGE）—— 对照组
 *
 * 成功判据：串口出现 "2BADB002K"（multiboot 路径进入且 EAX 交接正确），
 * 且 QEMU 退出码 = (0x33 << 1) | 1 = 103（isa-debug-exit @0x501）。
 */

.set MB_MAGIC,    0x1BADB002
.set MB_ALIGN,    (1 << 0)
.set MB_MEMINFO,  (1 << 1)
.set MB_KLUDGE,   (1 << 16)
#ifdef SPIKE_NO_KLUDGE
.set MB_FLAGS,    MB_ALIGN | MB_MEMINFO
#else
.set MB_FLAGS,    MB_ALIGN | MB_MEMINFO | MB_KLUDGE
#endif
.set MB_CHECKSUM, -(MB_MAGIC + MB_FLAGS)

.section .mbhdr, "a"
.align 4
mb_header:
    .long MB_MAGIC
    .long MB_FLAGS
    .long MB_CHECKSUM
    .long mb_header        /* header_addr：头部落地的物理地址（两变体恒为 0x100000） */
    .long _mb_load_addr    /* load_addr：bin=0x100000；ELF64=0x100000-段文件偏移 */
    .long _mb_load_end     /* load_end_addr（vaddr） */
    .long _mb_bss_end      /* bss_end_addr（vaddr） */
    .long _start           /* entry_addr：multiboot 交接的 32 位入口 */

.section .rodata
hexdig: .ascii "0123456789ABCDEF"

.section .bss
.align 16
spike_stack_bottom:
    .space 16384
spike_stack_top:

.section .text.entry, "ax"
.code32
/* 入口命名为 _start：zig cc 链接时强制 -e _start，multiboot ELF 路径取 e_entry
 * 作入口，两者必须一致（本文件历史版本叫 _mb_entry 时 e_entry 未被设置，
 * ELF 路径变体引导即 fault） */
.global _start
.type _start, @function
_start:
    cli
    cld
    movl  $spike_stack_top, %esp
    xorl  %ebp, %ebp

    /* EAX（multiboot 约定 = 0x2BADB002）逐位十六进制打到 COM1 */
    movl  $8, %ecx
1:  roll  $4, %eax
    movl  %eax, %ebx
    andl  $0xF, %ebx
    movb  hexdig(,%ebx,1), %bl
    call  spike_putchar
    loop  1b
    movb  $0x4B, %bl            /* 'K' */
    call  spike_putchar
    movb  $0x0D, %bl
    call  spike_putchar
    movb  $0x0A, %bl
    call  spike_putchar

    /* isa-debug-exit @0x501：写 0x33 -> QEMU 退出码 (0x33<<1)|1 = 103 */
    movw  $0x501, %dx
    movb  $0x33, %al
    outb  %al, %dx
2:  hlt
    jmp   2b
.size _start, . - _start

/* spike_putchar：%bl -> COM1（查询 LSR bit5 THR empty） */
.type spike_putchar, @function
spike_putchar:
    movw  $0x3FD, %dx
3:  inb   %dx, %al
    testb $0x20, %al
    jz    3b
    movw  $0x3F8, %dx
    movb  %bl, %al
    outb  %al, %dx
    ret
.size spike_putchar, . - spike_putchar
