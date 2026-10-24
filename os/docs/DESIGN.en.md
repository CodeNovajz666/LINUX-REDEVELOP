# Novix Design Document (English edition, v1.1.0)

The Chinese original (`docs/DESIGN.md`) is authoritative; this edition
mirrors its structure for the v1.1.0 milestone. Where the two disagree,
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
instead of demanding ones you never chose.

Non-goals (kept honest, see §6): no threads, no euid split, no network,
no slow-hash KDF, no fsck, single tty.

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
      │   (persistent root, LFN read/write, slot reuse), devfs.c, pipe.c
      ├─ drivers/: driver.c (probe→init→/dev registry), ata.c (PIO LBA28,
      │   MBR, FLUSH), rtc.c (CMOS wall clock)
      ├─ task.c: PCB, fork(COW), exec(argv), waitpid, zombie reaping
      ├─ sched.c: one global runqueue, on_cpu ownership protocol,
      │   PRIORITY/RR/FCFS/EDF/RMS + aging
      ├─ signal.c, isr.c (ring3 kills with 128+signo), syscall.c (38 numbers)
      ├─ elf.c: ELF64 registered loader + argv stack push (16-byte ABI)
      ├─ ktest.c: 43 self-tests (3 early + 19 mt + 5 fs + 3 vm + 4 smp
      │   + 4 disk + 3 uid + 2 osh)
      └─ shell.c: kernel debug shell (console=debug back door)
user programs (29): crt0, ulib, sha256 (user twin), etc (shared /etc
  parsing); hello, spawner, fault_div, fault_segv, filetest, forker,
  forkacct, execer, malloctest, sigtest, masktest, pipetest, memhog;
  getty, login, osh, id, whoami, who, passwd, useradd, register, cat,
  touch, uidtest; echo, grep, init, iotest
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
stack top 0x7FFFFFFFE000. Demand paging, COW and swap resolve for both
privilege levels.

### 3.5 exec argv ABI (fixed in v1.0.0)
`kernel/elf.c` pushes `[rsp]=argc, [rsp+8]=argv[0]...` with rsp
16-byte aligned. The v0.9.0 layout could leave an 8-byte gap between
the rounded rsp and the array for particular string-length
combinations, so argv[0] read unwritten stack; the array position is
now pinned to `argc_off + 8`.

### 3.6 Filesystem
VFS (inode/file/f_op split), POSIX owner/group/world checks with root
bypass, per-task cwd. FAT32 persistent root with LFN and deleted-slot
reuse (directories stop growing on churn); ramfs keeps /dev, /tmp
(world-writable) and /var. Mount table overlays the FAT root over
ramfs. Pipes are ring buffers with EOF/EPIPE.

### 3.7 Users and login
uid/gid/ruid in the PCB; SUID exec (`/bin/passwd`); salted SHA-256
shadow with a 3-strike pause; init (kernel pid 1) supervises
`/bin/init`, which runs `/etc/init`'s one-shot lines and supervises the
respawn service (getty). login drops privileges, writes utmp, prints
/etc/motd and execs the account's shell. While /etc/passwd holds zero
accounts, getty execs the registration wizard instead (§3.9).

### 3.8 osh (v1.0.0 user shell)
Pipelines (`|`, dup2-wired), redirections (`>` `>>` `<`), background
jobs (`&`, `jobs`, `wait`), shell variables (`NAME=value`, `$VAR`,
`$?`, `$pid`), comments. Explicit scope: no quoting, variables are not
exported, no Ctrl-C routing to jobs.

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

## 4. Build and run

```
python build.py            # kernel + 29 user programs + 64 MiB FAT32 disk
python build.py test       # headless: 43 tests, 2 CPUs, exit code gates
python build.py test -smp 1
python build.py run        # boot into the registration/login world
python tools/test_shell.py # 78 automated checks over three live sessions
python build.py iso        # novix.iso via grub-mkrescue (host needs
                           # grub-pc-bin + xorriso; skipped honestly
                           # when absent -- QEMU -kernel is the tested path)
```

## 5. Differences from a production OS (v1.0)

| Area | Novix today |
|---|---|
| Processes | fork/exec/COW; no threads |
| FS | FAT32 root (no rmdir of non-empty dirs, no fsck); Unix bits are boot-time fixups in the inode cache |
| Security | uid/gid/ruid, SUID, POSIX bits, out-of-box registration; no euid split, no KDF, no ASLR |
| Syscalls | 38 |
| Boot | QEMU -kernel (tested, CI-covered); ISO target (grub-mkrescue, host-tool gated) |
| Network | none |
