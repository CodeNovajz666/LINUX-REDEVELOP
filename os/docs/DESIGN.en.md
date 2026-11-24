# Novix Design Document (English edition, v1.2.0)

The Chinese original (`docs/DESIGN.md`) is authoritative; this edition
mirrors its structure for the v1.2.0 milestone. Where the two disagree,
the code and the Chinese text win.

## 1. Goals and non-goals

Novix is a real, bootable, multitasking, multi-core operating system for
x86_64 under QEMU, grown out of a Linux kernel module that *simulated*
PCB-style process management. Every "fake" of the original module was
replaced by the real thing: real boot, real interrupts, real page tables,
ring 3, spinlocks. v1.0.0 completed the "a real OS" promise: bootable
from ISO (GRUB multiboot), a scripting user shell (osh), a user-space
init reading `/etc/init`, the documentation set — and out-of-box
registration: the image ships zero accounts, so the first boot asks
you to *create* your credentials (root password, then your user)
instead of demanding ones you never chose. The v1.1.x series then closed
the honesty gaps one by one: line editing, rename/rmdir, PBKDF2,
a guarded stack, environment variables, a text toolbox, a full
(euid, egid, ruid, rgid) identity split, and a real `/proc`.

Non-goals (kept honest, see §6): no network, no file-backed
writeback mmap, no fsck, single tty.

## 2. Architecture

```
QEMU (x86_64, -smp 2, -m 128M, IDE disk + RTC localtime)
 └─ Multiboot1(kludge) / GRUB ISO → boot.S (long-mode trampoline) → kmain
      ├─ console (VGA 0xB8000 + COM1, atomic message writes, tty echo bit)
      ├─ GDT64/TSS64 per-core, shared IDT64, IOAPIC/LAPIC (8259 parked)
      ├─ smp/mp.c: MP-table scan → smp/smp.c: AP trampoline, INIT-SIPI-SIPI
      ├─ percpu.c: struct cpu[] + gs:0 self-anchor, this_cpu() via %gs
      ├─ PMM bitmap + per-page refcounts, 1024-frame swap-slot pool
      ├─ kernel heap (free-list allocator)
      ├─ paging.rs (Rust, no_std): four-level tables, COW clone, invlpg
      ├─ mm/vma.c: per-process VMA chains (FILE/ANON), mm/swap.c: kswapd,
      │   clock algorithm, TLB-shootdown IPIs
      ├─ fs/: namei.c (path walk + mounts + X-bit traversal), vfs.c (VFS +
      │   fd table + POSIX permission checks), ramfs.c (+initrd), fat32.c
      │   (persistent root, LFN read/write, slot reuse), devfs.c,
      │   procfs.c (read-only synthetic /proc, v1.1.8), pipe.c
      ├─ drivers/: driver.c (probe→init→/dev registry), ata.c (PIO LBA28,
      │   MBR, FLUSH), rtc.c (CMOS wall clock)
      ├─ task.c: PCB, fork(COW), exec(argv/envp), waitpid, zombie reaping
      ├─ sched.c: one global runqueue, on_cpu ownership protocol,
      │   PRIORITY/RR/FCFS/EDF/RMS + aging
      ├─ signal.c, isr.c (ring3 kills with 128+signo), syscall.c (52 numbers)
      ├─ elf.c: ELF64 registered loader + argv/envp stack push (16-byte ABI)
      ├─ ktest.c: 52 self-tests (3 early + 21 mt + 7 fs + 4 vm + 4 smp
      │   + 4 disk + 5 uid + 4 osh)
      └─ shell.c: kernel debug shell (console=debug back door)
user programs (52): crt0, ulib, sha256 (user twin), etc (shared /etc
  parsing); hello, spawner, fault_div, fault_segv, filetest, forker,
  forkacct, execer, malloctest, sigtest, masktest, pipetest, memhog,
  stackovf; getty, login, osh, id, whoami, who, passwd, useradd, register,
  cat, touch, uidtest, euidtest, shadowtest; echo, grep, init, iotest;
  ls, pwd, rm, cp, mv, mkdir, rmdir, date, sleep, uname, clear, env;
  wc, head, tail, which, find, cut, thread_hello, threadmix
build time: mkramfs.py packs /bin into the kernel; mkfatimg.py makes the
  64 MiB FAT32 disk (MBR, LFN) with /etc (an EMPTY passwd -- the
  out-of-box image ships zero accounts; shadow holds only the
  shadow-only "kck" hash anchor; inittab/issue/init/motd); accounts
  and home dirs are created at runtime by the registration wizard
  and useradd
```

