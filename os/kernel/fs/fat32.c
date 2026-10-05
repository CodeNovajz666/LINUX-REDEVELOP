#include "novix.h"
#include "fat32.h"
#include "ata.h"
#include "heap.h"
#include "spinlock.h"
#include "lib.h"
#include "console.h"

/*
 * FAT32 read/write filesystem on the ATA primary master (v0.8.0).
 *
 * Design notes (see also fat32.h and DESIGN.md section 7):
 *  - Inodes are synthesized: FAT has no on-disk inode, so every directory
 *    entry gets a cached kernel node keyed by (parent dir cluster, 8.3
 *    name). The cache hands out stable struct inode pointers, so two
 *    opens of the same path share one node (same deal as ramfs).
 *  - Everything is write-through: FAT sectors, directory entries and file
 *    clusters hit the platters before the operation returns. No dirty
 *    page cache, no ordering tricks -- "write, drop power, read back"
 *    is exactly what the power-cycle ktest proves via a second mount.
 *  - LFN is read-only: long names are parsed and matched
 *    case-insensitively, but files created here get plain uppercase 8.3
 *    short names.
 *  - Locking: one spinlock per superblock, irqsave variants (PIO polling
 *    happens under it). Lock order: fat32 lock > ata lock; nothing above
 *    fat32 may be held on entry from the fault path (inode_read_at takes
 *    it as a leaf, like ramfs does its node tree).
 */

#define FAT_FREE      0x00000000u
#define FAT_BAD       0x0FFFFFF7u
#define FAT_EOC_MIN   0x0FFFFFF8u
#define FAT_EOC       0x0FFFFFFFu
#define FAT_IS_EOC(v) ((v) >= FAT_EOC_MIN)

#define ATTR_RO     0x01
#define ATTR_SYS    0x04
#define ATTR_LFN    0x0F
#define ATTR_VOL    0x08
#define ATTR_DIR    0x10
#define ATTR_ARC    0x20
#define ATTR_MASK   0x3F

#define DIRENT_DEL  0xE5
#define DIRENT_END  0x00

#define FAT_NAME_MAX  63                        /* LFN slots we assemble */
#define LFN_MAX_PARTS 5                         /* ceil(63/13) */

struct fat32_sb {
    uint32_t part_lba;
    uint32_t fat_lba;                           /* first FAT copy */
    uint32_t data_lba;                          /* cluster 2 */
    uint32_t spc;                               /* sectors per cluster */
    uint32_t clus_bytes;
    uint32_t fat_secs;                          /* sectors per FAT */
    uint32_t cluster_count;                     /* data clusters */
    uint32_t root_clus;
    uint32_t free_clusters;
    uint32_t next_free;                         /* allocation hint */
    spinlock_t lock;
    uint32_t fat_cache_sec;                     /* cached sector, inside FAT #0 */
    bool fat_cache_valid;
    uint8_t fat_cache[ATA_SECTOR_SIZE];
    uint8_t *bounce;                            /* one cluster, scratch */
    struct fat32_node *nodes;                   /* inode cache list */
    struct fat32_node *root;
    uint32_t next_ino;
};

struct fat32_node {
    struct inode vfs;
    struct fat32_sb *sb;
    uint32_t first_clus;                        /* 0 = empty file */
    bool is_dir;
    uint32_t parent_clus;                       /* dir holding our dirent; 0 = root */
    uint8_t sname[11];                          /* raw 8.3 dirent name */
    uint32_t dirent_clus;                       /* cluster holding our dirent */
    uint32_t dirent_off;                        /* byte offset inside that cluster */
    struct fat32_node *next;
};

struct fat32_sb *g_fat_sb;

bool fat32_mounted(void) { return g_fat_sb != NULL; }
uint32_t fat32_free_clusters(struct fat32_sb *sb) { return sb ? sb->free_clusters : 0; }
uint32_t fat32_total_clusters(struct fat32_sb *sb) { return sb ? sb->cluster_count : 0; }

static struct fat32_node *node_of(struct inode *ino) { return ino->i_priv; }

/* ---- little-endian field access ---- */

