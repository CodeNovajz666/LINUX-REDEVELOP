#!/usr/bin/env python3
"""
Novix build orchestration.

Toolchain discovery: zig cc (clang+lld) as x86_64 cross compiler, rustc for
the Rust module (paging.rs), QEMU to run. Works on Windows/Linux/macOS.

  python build.py            build kernel + user programs
  python build.py run        build, then boot QEMU (milestone C gate, then exit)
  python build.py test       build, boot headless, check gate exit code
  python build.py clean      remove build artifacts

Both `run` and `test` accept `-smp N` (default 2, v0.7.0): how many CPUs
QEMU exposes. `-smp 1` is the single-core regression configuration.

v0.3.0 milestone C (see ROADMAP.md): the kernel is C + Rust -- paging.rs is
compiled as a no_std staticlib and linked with the C objects; user programs
are ELF64 and packed into the ramfs image embedded in the kernel binary.
"""
import os, sys, subprocess, shutil, struct

ROOT = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(ROOT, 'build')

# keep zig's global cache/tmp inside the project so builds are self-contained
os.environ.setdefault('ZIG_GLOBAL_CACHE_DIR', os.path.join(BUILD, 'zig-cache'))

# ---------- toolchain discovery ----------

def find_zig():
    env = os.environ.get('ZIG')
    if env:
        return env
    cand = os.path.join(ROOT, '..', 'tools',
                        'zig-x86_64-windows-0.16.0', 'zig.exe')
    if os.path.exists(cand):
        return cand
    which = shutil.which('zig')
    if which:
        return which
    sys.exit('zig not found: set ZIG=<path to zig.exe> or install from https://ziglang.org')

def find_qemu():
    env = os.environ.get('QEMU')
    if env:
        return env
    which = shutil.which('qemu-system-x86_64')
    if which:
        return which
    pkg = os.path.join(ROOT, '..', 'tools', 'qemu-pkg', 'qemu-system-x86_64.exe')
    if os.path.exists(pkg):
        return pkg
    pkg2 = os.path.join(ROOT, '..', 'tools', 'qemu-pkg', 'qemu-system-x86_64w.exe')
    if os.path.exists(pkg2):
        return pkg2
    sys.exit('qemu-system-x86_64 not found: set QEMU=<path> or install QEMU')

def find_rustc():
    env = os.environ.get('RUSTC')
    if env:
        return env
    which = shutil.which('rustc')
    if which:
        return which
    sys.exit('rustc not found: set RUSTC=<path> or install the Rust toolchain '
             '(rustup target add x86_64-unknown-none)')

ZIG = find_zig()
QEMU = find_qemu()
RUSTC = find_rustc()

# ---------- compile helpers ----------

BASE_FLAGS = [
    '-target', 'x86_64-freestanding',
    '-ffreestanding', '-fno-pic', '-fno-pie', '-fno-stack-protector',
    '-fno-builtin', '-fno-asynchronous-unwind-tables',
    '-mno-red-zone',
    '-mno-sse', '-mno-sse2', '-mno-sse3', '-mno-ssse3',
    '-mno-sse4.1', '-mno-sse4.2', '-mno-avx', '-mno-avx2',
    '-mno-mmx', '-mno-80387',
    '-Wall', '-Wextra', '-O2',
]

if os.environ.get('SCHED_TRACE'):
    BASE_FLAGS += ['-DSCHED_TRACE']

KERNEL_INC = os.path.join(ROOT, 'kernel', 'include')

KERNEL_SOURCES = [
    'boot.S', 'kmain.c', 'console.c', 'klog.c', 'lib.c', 'kpanic.c',
    'gdt.c', 'gdt_flush.S', 'idt.c', 'isr.S', 'isr.c',
    'pic.c', 'timer.c', 'pmm.c', 'heap.c', 'keyboard.c',
    # milestone C: tasks + syscalls + ELF64 + fs stack (paging lives in paging.rs)
    'elf.c', 'task.c', 'sched.c', 'syscall.c', 'ktest.c',
    'fs/vfs.c', 'fs/ramfs.c', 'fs/devfs.c', 'fs/namei.c',
    # milestone E: interactive shell (runs in the init task)
    'shell.c',
    # v0.5.0: signal delivery + pipes
    'signal.c', 'fs/pipe.c',
    # v0.6.0: per-process VMA list + demand paging + swap/kswapd
    'mm/vma.c', 'mm/swap.c',
    # v0.7.0: SMP -- per-CPU state, APIC (LAPIC+IOAPIC), MP table,
    # AP trampoline and IPIs
    'percpu.c', 'smp/mp.c', 'smp/apic.c', 'smp/smp.c', 'smp/trampoline.S',
]

