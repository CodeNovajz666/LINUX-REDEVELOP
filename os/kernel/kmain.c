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
#include "driver.h"
#include "ata.h"
#include "fat32.h"
#include "rtc.h"
#include "sched.h"
#include "task.h"
#include "ktest.h"
#include "shell.h"
#include "elf.h"
#include "mm.h"
#include "lib.h"

bool g_interactive;
bool g_debug_console;                      /* v0.9.0: console=debug -> kernel shell */
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
    int st = 0;
    if (task_waitpid(t->pid, 0, &st) != t->pid || st != 42) {
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

/* H (v0.8.0): disk + driver model + RTC. Runs after G with the APs live:
 * FAT32 lock contention across CPUs is part of the proof. No pmm equality
 * snapshot -- the mounted FAT32 instance owns a permanent inode cache. */
static bool gate_h(void) {
    if (ktest_run_disk() != 0) return false;
    kprintf("gate H: disk suite green (root on %s)\n",
            fat32_mounted() ? "FAT32 /dev/hd0" : "ramfs");
    return true;
}

/* I (v0.9.0): users, login and permission boundaries. Like H, no pmm
 * snapshot: the suite touches FAT (shadow read, alice's home) and the
 * inode cache it grows is permanent by design. */
static bool gate_i(void) {
    if (ktest_run_uid() != 0) return false;
    kprintf("gate I: uid suite green\n");
    return true;
}

/* J (v1.0.0): the osh scripting plumbing. iotest walks the new dup2
 * syscall end to end (pipe+fork wiring, edge semantics, file redirect) --
 * exactly the moves osh's pipelines make -- and the toolkit check proves
 * the shell, its pipeline partners and the /etc script files ship in the
 * image. No pmm snapshot: iotest grows FAT/ramfs state like the other
 * post-disk suites. */
static bool gate_j(void) {
    if (ktest_run_shell() != 0) return false;
    kprintf("gate J: osh plumbing suite green\n");
    return true;
}

/* ---- v0.9.0: boot-time permission fixup ----
 * FAT32 carries no Unix bits, so the inode cache's 0644/0755 defaults are
 * corrected once at boot: everything in /bin becomes executable (passwd
 * additionally gains S_ISUID), /etc/shadow goes root-only, and each
 * /home/<name> directory is chowned to the uid /etc/passwd lists for that
 * name. The fixups live in the inode cache only and are re-applied at
 * every boot -- runtime chowns (useradd) share the same limitation. */

static int parse_dec(const char *s, int len) {
    int v = 0;
    for (int i = 0; i < len && s[i] >= '0' && s[i] <= '9'; i++)
        v = v * 10 + (s[i] - '0');
    return v;
}

static void fix_mode(const char *path, uint16_t perm) {
    struct inode *ino = namei(path);
    if (ino) ino->mode = (ino->mode & S_IFMT) | perm;
}

/* /bin permission fixup, one entry per vfs_dir_foreach callback. Names are
 * clamped to the 31 chars the old readdir(name[32]) loop fed forward. */
struct bin_perm_ctx {
    char full[48];
};

static int bin_perm_one(void *c, const char *name) {
    struct bin_perm_ctx *P = c;
    strcpy(P->full, "/bin/");
    strncpy(P->full + 5, name, sizeof(P->full) - 6);
    P->full[sizeof(P->full) - 1] = 0;
    struct inode *ino = namei(P->full);
    if (!ino || (ino->mode & S_IFMT) != S_IFREG) return 0;
    ino->mode |= 0111;
    if (strcmp(name, "passwd") == 0) ino->mode |= S_ISUID;
    return 0;
}

static void fs_perm_fixup(void) {
    /* /bin: +x for everyone; passwd is the one SUID binary (one /bin walk
     * via foreach -- readdir(i) re-scanned the directory per entry) */
    struct bin_perm_ctx P;
    vfs_dir_foreach("/bin", bin_perm_one, &P);
    fix_mode("/etc/passwd", 0644);
    fix_mode("/etc/shadow", 0600);         /* the one file mortals may not read */
    fix_mode("/etc/issue", 0644);
    fix_mode("/etc/inittab", 0644);
    fix_mode("/etc/init", 0644);           /* v1.0.0: user init script */
    fix_mode("/etc/motd", 0644);           /* v1.0.0: post-login banner */

    /* /home/<name> belongs to the uid /etc/passwd announces for <name> */
    static char buf[2048];
    struct file *f = vfs_open("/etc/passwd", O_RDONLY);
    if (!f) return;
    int n = vfs_read(f, buf, sizeof(buf) - 1);
    vfs_close(f);
    if (n <= 0) return;
    buf[n] = 0;
    for (char *line = buf; line && *line; ) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        /* name:x:uid:gid:comment:home:shell */
        char *c[6] = { 0 };
        char *p = line;
        for (int i = 0; i < 6 && p; i++) {
            c[i] = p;
            char *colon = strchr(p, ':');
            if (colon) { *colon = 0; p = colon + 1; } else p = NULL;
        }
        if (c[0] && c[2] && c[5] && strncmp(c[5], "/home/", 6) == 0) {
            struct inode *home = namei(c[5]);
            if (home && (home->mode & S_IFMT) == S_IFDIR) {
                home->uid = (uint16_t)parse_dec(c[2], (int)strlen(c[2]));
                home->gid = home->uid;
            }
        }
        line = nl ? nl + 1 : NULL;
    }
}

