#!/usr/bin/env python3
"""Novix v0.3.0 阶段 0 spike：实测 QEMU `-kernel` 对 ELF64 的加载行为（定方案 A/B）。

四个变体（详见 spike64.S / spike64.ld）：
  bin  flat binary + multiboot1 a.out-kludge（zig objcopy -O binary）
  elf  ELF64        + multiboot1 a.out-kludge（两遍链接定 load_addr）
  nkl  ELF64        + multiboot 头、不置 kludge 位（对照组：预期被拒）
  mb1  ELF32 经典 multiboot 路径（对照组：现有 kernel.elf 同款，观测其 EAX magic）

成功判据：串口输出含 "2BADB00{2|3}K"（multiboot 路径进入；QEMU 11.1 实测
kludge 路径传 0x2BADB003，multiboot1 规范值为 0x2BADB002，两者均记为有效），
且 QEMU 退出码 = (0x33 << 1) | 1 = 103。
"""
import os, sys, re, subprocess, struct

HERE = os.path.dirname(os.path.abspath(__file__))
OS   = os.path.dirname(HERE)
OUT  = os.path.join(OS, 'build', 'spike')

# keep zig's global cache/tmp inside the project (same as build.py)
os.environ.setdefault('ZIG_GLOBAL_CACHE_DIR', os.path.join(OS, 'build', 'zig-cache'))

def find_zig():
    if os.environ.get('ZIG'):
        return os.environ['ZIG']
    cand = os.path.join(OS, '..', 'tools', 'zig-x86_64-windows-0.16.0', 'zig.exe')
    if os.path.exists(cand):
        return cand
    sys.exit('zig not found: set ZIG=<path to zig.exe>')

def find_qemu():
    if os.environ.get('QEMU'):
        return os.environ['QEMU']
    cand = os.path.join(OS, '..', 'tools', 'qemu-pkg', 'qemu-system-x86_64.exe')
    if os.path.exists(cand):
        return cand
    sys.exit('qemu-system-x86_64 not found: set QEMU=<path>')

ZIG, QEMU = find_zig(), find_qemu()
CC   = [ZIG, 'cc', '-target', 'x86_64-freestanding', '-ffreestanding',
        '-fno-pic', '-fno-pie', '-fno-stack-protector']
CC32 = [ZIG, 'cc', '-target', 'x86-freestanding', '-ffreestanding',
        '-fno-pic', '-fno-pie', '-fno-stack-protector']

def sh(cmd):
    print('$', ' '.join(str(c) for c in cmd))
    subprocess.run(cmd, check=True)

def compile_obj(out, defines, cc=None):
    sh((cc or CC) + ['-D' + d for d in defines] +
       ['-c', os.path.join(HERE, 'spike64.S'), '-o', out])

def link(out, obj, delta, elf32=False):
    tpl = open(os.path.join(HERE, 'spike64.ld')).read()
    if elf32:
        tpl = tpl.replace('elf64-x86-64', 'elf32-i386')
    gen = os.path.join(OUT, 'spike_delta_%d%s.ld' % (delta, '_32' if elf32 else ''))
    with open(gen, 'w') as f:
        f.write(tpl.replace('@DELTA@', str(delta)))
    sh((CC32 if elf32 else CC) + ['-nostdlib', '-static',
                                  '-Wl,-T,' + gen,
                                  '-Wl,--build-id=none',
                                  '-o', out, obj])

def probe_layout(path):
    """打印程序头，返回 vaddr==0x100000 的段（.skernel）的文件偏移 H。"""
    data = open(path, 'rb').read()
    assert data[:5] == b'\x7fELF\x02', 'not an ELF64 file'
    e_phoff, = struct.unpack_from('<Q', data, 0x20)
    e_shoff, = struct.unpack_from('<Q', data, 0x28)
    e_phentsize, e_phnum = struct.unpack_from('<HH', data, 0x36)
    e_shentsize, e_shnum = struct.unpack_from('<HH', data, 0x3A)
    for i in range(e_phnum):
        p_type, p_flags, p_offset, p_vaddr = \
            struct.unpack_from('<IIQQ', data, e_phoff + i * e_phentsize)
        if p_type == 1:      # PT_LOAD
            print('  LOAD off=0x%-6x vaddr=0x%-8x' % (p_offset, p_vaddr))
    for i in range(e_shnum):
        sh_flags, sh_addr, sh_offset = \
            struct.unpack_from('<QQQ', data, e_shoff + i * e_shentsize + 8)
        if sh_addr == 0x100000 and (sh_flags & 0x2):   # SHF_ALLOC
            return sh_offset
    sys.exit('cannot locate .skernel (vaddr 0x100000)')

