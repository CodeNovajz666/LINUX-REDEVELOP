/* Novix kernel linker script (x86_64, physical 1 MiB, identity-mapped).
 *
 * Loaded via Multiboot 1 a.out-kludge (QEMU -kernel, spike-verified):
 * file bytes are copied linearly to physical [load_addr, load_end), so
 * the image must satisfy vaddr == 0x100000 + file_off - DELTA with one
 * DELTA for every segment (lld keeps p_offset congruent to p_vaddr).
 *
 * @DELTA@ (file offset of the first PT_LOAD) is filled in by build.py
 * with a two-pass link: pass 1 yields the real offset, pass 2 bakes it
 * into _mb_load_addr -- only symbol values change, so the layout is
 * stable and the delta converges on the second pass.
 */
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

_mb_load_addr = 0x100000 - @DELTA@;

SECTIONS
{
    . = 0x00100000;

    .skernel : {
        KEEP(*(.mbhdr))
        *(.text .text.*)
        *(.rodata .rodata.*)
        *(.data .data.*)
        /* orphaned relocatable sections must land INSIDE the kludge copy
         * range: rustc emits GOT-indirect calls (e.g. copy_nonoverlapping
         * -> memcpy via `call [rip+off]`) even with relocation-model=static,
         * and an orphan .got after _mb_load_end is never loaded -- the call
         * then jumps to whatever garbage RAM holds in that slot (v0.4.0) */
        *(.got .got.*)
        *(.data.rel.ro .data.rel.ro.*)
        /* v0.7.0 AP trampoline: position-independent code, copied to
         * 0x8000 at runtime (smp.c); linked here just to stay loaded */
        *(.trampoline)
    }
    _mb_load_end = .;

    .bss : ALIGN(4K) {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
        _mb_bss_end = .;
    }

    . = ALIGN(4K);
    __kernel_end = .;

    /DISCARD/ : { *(.comment) *(.eh_frame*) *(.note*) }
}
