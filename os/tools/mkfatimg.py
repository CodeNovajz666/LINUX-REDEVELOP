#!/usr/bin/env python3
"""Build the Novix boot disk: an MBR with one FAT32 partition holding /bin.

Read (and written) by kernel/fs/fat32.c -- no host formatting tool is
involved, so image and driver agree by construction.

Layout (v0.8.0):
    LBA 0        MBR, partition 1: type 0x0C (FAT32 LBA), start LBA 2048
    LBA 2048     FAT32 boot sector (512B sectors, 512B clusters)
    LBA 2049     FSInfo sector
    LBA 2054     backup boot sector
    LBA 2080     FAT #0 and #1 (mirrored; the kernel writes both)
    LBA 4096     cluster 2 = root directory, then file data

Every file gets an LFN entry so its lowercase name survives verbatim;
the 8.3 alias is generated with ~N deduplication when the name does not
fit ("fault_div" -> "FAULT_~1").

v0.9.0: the image also carries /etc (passwd, shadow, inittab, issue),
/root, /home and /var/run directories.

v1.0.0: the OUT-OF-BOX image ships ZERO accounts -- /etc/passwd is empty
and /home is empty, so the first boot runs the registration wizard
(getty detects the empty passwd). /etc/shadow holds only a shadow-only
"kck" anchor entry: its build-time digest is what the shadow_roundtrip
ktest verifies against the kernel's own SHA-256, and it is not an
account, so it never triggers (nor blocks) registration. Account
credentials and home directories are created at runtime by /bin/register
and useradd -- nothing is pre-generated, and there is no password ledger
to keep.

Usage: mkfatimg.py out.img [/path/name=srcfile ...]
       a pair "dir/" with no '=' creates an empty directory
A /novix.txt marker file is always added.
"""
import sys, os, struct

IMG_SECTORS = 131072            # 64 MiB disk
PART_LBA = 2048                 # 1 MiB in, 1-MiB aligned partition
PART_SECTORS = IMG_SECTORS - PART_LBA
SEC = 512
SPC = 1                         # sectors per cluster: 512B clusters
RSVD = 32
NFATS = 2
ATTR_DIR = 0x10
ATTR_ARC = 0x20

def fat_size():
    """sectors per FAT so every cluster number fits in both FAT copies"""
    fatsz = 1
    while True:
        clusters = PART_SECTORS - RSVD - NFATS * fatsz
        if (clusters + 2) * 4 <= fatsz * SEC:
            return fatsz
        fatsz += 1

FATSZ = fat_size()
CLUS_BYTES = SEC * SPC
DATA_LBA = PART_LBA + RSVD + NFATS * FATSZ
NCLUSTERS = (PART_SECTORS - RSVD - NFATS * FATSZ) // SPC
assert NCLUSTERS >= 65525, 'volume too small to be FAT32 (need >= 65525 clusters)'

# ---------- name handling ----------

def sname_legal(c):
    return c.isalnum() or c in "_$%'-_@~`!(){}^#&"

def lfn_checksum(alias11):
    s = 0
    for c in alias11:
        s = (((s & 1) << 7) + (s >> 1) + c) & 0xFF
    return s

def alias_for(name, used):
    """8.3 alias for a display name; ~N dedup against `used` (a set)"""
    up = name.upper()
    if '.' in up:
        base, _, ext = up.rpartition('.')
    else:
        base, ext = up, ''
    base = ''.join(c for c in base if sname_legal(c))
    ext = ''.join(c for c in ext if sname_legal(c))
    if len(base) > 8 or len(ext) > 3:
        base = base[:6]
    alias = (base.ljust(8) + ext.ljust(3)).encode()
    if alias in used:
        for n in range(1, 100):
            alias = (base[:6] + f'~{n}').ljust(8).encode()[:8] + ext.ljust(3).encode()
            if alias not in used:
                break
    used.add(alias)
    return alias

LFN_OFFS = [1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30]

def lfn_entry_count(name):
    return (len(name) + 12) // 13

