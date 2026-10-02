# Novix — a real OS, grown out of a kernel module

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

**Novix is inspired by Linux's PCB (Process Control Block) model of
process management**, and is developed on top of that foundation. This
repository started life as a Linux kernel module (`src/`) that *simulated*
PCB-style process management — fake PCBs, fake context switches, fake
memory. It has since grown into **Novix**: a real, bootable, multitasking
operating system that runs on the bare metal of QEMU — real boot, real
interrupts, real page tables, real ring3 user programs — with every
"fake" of the original module replaced by the real thing.

```
    _   __
   / | / /___ _____  __
  /  |/ / __ `/ __ \/ /
 / /|  / /_/ / /_/ /  \
/_/ |_/\__,_/\____/_/\_\
Novix 0.2.0 -- a real OS, grown out of a kernel module
```

## What actually works

| Subsystem | Status | Details |
|---|---|---|
| Boot | ✅ | Multiboot, i386 protected mode, VGA text + COM1 serial console |
| Interrupts | ✅ | GDT/TSS, IDT, 8259 PIC remap, PIT 100 Hz timer |
| Memory | ✅ | Bitmap physical allocator, free-list kernel heap (16 MiB), 4 MiB large pages for kernel + per-process 4 KiB page directories |
| Multitasking | ✅ | Preemptive, interrupt-frame context switch via a single `iret`; 5 policies (PRIORITY / RR / FCFS / EDF / RMS) switchable at runtime; aging anti-starvation |
| User space | ✅ | ring3 ELF32 loader, per-process address spaces, `int 0x80` syscalls (15), `waitpid`/zombie reaping, orphan reparenting |
| File system | ✅ | VFS (inode/file/file_operations split), ramfs root, devfs (`/dev/console`, `/dev/null`), per-task fd table, initrd-packed `/bin` |
| Fault isolation | ✅ | ring3 exceptions kill the offending process (Unix `128+signo` exit status) instead of panicking; timestamped leveled kernel log (`klog`) |
| Shell | ✅ | Interactive shell over serial/keyboard: `help ps spawn kill policy uptime mem ls cat mkdir touch rm echo(> >>) clear reboot poweroff` |
| Testing | ✅ | 18/18 kernel self-tests + 21/21 automated shell checks, both wired to CI-friendly exit codes |

The multitasking tests verify observable behavior, not print statements:
priority/FCFS/EDF *execution order*, RR interleaving, sleep/wake timing,
aging rescue of a starved low-priority task, and user programs whose exit
statuses are asserted (`hello` → 42, `spawner` → 7 after reaping two children).
The filesystem tests assert create/write/read/seek/stat/unlink semantics,
fd-table exhaustion and release, and a user-space `filetest` program that
exercises the new syscalls end to end.

## Quick start

Requirements: Python 3, QEMU (`qemu-system-i386`), and either `zig` or any
i686-elf cross toolchain. On Windows the build scripts auto-discover bundled
tools under `tools/`.

```bash
cd os

python build.py          # build kernel + user programs
python build.py test     # headless self-test: boots, runs 18 tests, exits 1 on pass
python build.py run      # interactive: boot to the Novix shell (type "help")
```

Sample interactive session (`python build.py run`):

```
novix> ls
  bin                      <dir>
  dev                      <dir>
  tmp                      <dir>
