#include "novix.h"
#include "devfs.h"
#include "ramfs.h"
#include "console.h"
#include "keyboard.h"
#include "lib.h"

/*
 * devfs (v0.2: featherweight). Device nodes are just ramfs nodes with
 * S_IFCHR mode and custom file_operations -- the "everything is a file"
 * idea in its smallest working form. A real driver registry arrives in
 * 0.7.0; for now nodes are registered by hand at boot.
 */

static int dev_console_read(struct file *f, void *buf, uint32_t len) {
    (void)f;
    if (len == 0) return 0;
    /* one character per read, getchar-style: no line discipline yet */
    int c = keyboard_getchar();
    if (c < 0) return 0;
    *(char *)buf = (char)c;
    return 1;
}

static int dev_console_write(struct file *f, const void *buf, uint32_t len) {
    (void)f;
    irq_flags_t fl = irq_save();
    for (uint32_t i = 0; i < len; i++)
        console_putchar(((const char *)buf)[i]);
    irq_restore(fl);
    return (int)len;
}

static const struct file_operations console_fops = {
    .read = dev_console_read,
    .write = dev_console_write,
};

static int dev_null_write(struct file *f, const void *buf, uint32_t len) {
    (void)f; (void)buf;
    return (int)len;                        /* swallow */
}

static const struct file_operations null_fops = {
    .write = dev_null_write,
};

void devfs_init(void) {
    ramfs_mkdir("/dev");
    ramfs_mknod("/dev/console", S_IFCHR, &console_fops);
    ramfs_mknod("/dev/null", S_IFCHR, &null_fops);
}
