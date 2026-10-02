/* user program entry: call main, then exit(main's return value).
 * ABI (v0.3.0): status in rdi, syscall number in rax. At _start the kernel
 * hands us a 16-byte aligned stack, so the call below satisfies SysV's
 * "rsp % 16 == 8 at function entry". */
.text
.code64
.global _start
.type _start, @function
_start:
    call main
    mov %eax, %edi        /* status */
    mov $1, %eax          /* SYS_EXIT */
    int $0x80
1:  hlt
    jmp 1b
.size _start, . - _start
