#include "novix.h"
#include "multiboot.h"
#include "console.h"
#include "gdt.h"
#include "isr.h"
#include "pic.h"
#include "apic.h"
#include "smp.h"
#include "percpu.h"
#include "timer.h"
#include "pmm.h"
#include "heap.h"
#include "paging.h"
#include "keyboard.h"
#include "ramfs.h"
#include "devfs.h"
#include "sched.h"
#include "task.h"
#include "ktest.h"
#include "shell.h"
#include "elf.h"
#include "mm.h"
#include "lib.h"

bool g_interactive;
uint64_t g_ram_top;

extern char boot_stack_top[];      /* boot.S */

static void banner(void) {
    console_clear();
    kprintf("\n");
    kprintf("    _   __             \n");
    kprintf("   / | / /___ _____  __\n");
    kprintf("  /  |/ / __ `/ __ \\/ /\n");
    kprintf(" / /|  / /_/ / /_/ /  \\\n");
    kprintf("/_/ |_/\\__,_/\\____/_/\\_\\\n");
    kprintf("Novix %s -- a real OS, grown out of a kernel module\n\n", NOVIX_VERSION);
}

/* ---- v0.3.0 milestone gates, run inside the init task (pid 1) ----
 *
 * C: full ring3 round trip -- spawn /bin/hello from the ramfs, let it
 *    print through the syscall path, verify exit status 42 and that
 *    every page it owned came back.
 * D: the multitasking group (four scheduling policies, sleep/wake,
 *    aging, user processes through syscalls, ring3 fault isolation,
 *    cross-task kills).
 * E: the filesystem group (ramfs/VFS/fd table).
 *
 * The early group (pmm/heap/paging) runs in kmain *before* interrupts
 * go on: the paging test swaps CR3 back and forth, and a preempting
 * tick would restore the wrong directory when the task resumes. */

static bool gate_c(void) {
    uint32_t free0 = pmm_free_pages();
    struct task *t = task_spawn_path("/bin/hello");
    if (!t) {
        kprintf("gate C: /bin/hello not found in ramfs\n");
        return false;
    }
    int st = task_waitpid(t->pid, 0);
    if (st != 42) {
        kprintf("gate C: hello exit status %d, want 42\n", st);
        return false;
    }
    if (pmm_free_pages() != free0) {
        kprintf("gate C: %u pages leaked by the user task\n", free0 - pmm_free_pages());
        return false;
    }
    kprintf("gate C: ring3 hello ok (exit 42, pages fully reclaimed)\n");
    return true;
}

static bool gate_d(void) {
    uint32_t free0 = pmm_free_pages();
    if (ktest_run_mt() != 0) return false;
    if (pmm_free_pages() != free0) {
        kprintf("gate D: %u pages leaked across the mt suite\n",
                free0 - pmm_free_pages());
        return false;
    }
    kprintf("gate D: mt suite green, pages fully reclaimed\n");
    return true;
}

static bool gate_e(void) {
    /* ramfs has no rmdir: the fs suite deliberately leaves /tmp and
     * /tmp/kdir behind, so heap equality is not the right invariant
     * here. PMM pages are the hard currency -- the fs suite spawns no
     * user processes, so every page it touched must be back. */
    uint32_t free0 = pmm_free_pages();
    if (ktest_run_fs() != 0) return false;
    if (pmm_free_pages() != free0) {
        kprintf("gate E: %u pages leaked by the fs suite\n",
                free0 - pmm_free_pages());
        return false;
    }
    kprintf("gate E: fs suite green, pages fully reclaimed\n");
    return true;
}

/* F (v0.6.0): demand paging, swap and mmap. Runs inside init like its
 * siblings; the swapped-page accounting is checked with the watermark
 * forced high, then restored -- gate C/D/E have already run. */
static bool gate_f(void) {
    uint32_t free0 = pmm_free_pages();
    if (ktest_run_vm() != 0) return false;
    if (pmm_free_pages() != free0) {
        kprintf("gate F: %u pages leaked by the vm suite\n",
                free0 - pmm_free_pages());
        return false;
    }
    if (swap_free_slots() != swap_total_slots()) {
        kprintf("gate F: %u swap slots leaked by the vm suite\n",
                swap_total_slots() - swap_free_slots());
        return false;
    }
    kprintf("gate F: vm suite green, pages and slots fully reclaimed\n");
    return true;
}

/* G (v0.7.0): SMP. Runs AFTER smp_release() -- the APs are online and
 * joining the runqueue while the suite counts on two CPUs, so no pmm
 * equality snapshot here (kswapd and the APs legitimately move pages). */
