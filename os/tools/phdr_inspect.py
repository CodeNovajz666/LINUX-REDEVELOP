#!/usr/bin/env python3
"""Check program headers: is the .got slot inside a PT_LOAD p_filesz range,
and where does the kernel image end vs PMM_START?"""
import struct

ELF = r'd:\Projects\LINUX-REDEVELOP\os\build\kernel.elf'
with open(ELF, 'rb') as f:
    data = f.read()

e_phoff, = struct.unpack_from('<Q', data, 0x20)
e_phentsize, e_phnum = struct.unpack_from('<HH', data, 0x36)

GOT_SLOT_VA = 0x13d930
print('== program headers ==')
max_end = 0
for i in range(e_phnum):
    off = e_phoff + i * e_phentsize
    p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = \
        struct.unpack_from('<IIQQQQQQ', data, off)
    if p_type != 1:
        print(f'  type={p_type} (skipped)')
        continue
    covers = p_vaddr <= GOT_SLOT_VA < p_vaddr + p_memsz
    in_filesz = p_vaddr <= GOT_SLOT_VA < p_vaddr + p_filesz
    print(f'  LOAD off={p_offset:#x} va={p_vaddr:#x} filesz={p_filesz:#x} '
          f'memsz={p_memsz:#x} va_end={p_vaddr + p_memsz:#x}')
    if covers:
        print(f'    -> covers GOT slot: memsz={covers} filesz={in_filesz}'
              + ('  *** SLOT NOT IN filesz: CONTENT NOT LOADED! ***' if not in_filesz else ''))
    max_end = max(max_end, p_vaddr + p_filesz)

print(f'\nkernel image physical end (va+filesz max): {max_end:#x}')

# section headers: which sections are between max filesz end and memsz end
e_shoff, = struct.unpack_from('<Q', data, 0x28)
e_shentsize, e_shnum, e_shstrndx = struct.unpack_from('<HHH', data, 0x3A)
shstr = None
sections = []
for i in range(e_shnum):
    off = e_shoff + i * e_shentsize
    vals = struct.unpack_from('<IIQQQQIIQQ', data, off)
    sections.append(vals)
shstr_off = sections[e_shstrndx][4]
def sname(s):
    end = data.index(b'\0', shstr_off + s[0])
    return data[shstr_off + s[0]:end].decode()

print('\n== sections in the tail region (beyond filesz or in bss) ==')
for s in sections:
    name, stype, flags, addr, offset, size = s[0], s[1], s[2], s[3], s[4], s[5]
    if stype == 8 or addr == 0:
        continue
    n = sname(s)
    if '.got' in n or 'bss' in n or 'data' in n or 'rodata' in n or 'text' in n:
        print(f'  {n:<12} addr={addr:#x} off={offset:#x} size={size:#x} '
              f'end={addr + size:#x}')
