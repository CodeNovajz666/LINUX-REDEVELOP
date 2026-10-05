#ifndef DRIVER_H
#define DRIVER_H

#include <stdint.h>
#include <stdbool.h>
#include "vfs.h"

/*
 * v0.8.0 device model -- a teaching-sized echo of Linux's driver/device
 * split. Drivers register on a static bus; at boot driver_bus_init()
 * probes each one and brings up the devices it finds. No hotplug: the
 * registry is a plain array, the bus is the machine itself.
 *
 * Every live device is published as a node under /dev/ (devfs), so user
 * programs and the kernel reach hardware the same way -- "everything is
 * a file" now has a registry behind it instead of hand-made mknods.
 */

struct device {
    const char *name;                        /* "console", "hd0", ... */
    uint16_t mode;                           /* S_IFCHR / S_IFBLK */
    const struct file_operations *f_op;
    void *priv;                              /* driver-private state */
};

struct device_driver {
    const char *name;
    int (*probe)(void);                      /* hardware present? */
    struct device *(*init)(void);            /* bring up; NULL = failed */
};

/* register at boot (before driver_bus_init); returns 0 or -1 if full */
int driver_register(const struct device_driver *drv);

/* probe every registered driver and instantiate its devices */
int driver_bus_init(void);

/* device lookup by name (e.g. "hd0"), NULL if absent */
struct device *device_find(const char *name);
bool device_present(const char *name);

/* register the built-in drivers (console/null/zero/ata-hd); call before
 * driver_bus_init */
void driver_init(void);

/* mknod every live device under /dev/ -- called by devfs_init */
void devfs_publish(void);

#endif /* DRIVER_H */
