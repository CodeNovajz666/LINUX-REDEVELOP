#!/usr/bin/env python3
"""Inspect kernel.elf: dump symbols of interest and raw bytes around calls
in the Rust paging module to diagnose the jump-to-address-1 fault."""
import struct, sys

ELF = r'd:\Projects\LINUX-REDEVELOP\os\build\kernel.elf'

with open(ELF, 'rb') as f:
    data = f.read()

# parse ELF64 header
e_shoff, = struct.unpack_from('<Q', data, 0x28)
e_shentsize, e_shnum, e_shstrndx = struct.unpack_from('<HHH', data, 0x3A)

sections = []
for i in range(e_shnum):
    off = e_shoff + i * e_shentsize
    name, stype, flags, addr, offset, size, link, info, align, entsize = \
        struct.unpack_from('<IIQQQQIIQQ', data, off)
    sections.append(dict(name=name, type=stype, addr=addr, offset=offset,
                         size=size, link=link, entsize=entsize))

# section name string table
shstr = sections[e_shstrndx]
def sec_name(s):
    end = data.index(b'\0', shstr['offset'] + s['name'])
    return data[shstr['offset'] + s['name']:end].decode()

for s in sections:
    s['sname'] = sec_name(s)

# symbol table
syms = []
for s in sections:
    if s['type'] == 2:  # SHT_SYMTAB
        strtab = sections[s['link']]
        for i in range(s['size'] // s['entsize']):
            off = s['offset'] + i * s['entsize']
            nm, info, other, shndx, value, size = \
                struct.unpack_from('<IBBHQQ', data, off)
            end = data.index(b'\0', strtab['offset'] + nm)
            name = data[strtab['offset'] + nm:end].decode(errors='replace')
            if name:
                syms.append((value, size, name))

syms.sort()
lookup = {n: (v, sz) for v, sz, n in syms}

def find(name_sub):
    return [(v, sz, n) for v, sz, n in syms if name_sub in n]

print('== paging/memcpy symbols ==')
for v, sz, n in find('paging') + find('memcpy') + find('memset'):
    print(f'  {v:#010x} size={sz:<6} {n}')

# addr -> file offset via sections
def addr_to_off(addr):
    for s in sections:
        if s['type'] != 8 and s['addr'] <= addr < s['addr'] + s['size']:
            return s['offset'] + (addr - s['addr'])
    return None

# dump the bytes of paging_cow_fault
for target in ('paging_cow_fault', 'paging_dup_user_dir'):
    if target not in lookup:
        print(f'!! {target} not found')
        continue
    v, sz = lookup[target]
    off = addr_to_off(v)
    print(f'\n== {target} @ {v:#x} ({sz} bytes) ==')
    code = data[off:off + sz]
    # find call instructions: e8 rel32, ff /2 (ff d0-r7, ff 10-17, ff 50+ /2)
    for i in range(len(code) - 5):
        b = code[i]
        va = v + i
        if b == 0xE8:
            rel, = struct.unpack_from('<i', code, i + 1)
            dst = va + 5 + rel
            label = next((n for x, s, n in syms if x == dst), '?')
            print(f'  {va:#x}: e8 call rel32 -> {dst:#x}  ({label})')
        elif b == 0xFF:
            mod = code[i + 1]
            if (mod >> 3) & 7 == 2:  # /2 = call
                print(f'  {va:#x}: ff {mod:02x} indirect call (modrm)')
