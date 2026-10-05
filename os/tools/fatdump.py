import struct

img = open(r'd:\Projects\LINUX-REDEVELOP\os\build\novix-hd.img', 'rb').read()
SEC = 512
PART_LBA = 2048
bs = img[PART_LBA * SEC:(PART_LBA + 1) * SEC]
spc = bs[13]
rsvd = struct.unpack_from('<H', bs, 14)[0]
nfats = bs[16]
fatsz = struct.unpack_from('<I', bs, 36)[0]
root_clus = struct.unpack_from('<I', bs, 44)[0]
data_lba = PART_LBA + rsvd + nfats * fatsz
print(f'spc={spc} rsvd={rsvd} nfats={nfats} fatsz={fatsz} '
      f'root_clus={root_clus} data_lba={data_lba}')
clus_bytes = spc * SEC
c = root_clus
while True:
    off = (data_lba + (c - 2) * spc) * SEC
    data = img[off:off + clus_bytes]
    stop = False
    for i in range(0, clus_bytes, 32):
        e = data[i:i + 32]
        if e[0] == 0:
            print('  END')
            stop = True
            break
        attr = e[11]
        if attr == 0x0F:
            parts = bytes(e[1:11] + e[14:26] + e[28:32])
            print(f'  LFN seq={e[0] & 0x1F} flag40={bool(e[0] & 0x40)} '
                  f'cksum={e[13]:02x} parts={parts!r}')
        else:
            sname = bytes(e[0:11])
            s = 0
            for ch in sname:
                s = (((s & 1) << 7) + (s >> 1) + ch) & 0xFF
            first = struct.unpack_from('<H', e, 26)[0] | \
                    (struct.unpack_from('<H', e, 20)[0] << 16)
            size = struct.unpack_from('<I', e, 28)[0]
            print(f'  DIRENT {sname!r} attr={attr:02x} scksum={s:02x} '
                  f'first={first} size={size}')
    if stop:
        break
    fat_off = (PART_LBA + rsvd) * SEC + c * 4
    nxt = struct.unpack_from('<I', img, fat_off)[0] & 0x0FFFFFFF
    if nxt >= 0x0FFFFFF8:
        break
    c = nxt
