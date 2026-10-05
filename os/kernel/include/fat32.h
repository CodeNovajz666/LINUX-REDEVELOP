#ifndef FAT32_H
#define FAT32_H

#include <stdint.h>
#include <stdbool.h>
#include "vfs.h"

/*
 * FAT32 on the ATA primary-master disk (v0.8.0).
 *
 * - LFN is read-only (long names are parsed and matched, but newly
 *   created files get plain 8.3 short names).
 * - Every operation takes the per-sb spinlock: the cluster buffer, FAT
 *   sector cache and inode cache are shared state. The lock sits at the
 *   fs layer in the lock order (fat32 > ata); PIO happens under it, which
 *   is honest but coarse -- fine at teaching scale.
 * - Power-cycle honesty: writes go straight through to the disk (write-
 *   through, no dirty page cache), so "write, drop power, read back" is
 *   exactly what the ktest power-cycle case proves via a fresh mount.
 */

/* mount the FAT32 volume found on the ATA disk (MBR partition type
 * 0x0B/0x0C, or a bare FAT32 superblock at LBA 0); NULL = failure */
struct fat32_sb;
struct fat32_sb *fat32_mount(void);

/* the root directory's VFS inode of a mounted instance */
struct inode *fat32_root_inode(struct fat32_sb *sb);

/* release a mounted instance (inode cache + buffers); the caller must
 * not keep using inodes from it afterwards */
void fat32_unmount(struct fat32_sb *sb);

/* live (boot) instance, set by kmain when root=/dev/hd0 succeeded */
extern struct fat32_sb *g_fat_sb;
bool fat32_mounted(void);

/* free-cluster accounting of the live instance (test observability) */
uint32_t fat32_free_clusters(struct fat32_sb *sb);
uint32_t fat32_total_clusters(struct fat32_sb *sb);

#endif /* FAT32_H */
