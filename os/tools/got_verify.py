#!/usr/bin/env python3
"""Verify the ff15 GOT call in the rebuilt kernel points into the loaded range."""
import struct

ELF = r'd:\Projects\LINUX-REDEVELOP\os\build\kernel.elf'
with open(ELF, 'rb') as f:
    data = f.read()

e_shoff, = struct.unpack_from('<Q', data, 0x28)
e_shentsize, e_shnum, e_shstrndx = struct.unpack_from('<HHH', data, 0x3A)
sections = []
for i in range(e_shnum):
    off = e_shoff + i * e_shentsize
    name, stype, flags, addr, offset, size, link, info, align, entsize = \
        struct.unpack_from('<IIQQQQIIQQ', data, off)
    sections.append(dict(name=name, type=stype, addr=addr, offset=offset, size=size))

def addr_to_off(addr):
    for s in sections:
        if s['type'] != 8 and s['addr'] <= addr < s['addr'] + s['size']:
            return s['offset'] + (addr - s['addr'])
    return None

# symbol lookup
syms = []
for s in sections:
    if s['type'] == 2:
        strtab = sections[s['link']]
        for i in range(s['size'] // s['entsize']):
            off = s['offset'] + i * s['entsize']
            nm, info, other, shndx, value, size = \
                struct.unpack_from('<IBBHQQ', data, off)
            end = data.index(b'\0', strtab['offset'] + nm)
            name = data[strtab['offset'] + nm:end].decode(errors='replace')
            if name:
                syms.append((value, size, name))
lookup = {n: (v, sz) for v, sz, n in syms}

v, sz = lookup['paging_cow_fault']
off = addr_to_off(v)
code = data[off:off + sz]
print(f'paging_cow_fault @ {v:#x} ({sz} bytes)')
for i in range(len(code) - 5):
    if code[i] == 0xE8:
        rel, = struct.unpack_from('<i', code, i + 1)
        dst = v + i + 5 + rel
        label = next((n for x, s, n in syms if x == dst), '?')
        print(f'  {v + i:#x}: call {dst:#x} ({label})')
    elif code[i] == 0xFF and (code[i + 1] >> 3) & 7 == 2 and (code[i + 1] >> 6) == 0:
        disp, = struct.unpack_from('<i', code, i + 2)
        slot = v + i + 6 + disp
        slot_off = addr_to_off(slot)
        val, = struct.unpack_from('<Q', data, slot_off)
        label = next((n for x, s, n in syms if x == val), '?')
        # first PT_LOAD end = kludge load_end
        e_phoff, = struct.unpack_from('<Q', data, 0x20)
        e_phentsize, e_phnum = struct.unpack_from('<HH', data, 0x36)
        load_end = 0
        for j in range(e_phnum):
            o = e_phoff + j * e_phentsize
            p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = \
                struct.unpack_from('<IIQQQQQQ', data, o)
            if p_type == 1 and p_filesz:
                load_end = max(load_end, p_vaddr + p_filesz)
        covered = slot < load_end
        print(f'  {v + i:#x}: call [{slot:#x}] -> {val:#x} ({label}) '
              f'{"LOADED ok" if covered else "*** NOT LOADED ***"} '
              f'(load_end={load_end:#x})')
