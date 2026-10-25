#ifndef NOVIX_H
#define NOVIX_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "lib.h"

#define NOVIX_VERSION "1.1.8"

/* ---- SMP (v0.7.0) ---- */
#define SMP_MAX            4        /* QEMU -smp cap for stress runs */
#define TRAMPOLINE_ADDR    0x8000u  /* AP real-mode trampoline page */

/* ---- memory layout (physical == virtual for kernel, identity) ---- */
#define KERNEL_LOAD_ADDR   0x00100000u   /* 1 MiB, where multiboot loads us */
#define HEAP_START         0x00400000u   /* heap FLOOR: heap_init starts at
                                     * max(HEAP_START, __bss_end) (v1.1.1) */
#define HEAP_END           0x01000000u   /* 16 MiB -- grown in 0.2.0: ramfs lives here */
#define PMM_START          0x01000000u   /* first page managed by PMM */
#define RAM_TOP_MAX        0x08000000u   /* cap at 128 MiB */
#define PAGE_SIZE          4096u

/* user space sits just above the kernel's 1 GiB identity map: the classic
 * 0x400000 base would land on the kernel heap (4-16 MiB), so user code
 * starts at 1 GiB. The stack tops out below the canonical-half boundary. */
#define USER_CODE_BASE     0x40000000ull
#define USER_STACK_TOP     0x7FFFFFFFE000ull
/* v1.1.4: 8 pages (32 KiB) of user stack; the page(s) below the stack
 * VMA are the GUARD -- never mapped, never reachable: mmap's allocator
 * is capped at USER_MMAP_LIMIT, comfortably below the stack, so a stack
 * overflow faults on the guard and dies by SIGSEGV instead of walking
 * into an mmap region. */
#define USER_STACK_PAGES   8u
#define USER_MMAP_LIMIT    (USER_STACK_TOP - (USER_STACK_PAGES + 4) * 0x1000ull)
/* sbrk heap ceiling (v0.5.0): code starts at 1 GiB, the heap grows up from
 * the image end; 2 GiB leaves a huge gap to the stack (PML4[255]) for the
 * mmap region planned in 0.6.0. Everything stays under PML4[0] (512 GiB). */
#define USER_HEAP_LIMIT    0x80000000ull

/* ---- task config ---- */
#define TASK_MAX           64
#define TASK_NAME_LEN      16
#define TASK_MAX_ARGV      8         /* v0.9.0: exec argv slots */
#define TASK_MAX_ENVP      16        /* v1.1.5: exec envp slots */
#define ARG_MAX_LEN        64        /* v0.9.0: per-argument byte cap */
#define TASK_CWD_LEN       80
#define KSTACK_SIZE        16384u
#define PRIO_MIN           1
#define PRIO_MAX           10
#define TICK_HZ            100u
#define RR_QUANTUM         2
#define PRIO_QUANTUM       4
#define AGING_INTERVAL     20          /* runnable-but-not-run ticks before boost */

/* ---- segment selectors ---- */
#define SEL_KCODE          0x08
#define SEL_KDATA          0x10
#define SEL_UCODE          0x1B
#define SEL_UDATA          0x23
#define SEL_TSS            0x28

/* ---- IRQ numbers ---- */
#define IRQ_TIMER          0
#define IRQ_KEYBOARD       1
#define IRQ_SERIAL         4    /* COM1 */

/* ---- POSIX-compatible fatal signal numbers (subset; v0.4 will grow this) */
#define SIGINT   2
#define SIGILL   4
#define SIGTRAP  5
#define SIGBUS   7
#define SIGFPE   8
#define SIGKILL  9
#define SIGSEGV  11
#define SIGTERM  15
#define SIG_ERR  128          /* exit status convention: 128 + signo */
#define WNOHANG  1            /* waitpid flag: poll instead of block */

/* signals are 1..31; SIGKILL can never be caught, ignored or blocked */
#define SIG_MIN  1
#define SIG_MAX  31

/* sigprocmask(2) how values */
#define SIG_BLOCK    0
#define SIG_UNBLOCK  1
#define SIG_SETMASK  2

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint16_t inw(uint16_t port) {
    uint16_t v;
    __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void io_wait(void) { outb(0x80, 0); }

/* ---- v0.7.0 SMP primitives ---- */

static inline void wrmsr(uint32_t msr, uint64_t val) {
    __asm__ volatile("wrmsr" : :
                     "a"((uint32_t)val), "d"((uint32_t)(val >> 32)), "c"(msr));
}
static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
static inline void cpu_pause(void) { __asm__ volatile("pause"); }
static inline void mfence(void) { __asm__ volatile("mfence" ::: "memory"); }

/* local-CPU interrupt control. On SMP this no longer protects shared data
 * (cli/sti is per-CPU); use spinlock.h for shared structures. Still correct
 * for local-only critical sections (frame building, CR3 swap windows). */
typedef uint32_t irq_flags_t;
static inline irq_flags_t irq_save(void) {
    uint64_t f;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(f));
    return (irq_flags_t)f;
}
static inline void irq_restore(irq_flags_t f) {
    if (f & 0x0200) __asm__ volatile("sti");
}
static inline void halt(void) { __asm__ volatile("hlt"); }

void panic(const char *fmt, ...);
void qemu_exit(int code);   /* isa-debug-exit; only meaningful under QEMU */

#endif /* NOVIX_H */