static uint32_t rd16(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void st16(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static void st32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static bool clus_valid(struct fat32_sb *sb, uint32_t c) {
    return c >= 2 && c < 2 + sb->cluster_count;
}
static uint32_t clus_lba(struct fat32_sb *sb, uint32_t c) {
    return sb->data_lba + (c - 2) * sb->spc;
}

/* ---- FAT access (one-sector cache, write-through, both copies) ---- */

static int fat_entry(struct fat32_sb *sb, uint32_t c, uint32_t *out) {
    if (!clus_valid(sb, c)) return -1;
    uint32_t sec = (c * 4) / ATA_SECTOR_SIZE;
    if (!sb->fat_cache_valid || sb->fat_cache_sec != sec) {
        if (ata_read_sectors(sb->fat_lba + sec, 1, sb->fat_cache) != 0) return -1;
        sb->fat_cache_sec = sec;
        sb->fat_cache_valid = true;
    }
    *out = rd32(sb->fat_cache + (c * 4) % ATA_SECTOR_SIZE) & 0x0FFFFFFF;
    return 0;
}

static int fat_set(struct fat32_sb *sb, uint32_t c, uint32_t val) {
    if (!clus_valid(sb, c)) return -1;
    uint32_t sec = (c * 4) / ATA_SECTOR_SIZE;
    if (!sb->fat_cache_valid || sb->fat_cache_sec != sec) {
        if (ata_read_sectors(sb->fat_lba + sec, 1, sb->fat_cache) != 0) return -1;
        sb->fat_cache_sec = sec;
        sb->fat_cache_valid = true;
    }
    st32(sb->fat_cache + (c * 4) % ATA_SECTOR_SIZE, val & 0x0FFFFFFF);
    if (ata_write_sectors(sb->fat_lba + sec, 1, sb->fat_cache) != 0) return -1;
    return ata_write_sectors(sb->fat_lba + sb->fat_secs + sec, 1, sb->fat_cache);
}

static int clus_read(struct fat32_sb *sb, uint32_t c, uint8_t *buf) {
    return ata_read_sectors(clus_lba(sb, c), sb->spc, buf);
}
static int clus_write(struct fat32_sb *sb, uint32_t c, const uint8_t *buf) {
    return ata_write_sectors(clus_lba(sb, c), sb->spc, buf);
}

static uint32_t cluster_alloc(struct fat32_sb *sb) {
    if (sb->free_clusters == 0) return 0;
    uint32_t total = 2 + sb->cluster_count;
    uint32_t c = sb->next_free < 2 ? 2 : sb->next_free;
    for (uint32_t i = 0; i < sb->cluster_count; i++) {
        if (c >= total) c = 2;
        uint32_t v;
        if (fat_entry(sb, c, &v) != 0) return 0;
        if (v == FAT_FREE) {
            if (fat_set(sb, c, FAT_EOC) != 0) return 0;
            sb->free_clusters--;
            sb->next_free = c + 1;
            return c;
        }
        c++;
    }
    sb->free_clusters = 0;                      /* FAT disagrees with the count: trust the FAT */
    return 0;
}

static int chain_free(struct fat32_sb *sb, uint32_t first) {
    uint32_t c = first;
    uint32_t guard = 0;
    while (clus_valid(sb, c)) {
        uint32_t next;
        if (fat_entry(sb, c, &next) != 0) return -1;
        if (fat_set(sb, c, FAT_FREE) != 0) return -1;
        sb->free_clusters++;
        if (FAT_IS_EOC(next)) return 0;
        c = next;
        if (++guard > sb->cluster_count) return -1;  /* cycle: corrupt */
    }
    return 0;
}

/* cluster number at chain index (0 = first); 0 = walk failed */
static uint32_t cluster_at(struct fat32_sb *sb, uint32_t first, uint32_t idx) {
    uint32_t c = first;
    while (idx--) {
        uint32_t next;
        if (fat_entry(sb, c, &next) != 0) return 0;
        if (FAT_IS_EOC(next)) return 0;
        c = next;
    }
    return c;
}

/* ---- name helpers ---- */

static int ascii_lower(int c) {
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

/* FAT is case-insensitive, case-preserving (the LFN keeps the case) */
static bool fat_name_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (ascii_lower((unsigned char)*a) != ascii_lower((unsigned char)*b))
            return false;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static uint8_t sname_checksum(const uint8_t *sname) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++)
        sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + sname[i]);
    return sum;
}

/* "NAME    EXT   " -> "name.ext" (as stored, i.e. uppercase) */
static void sname_to_name(const uint8_t *raw, char *out) {
    int k = 0;
    for (int i = 0; i < 8 && raw[i] != ' '; i++) out[k++] = (char)raw[i];
    if (raw[8] != ' ') {
        out[k++] = '.';
        for (int i = 8; i < 11 && raw[i] != ' '; i++) out[k++] = (char)raw[i];
    }
    out[k] = 0;
}

static bool sname_legal_char(char c) {
    if (c >= 'a' && c <= 'z') return true;
    if (c >= 'A' && c <= 'Z') return true;
    if (c >= '0' && c <= '9') return true;
    return strchr(" $%'-_@~`!(){}^#&", c) != NULL;
}

static int name_to_sname(const char *name, uint8_t out[11]) {
    memset(out, ' ', 11);
    const char *dot = NULL;
    for (const char *p = name; *p; p++)
        if (*p == '.') dot = p;
    size_t blen = dot ? (size_t)(dot - name) : strlen(name);
    size_t elen = dot ? strlen(dot + 1) : 0;
    if (blen == 0 || blen > 8 || elen > 3) return -1;
    for (size_t i = 0; i < blen; i++) {
        char c = name[i];
        if (!sname_legal_char(c)) return -1;
        out[i] = (uint8_t)ascii_lower(c) - 32 >= 'A' && ascii_lower(c) - 32 <= 'Z'
                     ? (uint8_t)(c - 32) : (uint8_t)c;
    }
    for (size_t i = 0; i < elen; i++) {
        char c = dot[1 + i];
        if (!sname_legal_char(c)) return -1;
        out[8 + i] = (uint8_t)ascii_lower(c) - 32 >= 'A' && ascii_lower(c) - 32 <= 'Z'
                         ? (uint8_t)(c - 32) : (uint8_t)c;
    }
    if (out[0] == DIRENT_DEL) return -1;        /* 0xE5 means "deleted" */
    return 0;
}