def run_qemu(tag, img):
    ser = os.path.join(OUT, 'serial_%s.log' % tag)
    cmd = [QEMU, '-kernel', img, '-m', '128M', '-display', 'none',
           '-serial', 'file:' + ser, '-device', 'isa-debug-exit', '-no-reboot']
    print('$', ' '.join(cmd))
    try:
        r = subprocess.run(cmd, timeout=30,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        rc, qout = r.returncode, r.stdout.decode('utf-8', 'replace').strip()
    except subprocess.TimeoutExpired:
        rc, qout = None, 'TIMEOUT after 30s'
    out = open(ser, 'rb').read() if os.path.exists(ser) else b''
    m = re.search(rb'2BADB00(.)K', out)
    magic = ('0x2BADB00' + m.group(1).decode()) if m else None
    ok = (rc == 103) and (magic is not None)
    print('  EAX/magic: %s' % magic)
    print('  serial : %r' % out)
    if qout:
        print('  qemu   : %s' % qout)
    print('  exit=%s  ->  %s' % (rc, 'PASS' if ok else 'FAIL'))
    return ok

def main():
    os.makedirs(OUT, exist_ok=True)
    obj_kl  = os.path.join(OUT, 'spike64.o')
    obj_nkl = os.path.join(OUT, 'spike64_nkl.o')
    compile_obj(obj_kl, [])
    compile_obj(obj_nkl, ['SPIKE_NO_KLUDGE'])

    # 第一遍链接（delta=0）＝ flat-bin 变体；顺带实测 .skernel 文件偏移 H
    elf_bin = os.path.join(OUT, 'spike64_bin.elf')
    link(elf_bin, obj_kl, 0)
    print('pass-1 layout (also the flat-bin variant):')
    H = probe_layout(elf_bin)
    print('  .skernel file offset H = 0x%x  ->  ELF64 load_addr = 0x%x' % (H, 0x100000 - H))

    img_bin = os.path.join(OUT, 'spike64.bin')
    sh([ZIG, 'objcopy', '-O', 'binary', elf_bin, img_bin])

    img_elf = os.path.join(OUT, 'spike64.elf')
    link(img_elf, obj_kl, H)

    img_nkl = os.path.join(OUT, 'spike64_nkl.elf')
    link(img_nkl, obj_nkl, 0)

    # 对照组：ELF32 经典 multiboot 路径（现有 kernel.elf 同款）
    obj_32  = os.path.join(OUT, 'spike32.o')
    compile_obj(obj_32, ['SPIKE_NO_KLUDGE'], cc=CC32)
    img_32  = os.path.join(OUT, 'spike32.elf')
    link(img_32, obj_32, 0, elf32=True)

    print()
    res = {}
    for tag, img, desc in [
        ('bin', img_bin, 'flat-bin + a.out-kludge'),
        ('elf', img_elf, 'ELF64 + a.out-kludge'),
        ('nkl', img_nkl, 'ELF64 无 kludge 位（对照组）'),
        ('mb1', img_32,  'ELF32 经典 multiboot 路径（对照组）'),
    ]:
        print('---- [%s] %s ----' % (tag, desc))
        res[tag] = run_qemu(tag, img)
        print()

    print('==== 结论 ====')
    if res['elf']:
        print('方案 A 定版：QEMU -kernel 可直接引导 ELF64 + a.out-kludge，无需 GRUB/ISO。')
    elif res['bin']:
        print('方案 A 定版：QEMU -kernel 不吃 ELF64 文件本体，但接受 objcopy -O binary '
              '产物 + kludge 头（build.py 交付 .bin 即可），无需 GRUB/ISO。')
    else:
        print('方案 A 不可行（两变体均失败）-> 转 方案 B：Multiboot 2 + grub-mkrescue ISO。')
    if res['nkl']:
        print('额外发现：QEMU 的 multiboot ELF 路径可直接接受 ELF64（kludge 非必需）。')
    print('（EAX magic 观测值见上方各变体输出；boot.S 应同时接受 0x2BADB002/003）')
    sys.exit(0 if (res['elf'] or res['bin']) else 1)

if __name__ == '__main__':
    main()
