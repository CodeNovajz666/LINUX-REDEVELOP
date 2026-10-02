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
Novix 0.7.0 -- a real OS, grown out of a kernel module
```

## What actually works

| Subsystem | Status | Details |
|---|---|---|
| Boot | ✅ | x86_64 long mode; Multiboot 1 + a.out-kludge loads the ELF64 kernel via QEMU `-kernel` (no GRUB/ISO needed); VGA text + COM1 serial console |
| SMP (v0.7.0) | ✅ | MP-table topology discovery, AP trampoline (INIT-SIPI-SIPI), per-CPU GDT/TSS + `%gs` anchors, per-core LAPIC timer ticks, spinlocks with a documented lock order, TLB-shootdown IPIs, and an `on_cpu` task-ownership protocol that closes the sleep/wake steal race |
| Interrupts | ✅ | IDT64 gates, IOAPIC/LAPIC delivery (8259 parked), LAPIC timer as per-core preemption source (PIT stays as the BSP calibration base) |
| Memory | ✅ | Bitmap physical allocator with page refcounts, free-list kernel heap, **four-level page tables (a Rust module)**, demand paging (FILE/ANON/swap-in fault paths), copy-on-write `fork`, clock-algorithm swap driven by a kswapd kernel thread, anonymous `mmap`/`munmap` |
| Multitasking | ✅ | Preemptive on every core; one global runqueue under `sched_lock`; 5 policies (PRIORITY / RR / FCFS / EDF / RMS) switchable at runtime; aging anti-starvation; sleep/wait/zombie states |
| User space | ✅ | ring3 ELF64 loader, per-process address spaces, `sbrk` heap + user-space malloc/free, `int 0x80` syscalls (26), `waitpid`/zombie reaping, orphan reparenting |
| IPC & signals | ✅ | signals 1..31 (catch / block / sigreturn trampoline, SIGKILL unblockable), pipes (ring buffer + EOF/EPIPE), shell Ctrl+C → SIGINT |
| File system | ✅ | VFS (inode/file/file_operations split), ramfs root, devfs (`/dev/console`, `/dev/null`), per-task fd table, initrd-packed `/bin` (13 programs) |
| Fault isolation | ✅ | ring3 exceptions kill the offending process (Unix `128+signo` exit status) instead of panicking; kernel-mode #PF on user pages resolves COW/demand faults for both privilege levels |
| Shell | ✅ | Interactive shell over serial/keyboard with line editing: Backspace, Tab completion (commands + live `/bin` listing), ↑/↓ history, Ctrl+C |
| Testing | ✅ | 34/34 kernel self-tests (run on 2 CPUs **and** re-run on 1 CPU for regression) + 45/45 automated shell checks on a live 2-CPU session |

Tests verify observable behavior, not print statements: scheduling
*order* under four policies, cross-CPU execution proofs (six threads
yielding in a loop must collectively cover both cores, and each must
always observe its own pid through `%gs`), a 100,000-op spinlock
contention counter that must be exact, fork/COW isolation and page
accounting, signal catch/mask round trips, pipe data integrity, and a
forced-watermark swap round trip whose TLB-shootdown IPI count is
asserted.

## Quick start

Requirements: Python 3, QEMU (`qemu-system-x86_64`), `zig` (bundled
under `tools/` on Windows) and a Rust toolchain with the
`x86_64-unknown-none` target. The build scripts auto-discover bundled
tools under `tools/`.

```bash
cd os

python build.py              # build kernel + user programs
python build.py test         # headless self-test: 34 tests, 2 CPUs, exit 1 on pass
python build.py test -smp 1  # single-CPU regression run
python build.py run          # interactive: boot to the Novix shell (type "help")
python tools/test_shell.py   # 45 automated checks over a live serial session
```

Sample interactive session (`python build.py run`, 2 CPUs):

```
novix> cpus
CPU  APIC  ONLINE  CURRENT
0    0     yes     init
1    1     yes     idle
novix> spawn spawner
spawned pid 4 (spawner)
novix> [spawner] pid=4, fork+exec two children
[spawner] children: 5, 6; waiting...
[hello] user process running, pid=5
[hello] user process running, pid=6
[spawner] child 5 -> 42, child 6 -> 42
[spawner] all children reaped, exiting with 7
novix> ps
PID   PPID  STATE     PRI  CPU  TICKS  USER  EXIT      NAME
0     -1    READY     0   0    660    no    -         idle
1     0     RUN       10  0    396    no    -         init
2     0     READY     8   1    646    no    -         kswapd
3     -1    RUN       0   1    177    no    -         idle
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
│   ├── fs/             VFS core, path resolution, ramfs, devfs, pipes
│   └── paging.rs       four-level page tables — the Rust module
├── user/               ring3 programs: crt0, ulib (malloc/printf/syscalls),
│                       hello, spawner, forker, forkacct, execer,
│                       malloctest, sigtest, masktest, pipetest, memhog…
├── tools/              build helpers: ramfs image packer, shell test
│                       driver, ELF/symbol inspectors
├── spike/              v0.3.0 boot-approach spike — the QEMU `-kernel`
│                       ELF64 loading probe that chose Multiboot1 + a.out-kludge
├── docs/DESIGN.md      architecture document (Chinese)
├── build.py            build / run / test / clean (-smp N)
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

## How Novix still differs from a real OS

Novix is a real OS in the mechanical sense — it boots, it preempts, it
switches rings, it runs on several cores at once. Measured against a
production OS such as Linux, substantial gaps remain. They are listed
here honestly (the same spirit that removed the original module's fake
subsystems rather than keeping them):

| Area | Real OS (e.g. Linux) | Novix 0.7 today |
|---|---|---|
| Process creation | `fork()`/`exec()`/COW, threads | fork/exec/COW done; **no threads yet** |
| File system | VFS + persistent file systems (ext4, FAT…) | VFS/ramfs/devfs only — **no persistence yet** (FAT32 is v0.8.0); no `rmdir`, no permission bits, no cwd |
| Virtual memory | demand paging, COW, swapping, `mmap`, `brk` | demand paging / COW / swap / anon-mmap done; no reverse mappings, no file-backed mmap, no THP |
| IPC & signals | signals, pipes, shared memory, sockets | signals + pipes done; no shm/sockets |
| SMP & locking | per-CPU runqueues, RCU, preemptible kernel | one global runqueue (fine at this scale); spinlocks only; kernel not preemptible in the strict sense |
| Device drivers | unified driver model, disk, network, USB, GPU | VGA text, COM1, PS/2 keyboard, PIT/LAPIC — disk & RTC are v0.8.0 |
| Syscalls | 300+ | 26 |
| Security | users, permissions, capabilities, LSM, ASLR | none yet (users & login are v0.9.0) |
| Platform | UEFI/ACPI, real hardware | QEMU `-kernel` only so far; `poweroff` relies on the isa-debug-exit device |
| Network | full TCP/IP stack | none |
| Time | HPET/RTC wall clock, timezones | monotonic per-core tick only |

These gaps are exactly what the roadmap is about: v0.8.0 (ATA disk +
FAT32 + driver framework + RTC), v0.9.0 (users, login, permissions),
v1.0.0 (bootable ISO, user-space shell, init). See `ROADMAP.md`.

## License

MIT — see [LICENSE](LICENSE).