static const int lfn_char_off[13] = { 1,3,5,7,9,14,16,18,20,22,24,28,30 };

/* ---- directory walking ---- */

struct dir_scan_entry {
    char name[FAT_NAME_MAX + 1];                /* display name (LFN if present) */
    uint8_t sname[11];
    uint32_t first_clus;
    uint32_t size;
    bool is_dir;
    uint32_t dirent_clus;
    uint32_t dirent_off;                        /* offset of the 8.3 dirent */
    uint32_t lfn_first_clus;                    /* first LFN entry of the group */
    uint32_t lfn_first_off;                     /* (== dirent location when no LFN) */
    bool has_lfn;
};

typedef int (*dir_walk_fn)(void *ctx, const struct dir_scan_entry *e);
/* return: 1 = stop (callback matched), 0 = keep going; walker returns the
 * callback's stop or -1 on I/O / corruption */

static int dir_walk(struct fat32_sb *sb, uint32_t dir_clus, dir_walk_fn fn,
                    void *ctx) {
    uint32_t c = dir_clus;
    uint32_t guard = 0;

    while (clus_valid(sb, c)) {
        if (clus_read(sb, c, sb->bounce) != 0) return -1;

        uint16_t lfn_buf[FAT_NAME_MAX];         /* UTF-16, indexed by 13*seq */
        uint8_t lfn_total = 0, lfn_cksum = 0;
        uint32_t lfn_clus = 0, lfn_off = 0;

        for (uint32_t off = 0; off + 32 <= sb->clus_bytes; off += 32) {
            const uint8_t *e = sb->bounce + off;

            if (e[0] == DIRENT_END) return 0;   /* logical end of directory */
            if (e[0] == DIRENT_DEL) {           /* deleted: breaks any LFN group */
                lfn_total = 0;
                continue;
            }
            uint8_t attr = e[11] & ATTR_MASK;

            if (attr == ATTR_LFN) {
                uint8_t seq = e[0] & 0x1F;
                if (e[0] & 0x40) {              /* last logical part comes first */
                    lfn_total = seq;
                    lfn_cksum = e[13];
                    lfn_clus = c;
                    lfn_off = off;
                    memset(lfn_buf, 0xFF, sizeof(lfn_buf));
                }
                if (lfn_total && seq >= 1 && seq <= LFN_MAX_PARTS && seq <= lfn_total) {
                    for (int i = 0; i < 13; i++) {
                        uint16_t ch = (uint16_t)(e[lfn_char_off[i]] |
                                                 (e[lfn_char_off[i] + 1] << 8));
                        if (13 * (seq - 1) + i < FAT_NAME_MAX)
                            lfn_buf[13 * (seq - 1) + i] = ch;
                    }
                }
                continue;
            }
            if (attr & ATTR_VOL) {              /* volume label: not a file */
                lfn_total = 0;
                continue;
            }
            if (e[0] == '.') {                  /* "." and ".." are VFS-built */
                lfn_total = 0;
                continue;
            }

            struct dir_scan_entry ent;
            memset(&ent, 0, sizeof(ent));
            memcpy(ent.sname, e, 11);
            ent.first_clus = (rd16(e + 20) << 16) | rd16(e + 26);
            ent.size = rd32(e + 28);
            ent.is_dir = (attr & ATTR_DIR) != 0;
            ent.dirent_clus = c;
            ent.dirent_off = off;
            ent.lfn_first_clus = lfn_total ? lfn_clus : c;
            ent.lfn_first_off = lfn_total ? lfn_off : off;
            ent.has_lfn = false;

            /* assemble the LFN when the group is complete and checksums agree */
            if (lfn_total && lfn_cksum == sname_checksum(ent.sname)) {
                int k = 0;
                bool done = false;
                for (int p = 0; p < lfn_total && !done; p++) {
                    for (int i = 0; i < 13; i++) {
                        uint16_t ch = lfn_buf[13 * p + i];
                        if (ch == 0x0000) { done = true; break; }
                        if (ch == 0xFFFF) { done = true; break; }
                        if (k < FAT_NAME_MAX)
                            ent.name[k++] = (ch < 0x80) ? (char)ch : '?';
                    }
                }
                ent.name[k] = 0;
                ent.has_lfn = k > 0;
            }
            if (!ent.has_lfn)
                sname_to_name(ent.sname, ent.name);

            if (fn(ctx, &ent)) return 1;
            lfn_total = 0;
        }

        uint32_t next;
        if (fat_entry(sb, c, &next) != 0) return -1;
        if (FAT_IS_EOC(next)) return 0;
        c = next;
        if (++guard > sb->cluster_count) return -1;
    }
    return 0;
}

