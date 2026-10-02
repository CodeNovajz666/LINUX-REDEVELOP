/* Novix v0.3.0 阶段 0 spike 链接脚本（ELF64，基址 1MiB）—— 模板
 *
 * a.out-kludge 语义：QEMU 将文件字节 [0, load_end - load_addr) 原样拷贝到
 * 物理 [load_addr, load_end)，即文件偏移 F 的内容落在物理 load_addr + F。
 * .skernel 为单段连续布局，文件偏移与 vaddr 线性对应：
 *     vaddr = 0x100000 + (文件偏移 - H)，H = .skernel 的文件偏移
 * 由此（地址字段全取 vaddr，仅 load_addr 随变体不同）：
 *   flat-bin 变体：objcopy -O binary 后头部在文件偏移 0 -> load_addr = 0x100000（delta=0）
 *   ELF64   变体：头部在文件偏移 H              -> load_addr = 0x100000 - H（delta=H）
 *
 * @DELTA@ 由 run_spike.py 按变体替换（zig cc 不支持 --defsym，故走模板生成）。
 */
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

_mb_load_addr = 0x100000 - @DELTA@;

SECTIONS
{
    . = 0x100000;

    .skernel : {
        KEEP(*(.mbhdr))
        *(.text.entry)
        *(.text*)
        *(.rodata*)
        *(.data*)
    }
    _mb_load_end = .;

    .bss : {
        _bss_start = .;
        *(.bss*)
        *(COMMON)
        _mb_bss_end = .;
    }

    /DISCARD/ : { *(.comment) *(.eh_frame*) *(.note*) }
}
