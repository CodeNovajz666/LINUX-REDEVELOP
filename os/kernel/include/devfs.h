#ifndef DEVFS_H
#define DEVFS_H

#include "vfs.h"

/* registers /dev/console as a character device on the ramfs tree */
void devfs_init(void);

#endif /* DEVFS_H */