/* ---- inode cache ---- */

static const struct inode_operations fat_iops;
static const struct file_operations fat_file_fops;

static struct fat32_node *node_find(struct fat32_sb *sb, uint32_t parent_clus,
                                     const uint8_t sname[11]) {
    for (struct fat32_node *n = sb->nodes; n; n = n->next)
        if (n->parent_clus == parent_clus && memcmp(n->sname, sname, 11) == 0)
            return n;
    return NULL;
}

static void node_unlink(struct fat32_sb *sb, struct fat32_node *n) {
    struct fat32_node **pp = &sb->nodes;
    while (*pp && *pp != n) pp = &(*pp)->next;
    if (*pp) *pp = n->next;
}

static struct fat32_node *node_new(struct fat32_sb *sb, struct inode *parent_vfs,
                                   uint32_t parent_clus, const uint8_t sname[11],
                                   uint32_t first_clus, uint32_t size, bool is_dir,
                                   uint32_t dirent_clus, uint32_t dirent_off) {
    struct fat32_node *n = kmalloc(sizeof(*n));
    if (!n) return NULL;
    memset(n, 0, sizeof(*n));
    n->sb = sb;
    n->first_clus = first_clus;
    n->is_dir = is_dir;
    n->parent_clus = parent_clus;
    memcpy(n->sname, sname, 11);
    n->dirent_clus = dirent_clus;
    n->dirent_off = dirent_off;
    n->vfs.ino = sb->next_ino++;
    /* v0.9.0: FAT carries no permission bits, so the inode cache applies
     * Unix defaults -- dirs 0755, files 0644, owner root (uid 0). Runtime
     * chowns (login's /home pin) live in the cache only and do not survive
     * a remount; that is the documented FAT32 trade-off. */
    n->vfs.mode = is_dir ? (S_IFDIR | 0755) : (S_IFREG | 0644);
    n->vfs.size = size;
    n->vfs.i_priv = n;
    n->vfs.i_parent = parent_vfs;
    n->vfs.i_op = &fat_iops;
    n->vfs.f_op = is_dir ? NULL : &fat_file_fops;
    n->next = sb->nodes;
    sb->nodes = n;
    return n;
}

/* write the size/first-cluster fields of a node's dirent back to disk */
static int dirent_update(struct fat32_sb *sb, struct fat32_node *n) {
    if (n->dirent_clus == 0) return 0;          /* root has no dirent */
    if (clus_read(sb, n->dirent_clus, sb->bounce) != 0) return -1;
    uint8_t *e = sb->bounce + n->dirent_off;
    st16(e + 20, n->first_clus >> 16);
    st16(e + 26, n->first_clus & 0xFFFF);
    st32(e + 28, n->vfs.size);
    return clus_write(sb, n->dirent_clus, sb->bounce);
}

/* ---- VFS inode operations ---- */

struct lookup_ctx {
    struct fat32_sb *sb;
    struct inode *parent_vfs;
    uint32_t parent_clus;
    const char *target;
    struct fat32_node *node;
};

static int lookup_cb(void *ctx, const struct dir_scan_entry *e) {
    struct lookup_ctx *L = ctx;
    if (!fat_name_eq(e->name, L->target)) return 0;

    struct fat32_node *n = node_find(L->sb, L->parent_clus, e->sname);
    if (n) {
        /* refresh in case the entry changed shape on disk */
        n->first_clus = e->first_clus;
        n->is_dir = e->is_dir;
        n->dirent_clus = e->dirent_clus;
        n->dirent_off = e->dirent_off;
        n->vfs.size = e->is_dir ? 0 : e->size;
    } else {
        n = node_new(L->sb, L->parent_vfs, L->parent_clus, e->sname,
                     e->first_clus, e->is_dir ? 0 : e->size, e->is_dir,
                     e->dirent_clus, e->dirent_off);
    }
    L->node = n;
    return 1;                                   /* stop the walk */
}

static struct inode *fat_i_lookup(struct inode *dir, const char *name) {
    struct fat32_node *dn = node_of(dir);
    if (!dn || !dn->is_dir || !name[0]) return NULL;
    struct fat32_sb *sb = dn->sb;

    irq_flags_t fl;
    spin_lock_irqsave(&sb->lock, &fl);
    struct lookup_ctx ctx = { sb, dir, dn->first_clus, name, NULL };
    dir_walk(sb, dn->first_clus, lookup_cb, &ctx);
    spin_unlock_irqrestore(&sb->lock, fl);
    return ctx.node ? &ctx.node->vfs : NULL;
}

struct readdir_ctx {
    int want;
    int idx;
    char *out;
    size_t max;
};

