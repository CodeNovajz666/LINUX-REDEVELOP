#ifndef DEVFS_H
#define DEVFS_H

#include "vfs.h"

/* create /dev and mknod every live device from the driver registry */
void devfs_init(void);

#endif /* DEVFS_H */
