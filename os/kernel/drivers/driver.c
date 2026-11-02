#include "novix.h"
#include "driver.h"
#include "ramfs.h"
#include "console.h"
#include "keyboard.h"
#include "ata.h"
#include "lib.h"

/*
 * Driver registry + the built-in character devices (v0.8.0).
 *
 * Locking: the registry is written once during boot (single-threaded,
 * interrupts off) and read-only afterwards, so no lock is needed. The
 * device fops themselves take whatever locks their hardware needs
 * (console fops -> console_lock inside console_putchar; hd0 -> ata_lock).
 */

#define MAX_DRIVERS  8
#define MAX_DEVICES 16

static const struct device_driver *drivers[MAX_DRIVERS];
static int ndrivers;

static struct device devices[MAX_DEVICES];
static int ndevices;

int driver_register(const struct device_driver *drv) {
    if (!drv || ndrivers >= MAX_DRIVERS) return -1;
    drivers[ndrivers++] = drv;
    return 0;
}

struct device *device_find(const char *name) {
    for (int i = 0; i < ndevices; i++)
        if (strcmp(devices[i].name, name) == 0)
            return &devices[i];
    return NULL;
}

bool device_present(const char *name) {
    return device_find(name) != NULL;
}

int driver_bus_init(void) {
    for (int i = 0; i < ndrivers; i++) {
        if (drivers[i]->probe && !drivers[i]->probe()) continue;
        struct device *dev = drivers[i]->init ? drivers[i]->init() : NULL;
        if (!dev) {
            kprintf("driver: %s probe ok but init failed\n", drivers[i]->name);
            continue;
        }
        if (ndevices >= MAX_DEVICES) {
            kprintf("driver: device table full, %s dropped\n", dev->name);
            continue;
        }
        devices[ndevices++] = *dev;
        kprintf("driver: %s -> /dev/%s\n", drivers[i]->name, dev->name);
    }
    return ndevices;
}

void devfs_publish(void) {
    for (int i = 0; i < ndevices; i++) {
        char path[24];
        strcpy(path, "/dev/");
        strncpy(path + 5, devices[i].name, sizeof(path) - 6);
        path[sizeof(path) - 1] = 0;
        ramfs_mknod(path, devices[i].mode, devices[i].f_op);
    }
}

/* ---- built-in character devices ---- */

static int dev_console_read(struct file *f, void *buf, uint32_t len) {
    (void)f;
    if (len == 0) return 0;
    /* one character per read, getchar-style: no line discipline yet */
    int c = keyboard_getchar();
    if (c < 0) return 0;
    /* v0.9.0: tty echo -- printables and newline come back to the screen
     * while the echo flag is on (control chars are the reader's business,
     * e.g. the shell's own "\b \b" backspace feedback). TCSETS turns the
     * flag off so login can read a password silently. */
    if (console_echo_enabled()) {
        if (c == '\n' || (c >= 32 && c < 127))
            console_putchar((char)c);
    }
    *(char *)buf = (char)c;
    return 1;
}

static int dev_console_write(struct file *f, const void *buf, uint32_t len) {
    (void)f;
    /* one locked console_write pass, not len console_putchar round trips:
     * every per-char call re-took console_lock and re-programmed the VGA
     * cursor (4 outb), so a 4 KiB output cost 4096 lock acquisitions --
     * and interleaved its bytes with other CPUs' lines in the serial log
     * the test harness greps. The ANSI parser state is static and spans
     * calls, so batching is behavior-identical. */
    console_write(buf, len);
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

static int dev_zero_read(struct file *f, void *buf, uint32_t len) {
    (void)f;
    memset(buf, 0, len);
    return (int)len;
}

static const struct file_operations zero_fops = {
    .read = dev_zero_read,
    .write = dev_null_write,
};

/* ---- hd0: raw block access, f->pos counts sectors ---- */

static int dev_hd_read(struct file *f, void *buf, uint32_t len) {
    if (len % ATA_SECTOR_SIZE != 0) return -1;
    uint64_t lba = f->pos;
    if (lba + len / ATA_SECTOR_SIZE > ata_sector_count()) return -1;
    if (ata_read_sectors((uint32_t)lba, len / ATA_SECTOR_SIZE, buf) != 0)
        return -1;
    f->pos += len / ATA_SECTOR_SIZE;
    return (int)len;
}

static int dev_hd_write(struct file *f, const void *buf, uint32_t len) {
    if (len % ATA_SECTOR_SIZE != 0) return -1;
    uint64_t lba = f->pos;
    if (lba + len / ATA_SECTOR_SIZE > ata_sector_count()) return -1;
    if (ata_write_sectors((uint32_t)lba, len / ATA_SECTOR_SIZE, buf) != 0)
        return -1;
    f->pos += len / ATA_SECTOR_SIZE;
    return (int)len;
}

static const struct file_operations hd_fops = {
    .read = dev_hd_read,
    .write = dev_hd_write,
};

/* ---- driver definitions ----
 * v0.9.0: permission bits are part of the published mode -- console/null/
 * zero are world-writable like every real /dev/console; the raw disk is
 * root-only (0600), which is what a block device should be. */

static struct device console_device = { "console", S_IFCHR | 0666, &console_fops, NULL };
static struct device null_device    = { "null",    S_IFCHR | 0666, &null_fops,    NULL };
static struct device zero_device    = { "zero",    S_IFCHR | 0666, &zero_fops,    NULL };
static struct device hd0_device     = { "hd0",     S_IFBLK | 0600, &hd_fops,      NULL };

static int console_probe(void) { return 1; }   /* VGA/COM are always there */
static struct device *console_dev_init(void) { return &console_device; }
static struct device *null_init(void) { return &null_device; }
static struct device *zero_init(void) { return &zero_device; }

static int hd_probe(void) { return 1; }         /* the ATA port is always
                                                  wired; the disk may not be */
static struct device *hd_init(void) {           /* IDENTIFY brings up hd0 */
    return ata_init() == 0 ? &hd0_device : NULL;
}

static const struct device_driver builtin_drivers[] = {
    { "console", console_probe, console_dev_init },
    { "null",    console_probe, null_init },
    { "zero",    console_probe, zero_init },
    { "ata-hd",  hd_probe,      hd_init },
};

void driver_init(void) {
    for (unsigned i = 0; i < sizeof(builtin_drivers) / sizeof(builtin_drivers[0]); i++)
        driver_register(&builtin_drivers[i]);
}