static int readdir_cb(void *ctx, const struct dir_scan_entry *e) {
    struct readdir_ctx *R = ctx;
    if (R->idx != R->want) {
        R->idx++;
        return 0;
    }
    strncpy(R->out, e->name, R->max - 1);
    R->out[R->max - 1] = 0;
    return 1;
}

static int fat_i_readdir(struct inode *dir, int idx, char *name_out,
                         size_t name_max) {
    struct fat32_node *dn = node_of(dir);
    if (!dn || !dn->is_dir || !name_out || name_max == 0) return -1;
    struct fat32_sb *sb = dn->sb;

    irq_flags_t fl;
    spin_lock_irqsave(&sb->lock, &fl);
    struct readdir_ctx ctx = { idx, 0, name_out, name_max };
    int rc = dir_walk(sb, dn->first_clus, readdir_cb, &ctx);
    spin_unlock_irqrestore(&sb->lock, fl);

    if (rc == 1) return 0;                      /* entry copied out */
    if (rc != 0) return -1;
    return 1;                                   /* end of directory */
}

/* find the slot a new dirent goes into: the first deleted (0xE5) or
 * end-marker (0x00) slot in chain order, or a freshly linked zeroed
 * cluster when neither exists. reusing deleted slots is what keeps a
 * churned directory from growing a fresh cluster on every refill; the
 * walker treats 0xE5 as an LFN-group break, so one reused slot can never
 * splice into a live LFN group. *out_fresh marks a just-appended cluster
 * (all-zero on disk -- the caller may skip the read) */
static int dir_append_slot(struct fat32_sb *sb, uint32_t dir_clus,
                           uint32_t *out_clus, uint32_t *out_off,
                           bool *out_fresh) {
    uint32_t c = dir_clus, last = dir_clus;
    uint32_t guard = 0;
    while (clus_valid(sb, c)) {
        if (clus_read(sb, c, sb->bounce) != 0) return -1;
        for (uint32_t off = 0; off + 32 <= sb->clus_bytes; off += 32)
            if (sb->bounce[off] == DIRENT_END ||
                sb->bounce[off] == DIRENT_DEL) {
                *out_clus = c;
                *out_off = off;
                *out_fresh = false;
                return 0;
            }
        uint32_t next;
        if (fat_entry(sb, c, &next) != 0) return -1;
        last = c;
        if (FAT_IS_EOC(next)) break;
        c = next;
        if (++guard > sb->cluster_count) return -1;
    }
    uint32_t nc = cluster_alloc(sb);
    if (!nc) return -1;
    if (fat_set(sb, last, nc) != 0) return -1;  /* link into the chain */
    *out_clus = nc;
    *out_off = 0;
    *out_fresh = true;
    return 0;                                   /* cluster is all-zero already */
}

static void dirent_fill(uint8_t *e, const uint8_t sname[11], uint8_t attr,
                        uint32_t first, uint32_t size) {
    memset(e, 0, 32);
    memcpy(e, sname, 11);
    e[11] = attr;
    st16(e + 20, first >> 16);
    st16(e + 26, first & 0xFFFF);
    st32(e + 28, size);
}

static struct inode *fat_i_create(struct inode *dir, const char *name,
                                  uint16_t mode) {
    struct fat32_node *dn = node_of(dir);
    if (!dn || !dn->is_dir || !name[0] || strlen(name) >= FAT_NAME_MAX)
        return NULL;
    uint16_t type = mode & S_IFMT;
    if (type != S_IFREG && type != S_IFDIR) return NULL;
    uint8_t sname[11];
    if (name_to_sname(name, sname) != 0) return NULL;
    struct fat32_sb *sb = dn->sb;

    irq_flags_t fl;
    spin_lock_irqsave(&sb->lock, &fl);
    struct fat32_node *node = NULL;
    uint32_t dir_first = 0;                     /* a directory's own cluster */
    bool dirent_on_disk = false;                /* ...referenced by its slot */
    do {
        /* exists? (name-eq covers alias collisions in the common case) */
        struct lookup_ctx L = { sb, dir, dn->first_clus, name, NULL };
        if (dir_walk(sb, dn->first_clus, lookup_cb, &L) == 1 || L.node)
            break;

        bool is_dir = type == S_IFDIR;
        uint32_t first = 0;

        if (is_dir) {
            first = cluster_alloc(sb);
            if (!first) break;
            dir_first = first;
            /* a fresh directory starts with "." and ".." */
            memset(sb->bounce, 0, sb->clus_bytes);
            dirent_fill(sb->bounce, (const uint8_t *)".          ",
                        ATTR_DIR, first, 0);
            dirent_fill(sb->bounce + 32, (const uint8_t *)"..         ",
                        ATTR_DIR, dn->first_clus, 0);
            if (clus_write(sb, first, sb->bounce) != 0) break;
        }

        uint32_t slot_clus, slot_off;
        bool slot_fresh;
        if (dir_append_slot(sb, dn->first_clus, &slot_clus, &slot_off,
                            &slot_fresh) != 0)
            break;

        /* build the dirent in the slot's cluster: a just-appended cluster
         * is zero on disk, anything else is read-modify-write (the slot
         * may sit at offset 0 of an older cluster in the chain) */
        if (slot_fresh) {
            memset(sb->bounce, 0, sb->clus_bytes);
        } else {
            if (clus_read(sb, slot_clus, sb->bounce) != 0) break;
        }
        dirent_fill(sb->bounce + slot_off, sname,
                    is_dir ? ATTR_DIR : ATTR_ARC, first, 0);
        if (clus_write(sb, slot_clus, sb->bounce) != 0) break;
        dirent_on_disk = true;

        node = node_new(sb, dir, dn->first_clus, sname, first, 0, is_dir,
                        slot_clus, slot_off);
    } while (0);
    /* failure before the slot dirent landed: nothing on disk references
     * the directory's own cluster yet -- give it back (disk-full etc.);
     * once the dirent is written the create is real, leak or not */
    if (dir_first && !dirent_on_disk)
        chain_free(sb, dir_first);
    spin_unlock_irqrestore(&sb->lock, fl);
    return node ? &node->vfs : NULL;
}