## 3. Key mechanisms

### 3.1 One uniform interrupt frame
Every interrupt, exception and syscall builds the same 22-qword
`pt_regs` (15 GPRs + int_no/err + rip/cs/rflags/rsp/ss). `isr_handler`
returns the stack pointer of whichever task resumes next — one `iretq`
is both "return from interrupt" and "context switch". New tasks start
from a hand-built fake frame.

### 3.2 Ownership, not hope (SMP)
`schedule()` runs under `sched_lock`: if `prev` still belongs to this
core, park it (`on_cpu = -1`) and pick the next task in the same
critical section — a task is pickable only while parked, so "sleeping
task stolen by another core" is structurally impossible.

### 3.3 Lock order
`sched_lock > fs/pipe > pmm/heap > console`; leaf locks (e.g.
`ata_lock`, the per-mount FAT32 lock) never nest upward; irqsave
variants are the default.

### 3.4 Address space
Kernel low 2 MiB identity-mapped with 2 MiB pages and shared into every
user page directory — syscalls dereference user pointers directly. User
code at 0x40000000, sbrk heap to 0x80000000, anonymous mmap above,
stack top 0x7FFFFFFFE000. The user stack is 8 pages (32 KiB); the four
pages below it are never mapped and the mmap ceiling
(`USER_MMAP_LIMIT`) sits below them, so runaway recursion becomes a
clean SIGSEGV instead of corruption (v1.1.4). Demand paging, COW and
swap resolve for both privilege levels.

### 3.5 exec argv/envp ABI
`kernel/elf.c` pushes `[rsp]=argc, [rsp+8]=argv[0]...` with rsp
16-byte aligned. The v0.9.0 layout could leave an 8-byte gap between
the rounded rsp and the array for particular string-length
combinations, so argv[0] read unwritten stack; the array position is
now pinned to `argc_off + 8`. Since v1.1.5 the frame is
`[argc][argv][envp][strings]` with both arrays NULL-terminated;
crt0 publishes `environ`.

### 3.6 Filesystem
VFS (inode/file/f_op split), POSIX owner/group/world checks with root
bypass, per-task cwd. FAT32 persistent root with LFN and deleted-slot
reuse (directories stop growing on churn); ramfs keeps /dev, /tmp
(world-writable) and /var. `rename(2)`/`rmdir(2)` landed in v1.1.2
(rename edits the inode in place; rmdir is empty-directory only), and
`mv` takes the rename fast path across a mount. A read-only procfs is
mounted at `/proc` (v1.1.8): `(name, generator)` rows regenerate
`uptime`/`version`/`meminfo`/`cpuinfo` on every read; there is no write
path. Mount table overlays the FAT root over ramfs. Pipes are ring
buffers with EOF/EPIPE. Directory listings go through
`vfs_dir_foreach` (one path walk + one directory scan per listing; the
old readdir-by-index form re-scanned per entry). FAT32 appends are
O(1) via a chain-tail cache, and whole-cluster reads/writes bypass the
bounce buffer entirely.

### 3.7 Users and login
(ruid, euid)/(rgid, egid) in the PCB (v1.1.7); SUID exec raises only
the effective ids (`/bin/passwd`), and `access(2)` consults the real
pair. Shadow uses PBKDF2-HMAC-SHA256 (8-char salt, per-line iteration
count, 4096 by default; v1.1.3) with legacy salted-SHA-256 lines
upgraded on the next successful login. A 3-strike pause guards login;
init (kernel pid 1) supervises `/bin/init`, which runs `/etc/init`'s
one-shot lines and supervises the respawn service (getty). login drops
privileges, writes utmp, prints /etc/motd and execs the account's
shell. While /etc/passwd holds zero accounts, getty execs the
registration wizard instead (§3.9).

