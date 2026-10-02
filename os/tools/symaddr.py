import struct, sys

path = r"D:\Projects\LINUX-REDEVELOP\os\build\kernel.elf"
targets = [int(x, 16) for x in sys.argv[1:]] if len(sys.argv) > 1 else [0x103c13, 0x1021e2, 0x10da80, 0x18b778]

data = open(path, "rb").read()
assert data[:4] == b"\x7fELF"
e_shoff, = struct.unpack_from("<Q", data, 0x28)
e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 0x3A)

sections = []
for i in range(e_shnum):
    off = e_shoff + i * e_shentsize
    name, stype, flags, addr, offset, size, link, info, align, entsize = struct.unpack_from("<IIQQQQIIQQ", data, off)
    sections.append(dict(name=name, type=stype, addr=addr, offset=offset, size=size, link=link, entsize=entsize))

shstr = sections[e_shstrndx]
def sname(s):
    o = shstr["offset"] + s["name"]
    end = data.index(b"\0", o)
    return data[o:end].decode()

syms = []
for s in sections:
    if s["type"] == 2:  # SHT_SYMTAB
        strtab = sections[s["link"]]
        n = s["size"] // s["entsize"]
        for i in range(n):
            off = s["offset"] + i * s["entsize"]
            st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from("<IBBHQQ", data, off)
            if st_name == 0 or st_value == 0:
                continue
            o = strtab["offset"] + st_name
            end = data.index(b"\0", o)
            nm = data[o:end].decode()
            if nm:
                syms.append((st_value, st_size, nm))

syms.sort()
print(f"{len(syms)} symbols, base=0x{syms[0][0]:x}")

for t in targets:
    best = None
    for v, sz, nm in syms:
        if v <= t:
            best = (v, sz, nm)
        else:
            break
    if best:
        v, sz, nm = best
        delta = t - v
        inside = "inside" if delta < max(sz, 1) else "AFTER(+%d past end)" % (delta - sz)
        print(f"0x{t:x}: {nm}+0x{delta:x} (sym 0x{v:x} size {sz}) {inside}")
    else:
        print(f"0x{t:x}: no symbol found")