static int fat_i_mkdir(struct inode *dir, const char *name) {
    return fat_i_create(dir, name, S_IFDIR | 0755) ? 0 : -1;
}

struct unlink_ctx {
    const char *target;
    struct dir_scan_entry ent;
    bool found;
};

static int unlink_cb(void *ctx, const struct dir_scan_entry *e) {
    struct unlink_ctx *U = ctx;
    if (!fat_name_eq(e->name, U->target)) return 0;
    U->ent = *e;
    U->found = true;
    return 1;
}

/* mark the 32-byte dirent slots covering [from_off, to_off) deleted; the
 * bounds are always slot-aligned (LFN groups are contiguous with their
 * 8.3 dirent), so exactly the victim's entries die -- later files in the
 * same cluster must survive an unlink untouched */
static int mark_deleted_range(struct fat32_sb *sb, uint32_t clus,
                              uint32_t from_off, uint32_t to_off) {
    if (from_off >= to_off) return 0;
    if (clus_read(sb, clus, sb->bounce) != 0) return -1;
    for (uint32_t off = from_off; off + 32 <= to_off; off += 32)
        sb->bounce[off] = DIRENT_DEL;
    return clus_write(sb, clus, sb->bounce);
}

struct empty_ctx {
    bool empty;
};

static int empty_check_cb(void *ctx, const struct dir_scan_entry *e) {
    (void)e;
    struct empty_ctx *E = ctx;
    E->empty = false;
    return 1;
}

static int fat_i_unlink(struct inode *dir, const char *name) {
    struct fat32_node *dn = node_of(dir);
    if (!dn || !dn->is_dir || !name[0]) return -1;
    struct fat32_sb *sb = dn->sb;

    irq_flags_t fl;
    spin_lock_irqsave(&sb->lock, &fl);
    int rc = -1;
    do {
        struct unlink_ctx U;
        memset(&U, 0, sizeof(U));
        U.target = name;
        if (dir_walk(sb, dn->first_clus, unlink_cb, &U) != 1 || !U.found)
            break;
        struct dir_scan_entry *e = &U.ent;

        if (e->is_dir) {
            /* directories only when empty ("." and ".." are skipped by the
             * walker, so an empty dir yields zero scan entries) */
            struct empty_ctx E = { true };
            if (dir_walk(sb, e->first_clus, empty_check_cb, &E) != 0 || !E.empty)
                break;
        }

        /* delete exactly the LFN group + the 8.3 dirent (a group is at most
         * 21 entries, spanning at most two clusters) */
        if (e->lfn_first_clus == e->dirent_clus) {
            if (mark_deleted_range(sb, e->dirent_clus, e->lfn_first_off,
                                   e->dirent_off + 32) != 0)
                break;
        } else {
            if (mark_deleted_range(sb, e->lfn_first_clus, e->lfn_first_off,
                                   sb->clus_bytes) != 0)
                break;
            if (mark_deleted_range(sb, e->dirent_clus, 0,
                                   e->dirent_off + 32) != 0)
                break;
        }

        /* free the file's (or empty dir's) cluster chain */
        if (e->first_clus != 0 && chain_free(sb, e->first_clus) != 0)
            break;

        struct fat32_node *n = node_find(sb, dn->first_clus, e->sname);
        if (n) {
            node_unlink(sb, n);
            kfree(n);
        }
        rc = 0;
    } while (0);
    spin_unlock_irqrestore(&sb->lock, fl);
    return rc;
}