### 3.8 osh (v1.0.0 user shell)
Pipelines (`|`, dup2-wired), redirections (`>` `>>` `<`), background
jobs (`&`, `jobs`, `wait`), shell variables (`NAME=value`, `$VAR`,
`$?`, `$pid`), comments. Since v1.1.5 `export` promotes a variable into
the environment passed to children, and `$PATH` decides command lookup;
v1.1.1 added history recall and line editing. Explicit scope: no
quoting. Ctrl-C routing to jobs landed in v1.2.0 — each osh job runs
in one process group and the console's registered foreground group
gets the SIGINT (§3.10).

### 3.9 Out-of-box registration (v1.0.0)
The image ships zero accounts. getty counts valid /etc/passwd lines
(`etc_user_count`); on zero it execs `/bin/register`, a two-step
wizard: (1) set the root password (a system without an administrator
could never run useradd), (2) create the first user — uid from
`etc_max_uid()+1` with a floor of 100; an empty name skips the step
and leaves a valid root-only system. Passwords are entered twice with
echo off and a 3-strike brake; the write is shadow-first, so an
interrupted wizard never leaves a passwd entry nobody can log in
with. The wizard exits back into the init respawn; the next getty
pass sees the accounts and shows the normal login prompt. Registered
accounts live on the FAT32 disk (they survive poweroff) and the
wizard never runs again.

### 3.10 v1.1.x additions and v1.2.0
Line editing (v1.1.1: ↑/↓/Home/End/Ctrl-U/Ctrl-W decoded once in the
kernel input layer), rename/rmdir (v1.1.2), PBKDF2 + legacy upgrade
(v1.1.3), guarded 32 KiB stack (v1.1.4), environ/export/$PATH
(v1.1.5), the text toolbox wc/head/tail/which/find/cut (v1.1.6), the
(euid, egid, ruid, rgid) split with setreuid/setregid/access (v1.1.7),
and procfs (v1.1.8). v1.2.0 added threads and job control:
`clone(CLONE_VM)` on a reference-counted shared (mm, page_dir) pair
behind a per-mm lock, ulib pthread create/join/exit/mutex, and process
groups with Ctrl-C SIGINT routed to the console's registered
foreground group (foreground job dies with status 130, background
jobs and the prompt survive). A pre-release performance round rounded
it off: word-wide lib copies (`rep movsq`/`stosq`), single-pass
directory enumeration (`vfs_dir_foreach`), FAT32 O(1) appends with
whole-cluster I/O, one CR3 reload per fork instead of per-page
`invlpg`, a ranged TLB-shootdown ring (64 entries; a lagging CPU
full-flushes instead of losing a range), O(1) allocator bits, and a
one-pass exit teardown (swap slots freed inside the page-table walk,
which also required zeroing stale swap-marker PTEs on unmap). See the
Chinese `docs/DESIGN.md` §4.14.

## 4. Build and run

```
python build.py            # kernel + 52 user programs + 64 MiB FAT32 disk
python build.py test       # headless: 52 tests, 2 CPUs, exit code gates
python build.py test -smp 1
python build.py run        # boot into the registration/login world
python tools/test_shell.py # 126 automated checks over three live sessions
                           # (NOVIX_QEMU_TRACE=1 keeps a QEMU trace)
python build.py iso        # novix.iso via grub-mkrescue (host needs
                           # grub-pc-bin + xorriso; skipped honestly
                           # when absent -- QEMU -kernel is the tested path)
```

## 5. Differences from a production OS (v1.2.0)

| Area | Novix today |
|---|---|
| Processes | fork/exec/COW **and threads** (v1.2.0: `clone(CLONE_VM)` on a reference-counted shared address space with a per-mm lock, ulib pthread create/join/exit/mutex); no CLONE_FILES/SIGHAND-granular sharing |
| FS | FAT32 root, procfs, rename/rmdir (rmdir is empty-directory only, no fsck); Unix bits are boot-time fixups in the inode cache |
| Security | (ruid, euid)/(rgid, egid), SUID, POSIX bits, `access(2)`, PBKDF2 shadow, out-of-box registration; no capabilities/LSM, no ASLR |
| Syscalls | 52 |
| Boot | QEMU -kernel (tested, CI-covered); ISO target (grub-mkrescue, host-tool gated) |
| Network | none |