3 entries
novix> echo hello files > /tmp/note.txt
novix> cat /tmp/note.txt
hello files
novix> spawn spawner
spawned pid 20 (spawner)
novix> [spawner] pid=20, spawning two children
[spawner] children: 21, 22; waiting...
[hello] user process running, pid=21
[hello] user process running, pid=22
...
[spawner] child 21 -> 42, child 22 -> 42
[spawner] all children reaped, exiting with 7
novix> ps
PID   PPID  STATE     PRI  TICKS  USER  NAME
0     -1    READY     0   275    no    idle
1     0     RUN       10  279    no    init
20    1     ZOMBIE    5   5      yes   spawner
```

## Repository layout

```
os/                     ← the real OS (Novix)
├── kernel/             boot.S, GDT/IDT/PIC/PIT, paging, PMM, heap,
│                       scheduler, tasks, syscalls, ELF loader, shell, ktest
├── kernel/fs/          VFS core, path resolution, ramfs, devfs
├── user/               ring3 programs: crt0, ulib, hello, spawner, filetest
├── tools/              build orchestration + ramfs image packer
│                       + shell test driver + kernel symbolizer
├── docs/DESIGN.md      architecture document (Chinese)
└── build.py            build / run / test / clean
src/                    ← the original Linux kernel module (historical)
test/                   ← original module test programs (historical)
```

The original kernel module is kept for history and comparison. It was an
honest teaching artifact — but a simulator: it faked PCBs, faked context
switches, faked memory. Novix replaces every "fake" with the real thing.
See `os/docs/DESIGN.md` for the module → OS mapping table.

## Design highlights

- **One frame to switch them all.** Every interrupt, exception, and syscall
  enters through a stub that builds a uniform `pt_regs` frame. `isr_handler()`
  returns the stack pointer of whichever task should resume next — so a
  single `iret` at the end is simultaneously "return from interrupt" and
  "context switch". New tasks start life as a hand-built fake frame.
- **Honest memory.** The kernel is identity-mapped with 4 MiB large pages;
  user processes get private page directories with 4 KiB user pages at
  0x08048000, stack top at 0xC0000000. Zombie reaping frees every page,
  verified by the test suite's free-page accounting.
- **Input that doesn't lie.** Console input works over both PS/2 (IRQ1) and
  serial (IRQ4, for `qemu -serial stdio`), merged into one ring buffer with
  interrupt-safe draining and a polling fallback — so automated tests never
  lose keystrokes.
- **Syscalls that cross rings.** `int 0x80` with DPL 3 gates; the frame's
  segment registers carry the task back to ring 3, and the TSS carries the
  kernel stack back on the next entry.

## How Novix still differs from a real OS

Novix is a real OS in the mechanical sense — it boots, it preempts, it
switches rings. Measured against a production OS such as Linux, however,
substantial gaps remain. They are listed here honestly (the same spirit
that removed the original module's fake subsystems rather than keeping
them):

| Area | Real OS (e.g. Linux) | Novix 0.2 today |
|---|---|---|
| Process creation | `fork()`/`exec()` with COW address spaces, threads | `spawn` loads ELF from ramfs `/bin` (no fork/exec/threads yet) |
| File system | VFS + persistent file systems (ext4, FAT…) | ramfs only (no persistence, no permissions); no `rmdir` yet |
| Virtual memory | demand paging, COW, swapping, `mmap`, `brk` | a page fault kills the offending process (no demand paging / COW / swap yet); user programs are purely static; no sbrk/mmap |
| IPC & signals | signals, pipes, shared memory, sockets | none |
| SMP & locking | multicore, spinlocks/mutexes/RCU, preemptible kernel | single core, `cli`/`sti` only, non-preemptible kernel |
| Device drivers | unified driver model, disk, network, USB, GPU | only VGA text, COM1, PS/2 keyboard, PIT |
| Syscalls | 300+ (`open`/`read`/`mmap`/`brk`/`kill`…) | 15 |
| Security | users, permissions, capabilities, LSM, ASLR | no user accounts, no permission model |
| Platform | 64-bit, UEFI/ACPI, real hardware | 32-bit i386, Multiboot; `poweroff` relies on the QEMU isa-debug-exit device |
| Network | full TCP/IP stack | none |
| Time | HPET/RTC wall clock, timezones | a single 100 Hz PIT monotonic tick |

These gaps are exactly what the roadmap below is about.

## Roadmap

- fork/exec + copy-on-write (ramfs `open`/`read` is ready for `exec`)
- Signals, sbrk-style user heap growth, pipes
- Copy-on-write, demand paging, swapping
- SMP (the original module's `smp.c` was an empty shell; doing it for real)
- Disk drivers + FAT32, then users & login
- Extensive docs in both English and Chinese

## License

MIT — see [LICENSE](LICENSE).
