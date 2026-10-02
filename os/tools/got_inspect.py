#!/usr/bin/env python3
"""Decode the ff15 GOT-indirect call in paging_cow_fault and check the GOT slot."""
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

CALL_VA = 0x109532
off = addr_to_off(CALL_VA)
ins = data[off:off + 6]
print('instruction bytes:', ins.hex(' '))
disp, = struct.unpack_from('<i', ins, 2)
rip = CALL_VA + 6
slot_va = rip + disp
print(f'call [rip+{disp:#x}]  -> rip={rip:#x}, GOT slot va={slot_va:#x}')

slot_off = addr_to_off(slot_va)
if slot_off is None:
    print('!! GOT slot address is not inside any loaded section')
else:
    val, = struct.unpack_from('<Q', data, slot_off)
    print(f'GOT slot file offset {slot_off:#x}, stored value: {val:#x}')

    # which section holds the slot?
    for s in sections:
        if s['type'] != 8 and s['addr'] <= slot_va < s['addr'] + s['size']:
            shstr = sections[e_shstrndx]
            end = data.index(b'\0', shstr['offset'] + s['name'])
            print('slot lives in section:', data[shstr['offset'] + s['name']:end].decode())

# also find relocations pointing at the slot (static link: should be none left)
print('\n-- relocations targeting the GOT slot --')
found = False
for s in sections:
    if s['type'] == 4 or s['type'] == 9:  # SHT_RELA / SHT_REL
        for i in range(s['size'] // s['entsize'] or 1):
            off2 = s['offset'] + i * s['entsize']
            r_offset, = struct.unpack_from('<Q', data, off2)
            if r_offset == slot_va:
                r_info, = struct.unpack_from('<Q', data, off2 + 8)
                found = True
                print(f'  reloc at {r_offset:#x} info={r_info:#x}')
if not found:
    print('  (none)')

# print symbol table around value 1 or the memcpy target
print('\n-- all symbols named like memcpy/memset --')
for s in sections:
    if s['type'] == 2:
        strtab = sections[s['link']]
        for i in range(s['size'] // s['entsize']):
            off2 = s['offset'] + i * s['entsize']
            nm, info, other, shndx, value, size = \
                struct.unpack_from('<IBBHQQ', data, off2)
            end = data.index(b'\0', strtab['offset'] + nm)
            name = data[strtab['offset'] + nm:end].decode(errors='replace')
            if 'memcpy' in name or 'memset' in name:
                print(f'  {value:#x} size={size} {name}')