USER_PROGRAMS = ['hello', 'spawner', 'fault_div', 'fault_segv', 'filetest',
                 'forker', 'forkacct', 'execer',
                 'malloctest', 'sigtest', 'masktest', 'pipetest',
                 'memhog']

def run(cmd, **kw):
    printable = ' '.join(str(c) for c in cmd)
    print('$', printable)
    r = subprocess.run(cmd, **kw)
    if r.returncode != 0:
        sys.exit(f'command failed ({r.returncode}): {printable}')

def compile_file(src, obj, extra_inc=None):
    os.makedirs(os.path.dirname(obj), exist_ok=True)
    cmd = [ZIG, 'cc'] + BASE_FLAGS + ['-c', src, '-o', obj]
    if extra_inc:
        cmd += extra_inc
    run(cmd)
    return obj

def obj_name(kdir, src):
    # keep the extension in the object name: isr.S and isr.c must not
    # both map to isr.o
    base = os.path.basename(src).replace('.', '_')
    return os.path.join(kdir, base + '.o')

# ---------- Rust module (paging.rs -> staticlib) ----------

def build_paging_rs():
    out = os.path.join(BUILD, 'libpaging_rs.a')
    # relocation-model=static: rustc defaults this target to PIC, which
    # routes extern calls through a GOT the bare-metal link never fills
    # (first symptom: call *GOT -> jump to 0). Static emits direct e8 calls.
    run([RUSTC,
         '--edition', '2021',
         '--crate-type', 'staticlib',
         '--target', 'x86_64-unknown-none',
         '-C', 'panic=abort',
         '-C', 'relocation-model=static',
         '-C', 'opt-level=2',
         '-C', 'debuginfo=0',
         '-o', out,
         os.path.join(ROOT, 'kernel', 'paging.rs')])
    print(f'  rust: {out}')
    return out

# ---------- user programs (ELF64, linked at 1 GiB) ----------

def build_user():
    udir = os.path.join(BUILD, 'user')
    os.makedirs(udir, exist_ok=True)
    user_inc = ['-I' + os.path.join(ROOT, 'user')]
    objs = [compile_file(os.path.join(ROOT, 'user', src),
                         os.path.join(udir, src.replace('.', '_') + '.o'),
                         extra_inc=user_inc)
            for src in ('crt0.S', 'ulib.c')]
    binaries = []
    for prog in USER_PROGRAMS:
        po = compile_file(os.path.join(ROOT, 'user', prog + '.c'),
                          os.path.join(udir, prog + '_c.o'),
                          extra_inc=user_inc)
        out = os.path.join(udir, prog)
        run([ZIG, 'cc'] + BASE_FLAGS + [
            '-nostdlib', '-static',
            '-Wl,-T,' + os.path.join(ROOT, 'user', 'user.ld'),
            '-Wl,--build-id=none',
            '-o', out] + objs + [po])
        binaries.append(out)
        print(f'  user: {out} ({os.path.getsize(out)} bytes)')
    return binaries

def build_ramfs_image(binaries):
    """pack user binaries into the ramfs image embedded in the kernel"""
    out = os.path.join(BUILD, 'ramfs_img.c')
    pairs = ['/bin/%s=%s' % (os.path.basename(b), b) for b in binaries]
    run([sys.executable, os.path.join(ROOT, 'tools', 'mkramfs.py'), out] + pairs)
    return out

# ---------- kernel link (multiboot a.out-kludge) ----------

def elf_load_delta(path):
    """DELTA of the kludge loader: how far below 1 MiB _mb_load_addr sits.

    QEMU copies file bytes linearly to physical [load_addr, load_end), so
    every segment must satisfy p_vaddr == 0x100000 + p_offset - DELTA.
    lld keeps p_offset congruent to p_vaddr across segments; a mismatch
    here means the layout stopped being linear and the kludge load breaks.
    """
    with open(path, 'rb') as f:
        head = f.read(0x800)
    e_phoff, = struct.unpack_from('<Q', head, 0x20)
    e_phentsize, e_phnum = struct.unpack_from('<HH', head, 0x36)
    deltas = []
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        if struct.unpack_from('<I', head, off)[0] != 1:      # PT_LOAD
            continue
        p_offset, = struct.unpack_from('<Q', head, off + 8)
        p_vaddr, = struct.unpack_from('<Q', head, off + 16)
        deltas.append(0x100000 + p_offset - p_vaddr)
    if not deltas:
        sys.exit('kernel ELF has no PT_LOAD: ' + path)
    if len(set(deltas)) != 1:
        sys.exit(f'non-linear PT_LOAD layout, kludge load would break: {sorted(set(deltas))}')
    return deltas[0]