/* ---- v1.0.0: the user-space init ----
 * pid 1 (this kernel task) supervises /bin/init: the user-space script
 * executor that runs /etc/init's one-shot lines and then supervises the
 * respawn service (the tty getty -- same SysV rule as the v0.9.0 loop that
 * lived here, but scripted and editable on the disk). If /bin/init itself
 * ever dies, the kernel respawns it, so the machine always comes back to a
 * login prompt. console=debug still selects the kernel debug shell. */
static void user_init_supervise(void) {
    const char *argv[1] = { "init" };
    int supervised = -1;
    for (;;) {
        if (supervised < 0) {
            struct task *t = task_spawn_argv("/bin/init", argv, 1);
            if (t) {
                supervised = t->pid;
            } else {
                kprintf("init: cannot spawn /bin/init -- retrying\n");
                task_sleep_until(current_task(), timer_ticks() + TICK_HZ);
                kyield();
                continue;
            }
        }
        /* v1.0.1 (F3): block on ANY child, not just /bin/init. Orphans
         * re-parented to pid 1 are reaped the moment they exit instead of
         * piling up as zombies for as long as the supervised child lived;
         * the supervised child's own death still respawns it (SysV rule). */
        int st = 0;
        int pid = task_waitpid(-1, 0, &st);
        if (pid == supervised) {
            kprintf("init: /bin/init exited (status %d) -- respawning\n", st);
            supervised = -1;
        }
    }
}

/* init (pid 1): runs every gate, then either hands the machine to the
 * login world (getty -> login -> sh), the debug kernel shell (console=debug)
 * or prints the test summary and exits QEMU. Being pid 1 is what makes
 * "kill 1" hit the shell's own guard. */
