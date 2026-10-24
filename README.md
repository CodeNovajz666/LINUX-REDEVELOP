# Novix — a real OS, grown out of a kernel module

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

**Novix is inspired by Linux's PCB (Process Control Block) model of
process management**, and is developed on top of that foundation. This
repository started life as a Linux kernel module (`src/`) that *simulated*
PCB-style process management — fake PCBs, fake context switches, fake
memory. It has since grown into **Novix**: a real, bootable, multitasking,
**multi-core** operating system that runs on the bare metal of QEMU — real
boot, real interrupts, real page tables, real ring3 user programs, real
spinlocks — with every "fake" of the original module replaced by the real
thing.

```
    _   __
   / | / /___ _____  __
  /  |/ / __ `/ __ \/ /
 / /|  / /_/ / /_/ /  \
/_/ |_/\__,_/\____/_/\_\
Novix 1.0.0 -- a real OS, grown out of a kernel module
```

## What actually works

| Subsystem | Status | Details |
|---|---|---|
| Boot | ✅ | x86_64 long mode; Multiboot 1 + a.out-kludge loads the ELF64 kernel via QEMU `-kernel` (no GRUB/ISO needed); VGA text + COM1 serial console |
| SMP (v0.7.0) | ✅ | MP-table topology discovery, AP trampoline (INIT-SIPI-SIPI), per-CPU GDT/TSS + `%gs` anchors, per-core LAPIC timer ticks, spinlocks with a documented lock order, TLB-shootdown IPIs, and an `on_cpu` task-ownership protocol that closes the sleep/wake steal race |
| Interrupts | ✅ | IDT64 gates, IOAPIC/LAPIC delivery (8259 parked), LAPIC timer as per-core preemption source (PIT stays as the BSP calibration base) |
| Memory | ✅ | Bitmap physical allocator with page refcounts, free-list kernel heap, **four-level page tables (a Rust module)**, demand paging (FILE/ANON/swap-in fault paths), copy-on-write `fork`, clock-algorithm swap driven by a kswapd kernel thread, anonymous `mmap`/`munmap` |
| Multitasking | ✅ | Preemptive on every core; one global runqueue under `sched_lock`; 5 policies (PRIORITY / RR / FCFS / EDF / RMS) switchable at runtime; aging anti-starvation; sleep/wait/zombie states |
| User space | ✅ | ring3 ELF64 loader, per-process address spaces, `sbrk` heap + user-space malloc/free, `int 0x80` syscalls (44, incl. `dup2`/`getdents`/`unlink`/`datetime`), argv/argc on the user stack, `waitpid`/zombie reaping, orphan reparenting |
| IPC & signals | ✅ | signals 1..31 (catch / block / sigreturn trampoline, SIGKILL unblockable), pipes (ring buffer + EOF/EPIPE), shell Ctrl+C → SIGINT |
| File system | ✅ | VFS (inode/file/file_operations split), ramfs, devfs (`/dev/console`, `/dev/null`), mount table (shadowing + readdir merge), **FAT32 persistent root** (LFN read+write) booted via `root=/dev/hd0`, per-task fd table + per-task cwd, initrd-packed `/bin` (33 programs) |
| Storage, drivers & clock (v0.8.0) | ✅ | ATA PIO disk (IDENTIFY, LBA28, MBR partitions, FLUSH CACHE), unified driver model (probe → init → `/dev` registry: console/null/zero/hd0), CMOS RTC wall clock + `date`; the power-cycle test re-mounts the disk in a fresh instance to prove data reached the platter |
| Users & login (v0.9.0) | ✅ | uid/gid/ruid identity in the PCB (fork inherits, exec keeps), POSIX owner/group/world permission checks in the VFS (root bypass), boot-time permission fixup for FAT32, SUID exec (`/bin/passwd` is the one SUID binary), the login chain init→getty→login→sh driven by `/etc/inittab` (respawn on logout), salted-SHA-256 `/etc/passwd`+`/etc/shadow` password verification with terminal echo off via TCGETS/TCSETS ioctl, `id`/`whoami`/`who`/`passwd`/`useradd` commands; password changes survive poweroff on the FAT32 disk |
| Out-of-box registration (v1.0.0) | ✅ | the disk image ships **zero accounts**; getty detects the empty `/etc/passwd` and runs the two-step wizard instead of login — set the root password, then create the first user (uid ≥ 100, empty name = root-only system). Credentials are created by the first person to boot the machine, survive poweroff on the FAT32 disk, and the wizard never runs again |
| Fault isolation | ✅ | ring3 exceptions kill the offending process (Unix `128+signo` exit status) instead of panicking; kernel-mode #PF on user pages resolves COW/demand faults for both privilege levels |
| Shell | ✅ | Interactive kernel shell over serial/keyboard (Backspace, Tab completion, ↑/↓ history, Ctrl+C) kept as the `console=debug` back door; the login world runs **osh** (v1.0.0) |
| osh scripting (v1.0.0) | ✅ | User-space shell with pipelines `|` (fork+exec+dup2 wiring), redirections `>` `>>` `<`, background jobs `&` + `jobs`/`wait`, shell variables `$VAR`/`$?`/`$pid`, `#` comments; debug built-ins `ps`/`cpus`/`mem [-v]`/`upt`/`kill` over read-only syscalls 39-41; the acceptance line `cat /etc/motd | grep Novix > /tmp/x` runs end to end |
| Interactive toolbox (v1.1.0) | ✅ | the everyday file/system commands live in `/bin` and run straight from osh: `ls` (SYS_GETDENTS — byte-identical to the kernel shell's listing, directory R checked at the boundary), `pwd`, `rm` (SYS_UNLINK), `cp`, `mv` (copy+unlink, honestly no rename(2)), `date` (SYS_DATETIME, kernel-formatted RTC line), `sleep`, `uname`, `clear` (ioctl TCCLEAR); osh gained a `history` built-in and a truthful `help` |
| init & userspace infra (v1.0.0) | ✅ | `/bin/init` reads `/etc/init` (one-shot lines + one supervised `respawn` service, SysV rule), `/etc/motd` after login, `echo`/`grep` in /bin; ISO build target (`build.py iso`, grub multiboot menu, host-tool gated); docs set: `docs/syscall.md` (44 entries, CI-checked against the header), `docs/driver-writer-guide.md`, `docs/DESIGN.en.md` |
| Testing | ✅ | 46/46 kernel self-tests (run on 2 CPUs **and** re-run on 1 CPU for regression) + 93/93 automated shell checks over three live sessions: kernel-shell regression, first-boot registration + login/scripting/toolbox smoke test, persistence of the registered accounts across poweroff |

Tests verify observable behavior, not print statements: scheduling
*order* under four policies, cross-CPU execution proofs (six threads
yielding in a loop must collectively cover both cores, and each must
always observe its own pid through `%gs`), a 100,000-op spinlock
contention counter that must be exact, fork/COW isolation and page
accounting, signal catch/mask round trips, pipe data integrity, and a
forced-watermark swap round trip whose TLB-shootdown IPI count is
asserted, and a FAT32 write/read/unlink round trip whose power-cycle
proof re-mounts the same disk in a fresh instance — RAM state cannot
cheat a remount.

## Quick start

Requirements: Python 3, QEMU (`qemu-system-x86_64`), `zig` (bundled
under `tools/` on Windows) and a Rust toolchain with the
`x86_64-unknown-none` target. The build scripts auto-discover bundled
tools under `tools/`.

```bash
cd os

python build.py              # build kernel + user programs + 64 MiB FAT32 disk image
python build.py test         # headless self-test: 46 tests, 2 CPUs, exit 1 on pass
python build.py test -smp 1  # single-CPU regression run
python build.py run          # interactive: boot into the login world (getty)
python tools/test_shell.py   # 93 automated checks over three live serial sessions
python build.py iso          # novix.iso via grub-mkrescue (needs grub-pc-bin + xorriso)
```

The image ships **no accounts**. The first boot drops into a two-step
registration wizard: set the root password, then create your user
(an empty name leaves a root-only system). After registration the
normal login world runs — log in as `root` or your user for the full
shell (`cd`, `logout`, `/bin` programs). Booting with the kernel
argument `console=debug` skips getty and drops into the kernel debug
shell instead (that is what session 1 of the shell test drives).

Sample first boot (`python build.py run`, 2 CPUs):

```
This is a fresh Novix system -- no user accounts exist yet.

Registration sets the administrator (root) password, then
creates your first user account.

New password for root:
Retype new password:
register: root password set

Choose a username (empty for a root-only system): ktest
New password:
Retype new password:
register: account 'ktest' created (uid 100, home /home/ktest)

Registration complete. Please log in.
Novix 1.0.0 (tty1) -- authorized users only

login: ktest
Password:
Novix 1.0.0 -- a real OS, grown out of a kernel module
osh: pipelines a|b, redirects a>b a>>b a<b, jobs a&, $VAR $?
ktest@novix:/home/ktest$ id
uid=100(ktest) gid=100(ktest)
ktest@novix:/home/ktest$ cat /etc/motd | grep Novix > /tmp/x
ktest@novix:/home/ktest$ cat /tmp/x
Novix 1.0.0 -- a real OS, grown out of a kernel module
ktest@novix:/home/ktest$ hello &
[1] pid 12
ktest@novix:/home/ktest$ wait
ktest@novix:/home/ktest$ logout
Novix 1.0.0 (tty1) -- authorized users only
login:
```

## Repository layout

```
os/                     ← the real OS (Novix)
├── kernel/             boot.S (long-mode trampoline), GDT/IDT, isr, timer,
│                       PMM, heap, scheduler, tasks, syscalls, ELF64 loader,
│                       signal, console, shell, ktest
│   ├── include/        kernel headers (task, sched, smp, percpu, spinlock…)
│   ├── mm/             VMA address spaces, swap (kswapd)
│   ├── smp/            MP-table parsing, LAPIC/IOAPIC, AP startup, IPIs
│   ├── fs/             VFS core, path resolution + mount table, ramfs,
│   │                   devfs, pipes, FAT32 (LFN read+write)
│   ├── drivers/        unified driver model, ATA PIO disk, CMOS RTC
│   └── paging.rs       four-level page tables — the Rust module
├── user/               ring3 programs: crt0, ulib (malloc/printf/syscalls),
│                       hello, spawner, forker, forkacct, execer,
│                       malloctest, sigtest, masktest, pipetest, memhog,
│                       the login world (getty, login, osh, id, whoami,
│                       who, passwd, useradd, cat, touch, uidtest) and
│                       the scripting tools (echo, grep, init, iotest)
├── tools/              build helpers: ramfs image packer, FAT32 image
│                       maker, shell test driver, syscall-doc checker,
│                       ELF/symbol inspectors
├── spike/              v0.3.0 boot-approach spike — the QEMU `-kernel`
│                       ELF64 loading probe that chose Multiboot1 + a.out-kludge
├── docs/DESIGN.md      architecture document (Chinese)
├── docs/DESIGN.en.md   architecture document (English edition)
├── docs/syscall.md     the 38-entry syscall reference (CI-checked)
├── docs/driver-writer-guide.md   how to add a /dev driver
├── grub.cfg            GRUB menu staged into novix.iso by `build.py iso`
├── build.py            build / run / test / iso / clean (-smp N)
└── Makefile            convenience wrapper (make run / test / test1 / shell)
tools/                  ← bundled Windows toolchains: zig, qemu-system-x86_64
src/                    ← the original Linux kernel module (historical)
test/                   ← original module test programs (historical)
docs/                   ← original module API docs (historical)
examples/, scripts/     ← original module examples & shell scripts (historical)
makefile, .gitlab-ci.yaml, create_zip.sh, final_verify.sh
                        ← original module build / CI / packaging (historical)
DESIGN.md               cross-version contracts: address space, signals,
                        syscall ABI, SMP invariants (Chinese)
ROADMAP.md / CHANGELOG.md  per-version plan and history (Chinese)
```

Everything outside `os/` except the root-level `DESIGN.md` /
`ROADMAP.md` / `CHANGELOG.md` / `README.md` and the bundled `tools/`
toolchains belongs to the original kernel-module project and is kept
only for history. Each of those files carries a `[历史归档]` banner at
the top pointing back to the current entry points.

The original kernel module is kept for history and comparison. It was an
honest teaching artifact — but a simulator: it faked PCBs, faked context
switches, faked memory. Novix replaces every "fake" with the real thing.
See `os/docs/DESIGN.md` for the module → OS mapping table.

## Design highlights

- **One frame to switch them all.** Every interrupt, exception, and syscall
  enters through a stub that builds a uniform 22-qword `pt_regs` frame.
  `isr_handler()` returns the stack pointer of whichever task should
  resume next — so a single `iretq` at the end is simultaneously "return
  from interrupt" and "context switch". New tasks start life as a
  hand-built fake frame.
- **C + Rust in one kernel.** The paging subsystem is a `no_std` Rust
  static library (`paging.rs`, target `x86_64-unknown-none`) linked
  beside the C objects — type-safe PTE flags, four-level walk, COW
  cloning, per-page refcount hooks. Everything else stays C for
  teaching clarity.
- **Honest memory.** Kernel low 2 MiB is identity-mapped with large pages
  and shared into every user page directory (syscalls dereference user
  pointers directly). User code loads at 0x40000000, the sbrk heap grows
  to 0x80000000, anonymous mmap allocates above that, the stack tops at
  0x7FFFFFFFE000. Faults on not-yet-touched or COW pages resolve in the
  kernel for *both* privilege levels — demand paging and COW are not
  ring3-only tricks.
- **Ownership, not hope.** Each task carries an `on_cpu` field: parked
  (-1) or owned by a core. A task is pickable only while parked, and the
  owner parks it in the same critical section where it picks the next
  task — which is what makes "sleeping task stolen by another core while
  still on the first core's stack" structurally impossible.
- **Locks with a written law.** `sched_lock > fs/pipe > pmm/heap >
  console`, leaf locks never nest, irqsave variants are the default
  (cli/sti protects only the local core on SMP).
- **Input that doesn't lie.** Console input works over both PS/2 and
  serial, merged into one ring buffer with interrupt-safe draining and a
  polling fallback — so automated tests never lose keystrokes. Tab
  completion and command history ride on top.
- **Files that outlive the kernel.** The build also emits a 64 MiB
  FAT32 image on the emulated IDE disk; with `root=/dev/hd0` it takes
  over as the real root (ramfs keeps only /dev and /tmp), and the
  power-cycle self-test re-mounts the same disk in a fresh instance to
  prove the data reached the platter. Drivers register through one
  model (probe → init → /dev node), and the CMOS RTC backs a real
  `date`.

## How Novix still differs from a real OS

Novix is a real OS in the mechanical sense — it boots, it preempts, it
switches rings, it runs on several cores at once. Measured against a
production OS such as Linux, substantial gaps remain. They are listed
here honestly (the same spirit that removed the original module's fake
subsystems rather than keeping them):

| Area | Real OS (e.g. Linux) | Novix 1.0 today |
|---|---|---|
| Process creation | `fork()`/`exec()`/COW, threads | fork/exec/COW done; **no threads yet** |
| File system | VFS + persistent file systems (ext4, FAT…) | VFS/ramfs/devfs + **FAT32 persistent root** (LFN, power-cycle tested); no `rmdir` of non-empty dirs, no fsck; permission bits enforced (owner/group/world + root bypass) but only fixable at boot (FAT stores no Unix bits) |
| Virtual memory | demand paging, COW, swapping, `mmap`, `brk` | demand paging / COW / swap / anon-mmap done; no reverse mappings, no file-backed mmap, no THP |
| IPC & signals | signals, pipes, shared memory, sockets | signals + pipes done; no shm/sockets |
| SMP & locking | per-CPU runqueues, RCU, preemptible kernel | one global runqueue (fine at this scale); spinlocks only; kernel not preemptible in the strict sense |
| Device drivers | unified driver model, disk, network, USB, GPU | VGA text, COM1, PS/2, PIT/LAPIC, ATA disk, RTC behind one unified driver model (/dev registry); no network/USB/GPU |
| Syscalls | 300+ | 44 |
| Security | users, permissions, capabilities, LSM, ASLR | users & login done (uid/gid/ruid, SUID exec, POSIX permission checks, salted SHA-256 login chain); no capabilities/LSM/ASLR, single uid per process (no euid split), no slow-hash KDF |
| Platform | UEFI/ACPI, real hardware | QEMU `-kernel` is the tested path; `build.py iso` emits a GRUB multiboot ISO (host-tool gated, untested on bare metal); `poweroff` relies on the isa-debug-exit device |
| Network | full TCP/IP stack | none |
| Time | HPET/RTC wall clock, timezones | monotonic per-core tick + RTC wall clock (`date`, local time); no timezones |

These gaps are exactly what the roadmap is about: 1.x (threads, procfs,
TCP/IP, real-machine bring-up). See `ROADMAP.md`.

## License

MIT — see [LICENSE](LICENSE).