static int fat_i_truncate(struct inode *ino) {
    struct fat32_node *n = node_of(ino);
    if (!n || n->is_dir) return -1;
    struct fat32_sb *sb = n->sb;

    irq_flags_t fl;
    spin_lock_irqsave(&sb->lock, &fl);
    int rc = -1;
    do {
        if (n->first_clus != 0 && chain_free(sb, n->first_clus) != 0)
            break;
        n->first_clus = 0;
        n->vfs.size = 0;
        if (dirent_update(sb, n) != 0) break;
        rc = 0;
    } while (0);
    spin_unlock_irqrestore(&sb->lock, fl);
    return rc;
}

static const struct inode_operations fat_iops = {
    .lookup = fat_i_lookup,
    .create = fat_i_create,
    .mkdir = fat_i_mkdir,
    .unlink = fat_i_unlink,
    .truncate = fat_i_truncate,
    .readdir = fat_i_readdir,
};

/* ---- file operations ---- */

static int fat_f_read(struct file *f, void *buf, uint32_t len) {
    struct fat32_node *n = node_of(f->inode);
    if (!n || n->is_dir) return -1;
    struct fat32_sb *sb = n->sb;
    uint32_t pos = f->pos;
    if (pos >= n->vfs.size) return 0;
    uint32_t nlen = len;
    if (nlen > n->vfs.size - pos) nlen = n->vfs.size - pos;

    irq_flags_t fl;
    spin_lock_irqsave(&sb->lock, &fl);
    int rc = -1;
    do {
        uint32_t c = cluster_at(sb, n->first_clus, pos / sb->clus_bytes);
        if (!c) break;
        uint32_t in_off = pos % sb->clus_bytes;
        uint32_t copied = 0;
        while (copied < nlen) {
            if (clus_read(sb, c, sb->bounce) != 0) goto out;
            uint32_t chunk = sb->clus_bytes - in_off;
            if (chunk > nlen - copied) chunk = nlen - copied;
            memcpy((uint8_t *)buf + copied, sb->bounce + in_off, chunk);
            copied += chunk;
            in_off = 0;
            if (copied < nlen) {
                uint32_t next;
                if (fat_entry(sb, c, &next) != 0) goto out;
                if (FAT_IS_EOC(next)) break;    /* chain shorter than size */
                c = next;
            }
        }
        f->pos = pos + copied;
        rc = (int)copied;
    } while (0);
out:
    spin_unlock_irqrestore(&sb->lock, fl);
    return rc;
}

static int fat_f_write(struct file *f, const void *buf, uint32_t len) {
    struct fat32_node *n = node_of(f->inode);
    if (!n || n->is_dir || len == 0) return -1;
    struct fat32_sb *sb = n->sb;
    uint32_t pos = f->pos;
    uint32_t end = pos + len;

    irq_flags_t fl;
    spin_lock_irqsave(&sb->lock, &fl);
    int rc = -1;
    do {
        uint32_t need = (end + sb->clus_bytes - 1) / sb->clus_bytes;

        /* grow the chain; new clusters arrive zeroed (sparse gaps read 0) */
        if (n->first_clus == 0) {
            uint32_t c = cluster_alloc(sb);
            if (!c) break;
            memset(sb->bounce, 0, sb->clus_bytes);
            if (clus_write(sb, c, sb->bounce) != 0) break;
            n->first_clus = c;
        }
        uint32_t last = n->first_clus;
        uint32_t have = 1;
        while (have < need || last == n->first_clus) {
            uint32_t next;
            if (fat_entry(sb, last, &next) != 0) goto out;
            if (FAT_IS_EOC(next)) break;
            last = next;
            have++;
            if (have > sb->cluster_count) goto out;
        }
        while (have < need) {
            uint32_t c = cluster_alloc(sb);
            if (!c) break;
            memset(sb->bounce, 0, sb->clus_bytes);
            if (clus_write(sb, c, sb->bounce) != 0) goto out;
            if (fat_set(sb, last, c) != 0) goto out;
            last = c;
            have++;
        }

        uint32_t c = cluster_at(sb, n->first_clus, pos / sb->clus_bytes);
        if (!c) break;
        uint32_t in_off = pos % sb->clus_bytes;
        uint32_t copied = 0;
        while (copied < len) {
            /* read-modify-write: partial clusters preserve their tail */
            if (clus_read(sb, c, sb->bounce) != 0) goto out;
            uint32_t chunk = sb->clus_bytes - in_off;
            if (chunk > len - copied) chunk = len - copied;
            memcpy(sb->bounce + in_off, (const uint8_t *)buf + copied, chunk);
            if (clus_write(sb, c, sb->bounce) != 0) goto out;
            copied += chunk;
            in_off = 0;
            if (copied < len) {
                uint32_t next;
                if (fat_entry(sb, c, &next) != 0) goto out;
                if (FAT_IS_EOC(next)) goto out; /* chain too short: corrupt */
                c = next;
            }
        }

        if (end > n->vfs.size) n->vfs.size = end;
        if (dirent_update(sb, n) != 0) goto out;
        f->pos = end;
        rc = (int)len;
    } while (0);
out:
    spin_unlock_irqrestore(&sb->lock, fl);
    return rc;
}