static void init_main(void *arg) {
    (void)arg;
    bool ok = gate_c() && gate_d() && gate_e() && gate_f();

    /* v0.7.0: C..F ran strictly single-core (APs parked at the trampoline
     * gate); now let them join the runqueue and prove SMP under load */
    smp_release();
    ok = gate_g() && ok;
    ok = gate_h() && ok;
    ok = gate_i() && ok;
    ok = gate_j() && ok;

    kprintf("\n%s\n", ok ? "GATES-OK" : "GATES-FAILED");

    int failed = ktest_failed_count();
    kprintf("\nKTEST SUMMARY: %d/%d passed, %d failed\n",
            ktest_total_count() - failed, ktest_total_count(), failed);

    if (g_interactive) {
        if (!ok) kprintf("boot: WARNING -- gate failed, shell may misbehave\n");
        if (g_debug_console) {
            kprintf("boot: console=debug -> kernel shell\n");
            shell_run();                       /* never returns */
        }
        user_init_supervise();                 /* v1.0.0: /bin/init, never returns */
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

    if ((mbi->flags & 0x4) && mbi->cmdline) {
        g_interactive = strstr((const char *)(uintptr_t)mbi->cmdline, "interactive") != NULL;
        g_debug_console = strstr((const char *)(uintptr_t)mbi->cmdline, "console=debug") != NULL;
    }

    const char *cmdline = (mbi->flags & 0x4) && mbi->cmdline
        ? (const char *)(uintptr_t)mbi->cmdline : "";
    bool want_disk_root = strstr(cmdline, "root=/dev/hd0") != NULL;

    kprintf("boot: magic ok, ram top %llu MiB, interactive=%s%s\n",
            (unsigned long long)(g_ram_top >> 20), g_interactive ? "yes" : "no",
            g_debug_console ? ", console=debug" : "");

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
    ramfs_init();                       /* ramfs root: the boot fallback */
    ramfs_mount_initrd(ramfs_img, ramfs_img_size);

    /* v0.8.0: the driver bus -- probe the static registry, bring up what is
     * wired (console/null/zero always, hd0 when a disk answers IDENTIFY) */
    driver_init();
    driver_bus_init();
    devfs_init();                       /* /dev + publish every live device */
    ramfs_mkdir("/tmp");                /* scratch survives the root switch */
    ramfs_mkdir("/var");                /* v0.9.0: utmp home, survives too */
    /* v1.0.0: /tmp is world-writable (Unix convention; no sticky bit yet) --
     * osh redirections and mortal tests create their scratch here */
    {
        struct inode *tmp_ino = namei("/tmp");
        if (tmp_ino) tmp_ino->mode |= 0777;
    }
    ramfs_mkdir("/var/run");
    /* v1.0.0: out-of-box homes. The FAT image ships /root and /home; on the
     * no-disk (ramfs) boot nothing else creates them, and register's
     * umkdir("/home/<name>") would only warn. Invisible after the FAT root
     * switch, so the disk boot is unaffected. */
    ramfs_mkdir("/root");
    ramfs_mkdir("/home");
    rtc_init();

    /* v0.8.0 rootfs switch: FAT32 root over the ramfs root, ramfs /dev, /tmp
     * and /var pinned under it. Failures fall back to the ramfs root, honestly. */
    if (want_disk_root) {
        struct inode *dev_dir = namei("/dev");
        struct inode *tmp_dir = namei("/tmp");
        struct inode *var_dir = namei("/var");
        struct fat32_sb *sb = fat32_mount();
        struct inode *fat_root = sb ? fat32_root_inode(sb) : NULL;
        if (sb && fat_root && dev_dir && tmp_dir && var_dir &&
            vfs_mount(fat_root, "dev", dev_dir) == 0 &&
            vfs_mount(fat_root, "tmp", tmp_dir) == 0 &&
            vfs_mount(fat_root, "var", var_dir) == 0) {
            g_fat_sb = sb;
            vfs_set_root(fat_root);
            kprintf("boot: root on FAT32 (/dev/hd0): %u/%u clusters free, "
                    "%u MiB, ramfs keeps /dev /tmp /var\n",
                    fat32_free_clusters(sb), fat32_total_clusters(sb),
                    ata_sector_count() / 2048);
        } else {
            kprintf("boot: root=/dev/hd0 but no FAT32 volume -- staying on ramfs\n");
            if (sb) fat32_unmount(sb);
        }
    }

    /* v1.1.8: the read-only synthetic /proc -- uptime/version/meminfo/
     * cpuinfo, generated on read; pinned under whichever root is live */
    procfs_init();

    /* v0.9.0: FAT carries no permission bits -- correct the inode cache
     * (exec bits, /etc/shadow 0600, /home ownership) before anything runs */
    fs_perm_fixup();

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
    kprintf("boot: C+Rust kernel up (paging.rs four-level), root on %s, %u free pages\n",
            fat32_mounted() ? "FAT32" : "ramfs", pmm_free_pages());

    /* kmain *is* the idle task (pid 0): sleep until something happens */
    for (;;) halt();
}