def lfn_entries(name, alias11):
    """LFN entries, physically reversed (last logical part, flagged 0x40, first)"""
    parts = [name[i:i + 13] for i in range(0, len(name), 13)]
    n = len(parts)
    ck = lfn_checksum(alias11)
    out = []
    for phys in range(n - 1, -1, -1):
        seq = phys + 1
        e = bytearray(32)
        e[0] = seq | (0x40 if phys == n - 1 else 0)
        e[11] = 0x0F
        e[12] = 0
        e[13] = ck
        chars = [ord(c) for c in parts[phys]]
        if len(chars) < 13:
            chars.append(0)                    # terminator
        while len(chars) < 13:
            chars.append(0xFFFF)               # pad
        for i, ch in enumerate(chars):
            struct.pack_into('<H', e, LFN_OFFS[i], ch)
        out.append(bytes(e))
    return out

def dirent(alias11, attr, first_clus, size):
    e = bytearray(32)
    e[0:11] = alias11
    e[11] = attr
    struct.pack_into('<H', e, 20, first_clus >> 16)
    struct.pack_into('<H', e, 26, first_clus & 0xFFFF)
    struct.pack_into('<I', e, 28, size)
    return bytes(e)

# ---------- in-memory volume ----------

class Node:
    def __init__(self, name, is_dir):
        self.name = name
        self.is_dir = is_dir
        self.data = b''
        self.children = []
        self.first = 0

class FatVolume:
    def __init__(self):
        self.fat = [0] * (NCLUSTERS + 2)
        self.fat[0] = 0x0FFFFFF8
        self.fat[1] = 0x0FFFFFFF
        self.next_free = 2
        self.regions = []          # (byte offset in image, bytes)

    def alloc_chain(self, n):
        assert self.next_free + n <= NCLUSTERS + 2, 'image full'
        first = self.next_free
        c = first
        for _ in range(n - 1):
            self.fat[c] = c + 1
            c += 1
        self.fat[c] = 0x0FFFFFFF
        self.next_free = c + 1
        return first

    def put(self, cluster, data):
        assert len(data) <= CLUS_BYTES
        self.regions.append(((DATA_LBA + (cluster - 2) * SPC) * SEC, data))

