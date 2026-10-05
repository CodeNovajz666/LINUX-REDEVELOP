/* user program entry: hand argc/argv to main, then exit(main's return).
 * v0.9.0: the kernel builds the classic initial stack
 *     rsp -> [argc][argv[0] .. argv[argc-1]][NULL]  (strings above)
 * so argc sits at (%rsp) and argv at 8(%rsp).
 * ABI (v0.3.0): status in rdi, syscall number in rax. At _start the kernel
 * hands us a 16-byte aligned stack, so the call below satisfies SysV's
 * "rsp % 16 == 8 at function entry". */
.text
.code64
.global _start
.type _start, @function
_start:
    mov (%rsp), %rdi        /* argc */
    lea 8(%rsp), %rsi       /* argv */
    call main
    mov %eax, %edi        /* status */
    mov $1, %eax          /* SYS_EXIT */
    int $0x80
1:  hlt
    jmp 1b
.size _start, . - _start