static bool gate_g(void) {
    if (ktest_run_smp() != 0) return false;
    kprintf("gate G: smp suite green (%d cpu(s) online)\n", smp_cpus_online());
    return true;
}

/* init (pid 1): runs every gate, then either hands the machine to the
 * interactive shell or prints the test summary and exits QEMU. Being
 * pid 1 is what makes "kill 1" hit the shell's own guard. */
static void init_main(void *arg) {
    (void)arg;
    bool ok = gate_c() && gate_d() && gate_e() && gate_f();

    /* v0.7.0: C..F ran strictly single-core (APs parked at the trampoline
     * gate); now let them join the runqueue and prove SMP under load */
    smp_release();
    ok = gate_g() && ok;

    kprintf("\n%s\n", ok ? "GATE-E-OK" : "GATE-E-FAILED");

    int failed = ktest_failed_count();
    kprintf("\nKTEST SUMMARY: %d/%d passed, %d failed\n",
            ktest_total_count() - failed, ktest_total_count(), failed);

    if (g_interactive) {
        if (!ok) kprintf("boot: WARNING -- gate failed, shell may misbehave\n");
        shell_run();                           /* never returns */
    }

    if (ok) kprintf("NOVIX-BOOT-OK\n");
    else kprintf("NOVIX-BOOT-FAILED\n");
    qemu_exit(ok ? 0 : 1);       /* v0.3.0 gates: QEMU exit code 1 = all green */
    for (;;) halt();
}

void kmain(uint32_t magic, struct multiboot_info *mbi) {
    console_init();
    banner();

    if (magic != MULTIBOOT_BOOT_MAGIC)
        panic("multiboot magic %08x invalid", magic);

    if (mbi->flags & MB_FLAG_MEMINFO)
        g_ram_top = 0x100000u + mbi->mem_upper * 1024u;
    else
        g_ram_top = RAM_TOP_MAX;
    if (g_ram_top > RAM_TOP_MAX) g_ram_top = RAM_TOP_MAX;
    if (g_ram_top < 0x01000000u)
        panic("need at least 16 MiB, have %llu MiB", (unsigned long long)(g_ram_top >> 20));

    if ((mbi->flags & 0x4) && mbi->cmdline)
        g_interactive = strstr((const char *)(uintptr_t)mbi->cmdline, "interactive") != NULL;

    kprintf("boot: magic ok, ram top %llu MiB, interactive=%s\n",
            (unsigned long long)(g_ram_top >> 20), g_interactive ? "yes" : "no");

    /* v0.7.0: percpu_init wires %gs -> &cpus[0] BEFORE anything touches
     * this_cpu() (gdt_init builds CPU0's private GDT/TSS through it) */
    percpu_init();
    gdt_init();
    tss_set_kstack((uint64_t)(uintptr_t)boot_stack_top);
    idt_init();
    pic_init();                       /* remap only; smp_early parks the 8259 */

    pmm_init(g_ram_top);
    heap_init();
    swap_init();                     /* v0.6.0: reserve swap frames before any
                                        test snapshots the free-page count */
    timer_init(TICK_HZ);             /* PIT runs into a masked IRQ0 line now */

    /* v0.7.0: MP table -> APIC mode. From here on every external IRQ is
     * IOAPIC-delivered (keyboard/serial to the BSP) and the tick comes
     * from the per-CPU LAPIC timer; must run before sti. */
    smp_early();

    keyboard_init();

    paging_init(g_ram_top);
    ramfs_init();
    devfs_init();
    ramfs_mount_initrd(ramfs_img, ramfs_img_size);

    /* early group with interrupts off: the paging test swaps CR3 */
    if (ktest_run_early() != 0) {
        kprintf("\nNOVIX-BOOT-FAILED\n");
        qemu_exit(1);
        for (;;) halt();
    }

    sched_init();
    task_create_idle("idle");
    if (!task_create_kernel("init", init_main, NULL, PRIO_MAX))
        panic("cannot create init task");
    swap_create_daemon();            /* v0.6.0: kswapd reclaim thread */
    smp_start_aps();                 /* v0.7.0: APs up but parked at the gate */

    __asm__ volatile("sti");
    kprintf("boot: interrupts on (GDT64+TSS64+IDT64, APIC mode, %d cpu(s))\n", ncpu);
    kprintf("boot: C+Rust kernel up (paging.rs four-level), ramfs mounted, %u free pages\n",
            pmm_free_pages());

    /* kmain *is* the idle task (pid 0): sleep until something happens */
    for (;;) halt();
}
