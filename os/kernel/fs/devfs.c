#include "novix.h"
#include "devfs.h"
#include "driver.h"
#include "ramfs.h"
#include "lib.h"

/*
 * devfs (v0.8.0): the /dev directory. Device nodes are ramfs nodes with
 * S_IFCHR/S_IFBLK mode and the file_operations their driver registered --
 * "everything is a file" with a registry behind it. devfs_init only creates
 * the directory and publishes every live device the driver bus found; the
 * fops themselves live in drivers/driver.c.
 */

void devfs_init(void) {
    ramfs_mkdir("/dev");
    devfs_publish();
}
