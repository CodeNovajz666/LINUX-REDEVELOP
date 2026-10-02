#ifndef RAMFS_H
#define RAMFS_H

#include <stdint.h>
#include "vfs.h"

/* in-memory filesystem: the whole tree lives in the kernel heap */
void ramfs_init(void);                      /* creates "/" and registers it as vfs root */
int ramfs_mkdir(const char *path);
/* create a node with custom file operations (used by devfs for /dev/console) */
struct inode *ramfs_mknod(const char *path, uint16_t mode,
                          const struct file_operations *f_op);
/* unpack a build-time image (see tools/mkramfs.py) into the live tree;
 * returns the number of files created */
int ramfs_mount_initrd(const void *img, uint32_t size);

#endif /* RAMFS_H */
