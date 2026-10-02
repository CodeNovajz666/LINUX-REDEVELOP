import struct, sys

path = r"D:\Projects\LINUX-REDEVELOP\os\build\kernel.elf"

data = open(path, "rb").read()
e_shoff, = struct.unpack_from("<Q", data, 0x28)
e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 0x3A)

sections = []
for i in range(e_shnum):
    off = e_shoff + i * e_shentsize
    name, stype, flags, addr, offset, size, link, info, align, entsize = struct.unpack_from("<IIQQQQIIQQ", data, off)
    sections.append(dict(type=stype, addr=addr, offset=offset, size=size))

def va_to_off(va):
    for s in sections:
        if s["type"] == 1 and s["addr"] != 0 and s["addr"] <= va < s["addr"] + s["size"]:
            return s["offset"] + (va - s["addr"]), s
    return None, None

def dump(va, before=48, after=8):
    o, s = va_to_off(va)
    if o is None:
        print(f"0x{va:x}: not mapped in any PROGBITS section")
        return
    start = o - before
    print(f"== bytes around 0x{va:x} (section vaddr 0x{s['addr']:x}) ==")
    for i in range(0, before + after, 16):
        addr = va - before + i
        chunk = data[start + i:start + i + 16]
        hexs = " ".join(f"{b:02x}" for b in chunk)
        marker = " <-- FAULT" if addr <= va < addr + 16 else ""
        print(f"  0x{addr:08x}: {hexs}{marker}")

for a in [int(x, 16) for x in sys.argv[1:]]:
    dump(a)