def link_kernel(objs, archives):
    kernel = os.path.join(BUILD, 'kernel.elf')
    gen = os.path.join(BUILD, 'linker_gen.ld')
    tpl = open(os.path.join(ROOT, 'linker.ld')).read()
    delta = 0
    for _ in range(3):
        with open(gen, 'w') as f:
            f.write(tpl.replace('@DELTA@', str(delta)))
        run([ZIG, 'cc'] + BASE_FLAGS + [
            '-nostdlib', '-static',
            '-Wl,-T,' + gen,
            '-Wl,--build-id=none',
            '-o', kernel] + objs + archives)
        actual = elf_load_delta(kernel)
        if actual == delta:
            print(f'  kernel: {kernel} ({os.path.getsize(kernel)} bytes, '
                  f'kludge delta {delta:#x})')
            return kernel
        delta = actual
    sys.exit('kludge delta did not converge')

def build_kernel():
    kdir = os.path.join(BUILD, 'obj')
    os.makedirs(kdir, exist_ok=True)
    objs = []
    for src in KERNEL_SOURCES:
        obj = obj_name(kdir, src)
        compile_file(os.path.join(ROOT, 'kernel', src), obj,
                     extra_inc=['-I' + KERNEL_INC])
        objs.append(obj)
    # generated ramfs image (depends on the user binaries)
    binaries = build_user()
    ramfs_c = build_ramfs_image(binaries)
    ramfs_obj = os.path.join(kdir, 'ramfs_img_c.o')
    compile_file(ramfs_c, ramfs_obj)
    objs.append(ramfs_obj)
    archive = build_paging_rs()
    return link_kernel(objs, [archive])

# ---------- commands ----------

def parse_smp(argv):
    """pull `-smp N` out of argv; default 2 (v0.7.0 SMP is the tested path)"""
    smp = 2
    rest = []
    i = 0
    while i < len(argv):
        if argv[i] == '-smp':
            i += 1
            if i >= len(argv):
                sys.exit('usage: -smp N')
            try:
                smp = int(argv[i])
            except ValueError:
                sys.exit('usage: -smp N (integer)')
            if not 1 <= smp <= 8:
                sys.exit('-smp N: N must be 1..8 (SMP_MAX)')
        else:
            rest.append(argv[i])
        i += 1
    return smp, rest

def cmd_build(argv=None):
    build_kernel()                    # uniform (argv) command signature

def qemu_cmd(smp=2):
    # sockets=N: one physical package per CPU. SeaBIOS writes the MP table
    # with one processor entry per package when HTT is on (default topology
    # underreports cores), so a plain "-smp 2" makes mp.c find a single cpu.
    return [QEMU, '-kernel', os.path.join(BUILD, 'kernel.elf'), '-m', '128M',
            '-smp', f'{smp},sockets={smp},cores=1,threads=1',
            '-display', 'none', '-device', 'isa-debug-exit']

def cmd_run(argv):
    smp, _ = parse_smp(argv)
    build_kernel()
    print(f'booting with {smp} cpu(s) (gates C-G run in the init task, '
          f'then the interactive shell)...')
    cmd = qemu_cmd(smp) + ['-append', 'interactive', '-serial', 'stdio']
    try:
        rc = subprocess.call(cmd)
    except KeyboardInterrupt:
        sys.exit(130)                     # Ctrl-C: user-initiated stop, keep it quiet
    # qemu exits (val<<1)|1: kernel writes 0 -> 1. Treat both 0 and 1 as clean.
    sys.exit(0 if rc in (0, 1) else rc)

def cmd_test(argv):
    smp, _ = parse_smp(argv)
    build_kernel()
    serial_log = os.path.join(BUILD, 'serial.log')
    cmd = qemu_cmd(smp) + ['-serial', 'file:' + serial_log, '-no-reboot']
    print('$', ' '.join(cmd))
    try:
        r = subprocess.run(cmd, timeout=180)
        rc = r.returncode
    except subprocess.TimeoutExpired:
        print('TIMEOUT: QEMU did not exit within 180s')
        rc = 99
    print(f'qemu exit code: {rc}  (1 = gate ok, 5 = boot panic)')
    if os.path.exists(serial_log):
        print('---- serial output ----')
        with open(serial_log, 'r', errors='replace') as f:
            print(f.read())
    sys.exit(0 if rc == 1 else 1)

def cmd_clean():
    shutil.rmtree(BUILD, ignore_errors=True)
    print('cleaned')

if __name__ == '__main__':
    what = sys.argv[1] if len(sys.argv) > 1 else 'build'
    rest = sys.argv[2:]
    {'build': cmd_build, 'run': cmd_run, 'test': cmd_test, 'clean': cmd_clean}[what](rest)
