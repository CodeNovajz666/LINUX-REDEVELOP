#!/usr/bin/env python3
"""Minimal ELF32 symbol lookup: print the function containing a given address."""
import struct, sys

def load_symbols(path):
    data = open(path, 'rb').read()
    if data[:4] != b'\x7fELF' or data[4] != 1:
        sys.exit('not an ELF32 file')
    e_shoff, = struct.unpack_from('<I', data, 0x20)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from('<HHH', data, 0x2E)
    sections = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        name, stype, flags, addr, offset, size, link, info, align, entsize = \
            struct.unpack_from('<IIIIIIIIHH', data, off)
        sections.append((stype, addr, offset, size, link, entsize))
    syms = []
    for stype, addr, offset, size, link, entsize in sections:
        if stype != 2 or link >= len(sections):  # SHT_SYMTAB
            continue
        if entsize == 0:
            entsize = 16
        strtab = sections[link]
        str_off, str_size = strtab[2], strtab[3]
        for i in range(size // entsize):
            so = offset + i * entsize
            st_name, st_value, st_size, st_info, st_other, st_shndx = \
                struct.unpack_from('<IIIBBH', data, so)
            if st_info & 0xF != 2:  # STT_FUNC
                continue
            end = data.index(b'\0', str_off + st_name)
            name = data[str_off + st_name:end].decode('ascii', 'replace')
            if st_value:
                syms.append((st_value, st_value + (st_size or 0x1000), name))
    syms.sort()
    return syms

def main():
    path, target = sys.argv[1], int(sys.argv[2], 16)
    syms = load_symbols(path)
    hit = None
    for lo, hi, name in syms:
        if lo <= target < hi:
            hit = (lo, hi, name)
            break
    if hit:
        print(f'0x{target:08x} is in {hit[2]} (0x{hit[0]:08x}-0x{hit[1]:08x}, +0x{target-hit[0]:x})')
    else:
        below = [(lo, hi, n) for lo, hi, n in syms if lo <= target]
        if below:
            lo, hi, n = below[-1]
            print(f'0x{target:08x} is after {n} (0x{lo:08x}, +0x{target-lo:x}); nearest above:')
        for lo, hi, n in syms:
            if lo > target:
                print(f'  next symbol: {n} @ 0x{lo:08x}')
                break

if __name__ == '__main__':
    main()