static const struct file_operations fat_file_fops = {
    .read = fat_f_read,
    .write = fat_f_write,
};

/* ---- mount / unmount ---- */

static bool bpb_is_fat32(const uint8_t *b) {
    if (b[510] != 0x55 || b[511] != 0xAA) return false;
    if (rd16(b + 11) != ATA_SECTOR_SIZE) return false;
    if (rd16(b + 17) != 0) return false;        /* root_ent_cnt: FAT32 = 0 */
    if (rd16(b + 19) != 0) return false;        /* tot_sec16: FAT32 = 0 */
    if (rd16(b + 22) != 0) return false;        /* fatsz16: FAT32 = 0 */
    return rd32(b + 36) != 0;                   /* fatsz32 */
}

/* first FAT32 partition (type 0x0B/0x0C) in the MBR, or 0xFFFFFFFF */
static uint32_t mbr_find_fat32(const uint8_t *mbr) {
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) return 0xFFFFFFFFu;
    for (int i = 0; i < 4; i++) {
        const uint8_t *pe = mbr + 446 + 16 * i;
        if (pe[4] == 0x0B || pe[4] == 0x0C)
            return rd32(pe + 8);
    }
    return 0xFFFFFFFFu;
}

struct fat32_sb *fat32_mount(void) {
    if (!ata_present()) return NULL;

    uint8_t sec[ATA_SECTOR_SIZE];
    if (ata_read_sectors(0, 1, sec) != 0) return NULL;

    uint32_t part_lba = 0;
    if (!bpb_is_fat32(sec)) {
        part_lba = mbr_find_fat32(sec);
        if (part_lba == 0xFFFFFFFFu) return NULL;
        if (ata_read_sectors(part_lba, 1, sec) != 0) return NULL;
        if (!bpb_is_fat32(sec)) return NULL;
    }

    uint32_t spc = sec[13];
    uint32_t rsvd = rd16(sec + 14);
    uint32_t nfats = sec[16];
    uint32_t fatsz = rd32(sec + 36);
    uint32_t tot = rd32(sec + 32);
    uint32_t root_clus = rd32(sec + 44);

    if (spc == 0 || spc > 128 || (spc & (spc - 1)) != 0) return NULL;
    if (rsvd == 0 || nfats == 0 || nfats > 4 || fatsz == 0 || tot == 0) return NULL;
    if (rsvd + nfats * fatsz >= tot) return NULL;

    struct fat32_sb *sb = kmalloc(sizeof(*sb));
    if (!sb) return NULL;
    memset(sb, 0, sizeof(*sb));
    sb->part_lba = part_lba;
    sb->spc = spc;
    sb->clus_bytes = spc * ATA_SECTOR_SIZE;
    sb->fat_secs = fatsz;
    sb->fat_lba = part_lba + rsvd;
    sb->data_lba = part_lba + rsvd + nfats * fatsz;
    sb->cluster_count = (tot - rsvd - nfats * fatsz) / spc;
    sb->root_clus = root_clus;
    sb->lock = (spinlock_t)SPINLOCK_INIT;

    /* this driver is FAT32 only: below 65525 clusters the volume is
     * FAT12/FAT16 by the spec's classification and we refuse it */
    if (sb->cluster_count < 65525 || !clus_valid(sb, root_clus)) {
        kfree(sb);
        return NULL;
    }
    sb->bounce = kmalloc(sb->clus_bytes);
    if (!sb->bounce) {
        kfree(sb);
        return NULL;
    }

    /* full FAT scan: free-cluster count + first free hint (one-time cost;
     * FSInfo is advisory, the FAT itself is the truth) */
    uint32_t total = 2 + sb->cluster_count;
    for (uint32_t c = 2; c < total; c++) {
        uint32_t v;
        if (fat_entry(sb, c, &v) != 0) goto fail;
        if (v == FAT_FREE) {
            if (sb->free_clusters == 0 && sb->next_free == 0)
                sb->next_free = c;
            sb->free_clusters++;
        }
    }
    if (sb->next_free == 0) sb->next_free = total;  /* volume full */

    sb->root = node_new(sb, NULL, 0, (const uint8_t *)"           ",
                        sb->root_clus, 0, true, 0, 0);
    if (!sb->root) goto fail;
    sb->root->vfs.i_parent = &sb->root->vfs;    /* ".." at the root stays */
    return sb;

fail:
    fat32_unmount(sb);
    return NULL;
}

struct inode *fat32_root_inode(struct fat32_sb *sb) {
    return sb && sb->root ? &sb->root->vfs : NULL;
}

void fat32_unmount(struct fat32_sb *sb) {
    if (!sb) return;
    while (sb->nodes) {
        struct fat32_node *n = sb->nodes->next;
        kfree(sb->nodes);
        sb->nodes = n;
    }
    if (sb->bounce) kfree(sb->bounce);
    if (g_fat_sb == sb) g_fat_sb = NULL;        /* defensive; callers do not
                                                   unmount the live root */
    kfree(sb);
}