def place(node, vol, is_root):
    """allocate cluster chains pre-order so children can point at their parent"""
    if not node.is_dir:
        nclus = max(1, (len(node.data) + CLUS_BYTES - 1) // CLUS_BYTES)
        node.first = vol.alloc_chain(nclus)
        return
    # entries: children (LFN + 8.3 each) plus "." ".." for non-root dirs
    nent = sum(1 + lfn_entry_count(c.name) for c in node.children)
    if not is_root:
        nent += 2
    nclus = (nent * 32 + CLUS_BYTES - 1) // CLUS_BYTES
    node.first = vol.alloc_chain(nclus)
    for c in node.children:
        place(c, vol, False)

def fill(node, vol, parent_first, used):
    """write directory content + file data (children are placed already)"""
    if not node.is_dir:
        nclus = max(1, (len(node.data) + CLUS_BYTES - 1) // CLUS_BYTES)
        data = node.data + b'\x00' * (nclus * CLUS_BYTES - len(node.data))
        for i in range(nclus):
            vol.put(node.first + i, data[i * CLUS_BYTES:(i + 1) * CLUS_BYTES])
        return
    blob = bytearray()
    if parent_first is not None:               # non-root dirs start with "." ".."
        blob += dirent(b'.          ', ATTR_DIR, node.first, 0)
        blob += dirent(b'..         ', ATTR_DIR, parent_first, 0)
    for c in node.children:
        alias = alias_for(c.name, used)
        if needs_lfn(c.name, alias):
            blob += b''.join(lfn_entries(c.name, alias))
        blob += dirent(alias, ATTR_DIR if c.is_dir else ATTR_ARC,
                       c.first, 0 if c.is_dir else len(c.data))
    chain = []
    c = node.first
    while True:
        chain.append(c)
        if vol.fat[c] == 0x0FFFFFFF:
            break
        c = vol.fat[c]
    blob += b'\x00' * (len(chain) * CLUS_BYTES - len(blob))
    for i, clus in enumerate(chain):
        vol.put(clus, blob[i * CLUS_BYTES:(i + 1) * CLUS_BYTES])
    for c in node.children:
        fill(c, vol, node.first, used)

def needs_lfn(name, alias11):
    """LFN is required whenever the display name differs from the alias.
    Aliases are always uppercase, so a lowercase name needs an LFN to
    survive verbatim -- compare case-sensitively."""
    base = alias11[0:8].decode().rstrip()
    ext = alias11[8:11].decode().rstrip()
    alias_disp = base + ('.' + ext if ext else '')
    return name != alias_disp

def boot_sector():
    bs = bytearray(SEC)
    bs[0:3] = b'\xeb\x58\x90'
    bs[3:11] = b'MSWIN4.1'
    struct.pack_into('<H', bs, 11, SEC)
    bs[13] = SPC
    struct.pack_into('<H', bs, 14, RSVD)
    bs[16] = NFATS
    struct.pack_into('<H', bs, 17, 0)              # root_ent_cnt (FAT32: 0)
    struct.pack_into('<H', bs, 19, 0)              # tot_sec16 (FAT32: 0)
    bs[21] = 0xF8                                   # media: hard disk
    struct.pack_into('<H', bs, 22, 0)              # fatsz16 (FAT32: 0)
    struct.pack_into('<H', bs, 24, 63)             # sectors per track
    struct.pack_into('<H', bs, 26, 255)            # heads
    struct.pack_into('<I', bs, 28, PART_LBA)       # hidden sectors
    struct.pack_into('<I', bs, 32, PART_SECTORS)   # total sectors
    struct.pack_into('<I', bs, 36, FATSZ)          # sectors per FAT
    struct.pack_into('<H', bs, 40, 0)              # ext flags: both FATs active
    struct.pack_into('<H', bs, 42, 0)              # fs version
    struct.pack_into('<I', bs, 44, 2)              # root cluster
    struct.pack_into('<H', bs, 48, 1)              # FSInfo sector
    struct.pack_into('<H', bs, 50, 6)              # backup boot sector
    bs[64] = 0x80                                   # drive number (BIOS)
    bs[66] = 0x29                                   # extended boot signature
    struct.pack_into('<I', bs, 67, 0x4E4F5649)     # volume id
    bs[71:82] = b'NOVIX     '                       # volume label
    bs[82:90] = b'FAT32   '                         # fs type
    bs[510:512] = b'\x55\xaa'
    return bs

def etc_files():
    """v1.0.0 /etc content for the OUT-OF-BOX image: ZERO accounts. passwd
    ships empty and shadow holds only the "kck" build-time anchor (a
    shadow-only line is not an account -- getty's empty-passwd check runs
    the registration wizard instead of login). /home ships empty too:
    register and useradd create home directories at runtime. Returns the
    (path, bytes) pairs."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from mkpasswd import shadow_line

    passwd = ""                        # no accounts: first boot registers
    # kck: shadow-only anchor (no passwd entry, never a login target).
    # passwd rewrites only the target user's line, so kck's digest stays at
    # its build-time value on every boot of every image generation -- the
    # shadow_roundtrip ktest verifies the host-tool vs kernel hash contract
    # against it even when registered passwords were changed at runtime.
    shadow = shadow_line('kck', 'ktest-pass') + "\n"
    inittab = "tty1:getty\n"
    issue = "Novix 1.2.0 (tty1) -- authorized users only\n"
    # the user-space init script (/bin/init reads it; the first
    # "respawn" line is the supervised tty service) and the post-login
    # banner login(1) prints before exec'ing the shell.
    init = (
        "# Novix 1.2.0 init script: one-shots first (in order), then the\n"
        "# respawn service is supervised forever.\n"
        "respawn /bin/getty\n"
    )
    motd = (
        "Novix 1.2.0 -- a real OS, grown out of a kernel module\n"
        "osh: pipelines a|b, redirects a>b a>>b a<b, jobs a&, $VAR $?\n"
    )

    pairs = [
        ('/etc/passwd', passwd.encode()),
        ('/etc/shadow', shadow.encode()),
        ('/etc/inittab', inittab.encode()),
        ('/etc/issue', issue.encode()),
        ('/etc/init', init.encode()),
        ('/etc/motd', motd.encode()),
        ('/root/', None),                  # root's home
        ('/home/', None),                  # empty: register/useradd fill it
        ('/var/run/', None),               # utmp lives here (who/login)
    ]
    return pairs


def emit(files, out_path):
    root = Node('', True)
    bin_node = Node('bin', True)
    root.children.append(bin_node)

    marker = Node('novix.txt', False)
    marker.data = (b'Novix FAT32 boot disk.\n'
                   b'Files written here survive poweroff.\n')
    root.children.append(marker)

    # v0.9.0 login world / v1.0.0 zero-account out-of-box image
    etc_pairs = etc_files()
    files = list(files) + etc_pairs

    for path, src in files:
        parts = [p for p in path.split('/') if p]
        cur = root
        for p in parts[:-1]:
            nxt = next((c for c in cur.children if c.name == p and c.is_dir), None)
            if nxt is None:
                nxt = Node(p, True)
                cur.children.append(nxt)
            cur = nxt
        leaf = parts[-1]
        if src is None:                        # trailing '/' -> empty dir
            if not next((c for c in cur.children if c.name == leaf), None):
                cur.children.append(Node(leaf, True))
            continue
        node = Node(leaf, False)
        if isinstance(src, bytes):
            node.data = src
        else:
            with open(src, 'rb') as f:
                node.data = f.read()
        cur.children.append(node)

    vol = FatVolume()
    place(root, vol, True)
    fill(root, vol, None, set())

    regions = list(vol.regions)

    # MBR
    mbr = bytearray(SEC)
    pe = bytearray(16)
    pe[0] = 0x80                                   # bootable
    pe[1:4] = b'\xfe\xff\xff'                      # CHS start (max, unused)
    pe[4] = 0x0C                                   # FAT32 with LBA
    pe[5:8] = b'\xfe\xff\xff'                      # CHS end (max, unused)
    struct.pack_into('<II', pe, 8, PART_LBA, PART_SECTORS)
    mbr[446:462] = pe
    mbr[510:512] = b'\x55\xaa'
    regions.append((0, bytes(mbr)))

    # boot sector (+ backup) and FSInfo
    bs = boot_sector()
    regions.append((PART_LBA * SEC, bytes(bs)))
    regions.append(((PART_LBA + 6) * SEC, bytes(bs)))
    free = sum(1 for v in vol.fat[2:] if v == 0)
    fsi = bytearray(SEC)
    fsi[0:4] = b'RRaA'
    struct.pack_into('<I', fsi, 484, 0x61417272)
    struct.pack_into('<I', fsi, 488, free)
    struct.pack_into('<I', fsi, 492, min(vol.next_free, NCLUSTERS + 1))
    fsi[510:512] = b'\x55\xaa'
    regions.append(((PART_LBA + 1) * SEC, bytes(fsi)))

    # FATs (mirrored)
    fat = bytearray(FATSZ * SEC)
    for i, v in enumerate(vol.fat):
        struct.pack_into('<I', fat, i * 4, v)
    regions.append(((PART_LBA + RSVD) * SEC, bytes(fat)))
    regions.append(((PART_LBA + RSVD + FATSZ) * SEC, bytes(fat)))

    with open(out_path, 'wb') as f:
        f.truncate(IMG_SECTORS * SEC)
        for off, data in regions:
            f.seek(off)
            f.write(data)
    print(f'  fat image: {out_path} ({IMG_SECTORS * SEC >> 20} MiB, {len(files)} files, '
          f'{free}/{NCLUSTERS} clusters free, FAT {FATSZ} sectors)')

if __name__ == '__main__':
    args = sys.argv[1:]
    out_path = args[0]
    pairs = []
    for a in args[1:]:
        if '=' in a:
            path, src = a.split('=', 1)
            pairs.append((path, src))
        elif a.endswith('/'):
            pairs.append((a, None))         # empty directory
        else:
            src = a
            path = '/bin/' + os.path.splitext(os.path.basename(src))[0]
            pairs.append((path, src))
    emit(pairs, out_path)
