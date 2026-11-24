#include "novix.h"
#include "ata.h"
#include "console.h"
#include "spinlock.h"

/*
 * ATA PIO, primary channel (0x1F0-0x1F7) master, LBA28, polled (v0.8.0).
 *
 * One lock serialises whole commands: the register file is per-channel
 * shared state, two CPUs issuing overlapping commands would corrupt each
 * other's sector counts. It is a leaf lock -- nothing above ata may be
 * held while calling in (fat32 takes its own, coarser lock above).
 */

#define ATA_PORT_DATA    0x1F0
#define ATA_PORT_ERROR   0x1F1
#define ATA_PORT_SECNT   0x1F2
#define ATA_PORT_LBA0    0x1F3
#define ATA_PORT_LBA1    0x1F4
#define ATA_PORT_LBA2    0x1F5
#define ATA_PORT_DRIVE   0x1F6
#define ATA_PORT_STATUS  0x1F7
#define ATA_PORT_COMMAND 0x1F7

#define ATA_SR_ERR  0x01
#define ATA_SR_DRQ  0x08
#define ATA_SR_DF   0x20
#define ATA_SR_BSY  0x80

#define ATA_CMD_IDENTIFY      0xEC
#define ATA_CMD_READ_PIO      0x20
#define ATA_CMD_WRITE_PIO     0x30
#define ATA_CMD_FLUSH_CACHE   0xE7

#define ATA_DRIVE_LBA28 0xE0            /* master + LBA mode */
#define ATA_DRIVE_CHS   0xA0            /* used only to sniff the bus */

static bool disk_present;
static uint32_t nsectors;
static spinlock_t ata_lock = SPINLOCK_INIT;

/* one string op per sector instead of 256 single-word port I/Os (each
 * inb/outw costs a slow bus cycle; rep amortizes the port decode) */
static inline void ata_insw(uint16_t port, void *buf, uint32_t words) {
    __asm__ volatile("rep insw"
                     : "+D"(buf), "+c"(words)
                     : "d"(port)
                     : "memory");
}

static inline void ata_outsw(uint16_t port, const void *buf, uint32_t words) {
    __asm__ volatile("rep outsw"
                     :
                     : "d"(port), "S"(buf), "c"(words)
                     : "memory");
}

/* wait for BSY to clear; then require DRQ unless idle_ok */
static int ata_wait(bool need_drq) {
    for (int i = 0; i < 200000; i++) {
        uint8_t s = inb(ATA_PORT_STATUS);
        if (s & ATA_SR_ERR) return -1;
        if (s & ATA_SR_DF) return -1;
        if (!(s & ATA_SR_BSY)) {
            if (!need_drq || (s & ATA_SR_DRQ)) return 0;
        }
    }
    return -1;
}

static void ata_select(uint32_t lba) {
    /* drive/head: LBA28 packs the top 4 bits of the address here */
    outb(ATA_PORT_DRIVE, ATA_DRIVE_LBA28 | ((lba >> 24) & 0x0F));
    io_wait();
}

int ata_init(void) {
    /* floating bus sniff: selecting an absent master reads 0xFF */
    outb(ATA_PORT_DRIVE, ATA_DRIVE_CHS);
    io_wait();
    if (inb(ATA_PORT_STATUS) == 0xFF) {
        kprintf("ata: no disk on primary master\n");
        return -1;
    }

    outb(ATA_PORT_DRIVE, ATA_DRIVE_LBA28);
    outb(ATA_PORT_SECNT, 0);
    outb(ATA_PORT_LBA0, 0);
    outb(ATA_PORT_LBA1, 0);
    outb(ATA_PORT_LBA2, 0);
    outb(ATA_PORT_COMMAND, ATA_CMD_IDENTIFY);
    io_wait();

    uint8_t s = inb(ATA_PORT_STATUS);
    if (s == 0) {
        kprintf("ata: no disk (status 0 after IDENTIFY)\n");
        return -1;
    }
    if (ata_wait(true) != 0) {
        kprintf("ata: IDENTIFY failed (status %#x)\n", s);
        return -1;
    }

    uint16_t id[256];
    for (int i = 0; i < 256; i++)
        id[i] = (uint16_t)inw(ATA_PORT_DATA);

    /* word 0 bit15 set = ATAPI/PACKET device, not a disk */
    if (id[0] & 0x8000) {
        kprintf("ata: IDENTIFY says ATAPI (%#x), not a disk\n", id[0]);
        return -1;
    }
    /* word 49 bit 9: LBA28 supported (every real disk and QEMU set it) */
    if (!(id[49] & 0x0200)) {
        kprintf("ata: device reports no LBA28\n");
        return -1;
    }

    nsectors = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
    if (nsectors == 0) nsectors = 1;           /* broken identify: trust 1 */
    disk_present = true;
    kprintf("ata: primary master, %u sectors (%u MiB), LBA28 PIO\n",
            nsectors, nsectors / 2048);
    return 0;
}

bool ata_present(void) { return disk_present; }
uint32_t ata_sector_count(void) { return nsectors; }

int ata_read_sectors(uint32_t lba, uint32_t count, void *buf) {
    if (!disk_present || count == 0 || count > 255 || !buf) return -1;
    if ((uint64_t)lba + count > nsectors) return -1;

    irq_flags_t fl;
    spin_lock_irqsave(&ata_lock, &fl);
    int rc = -1;
    do {
        ata_select(lba);
        outb(ATA_PORT_SECNT, (uint8_t)count);
        outb(ATA_PORT_LBA0, (uint8_t)lba);
        outb(ATA_PORT_LBA1, (uint8_t)(lba >> 8));
        outb(ATA_PORT_LBA2, (uint8_t)(lba >> 16));
        outb(ATA_PORT_COMMAND, ATA_CMD_READ_PIO);

        uint16_t *out = (uint16_t *)buf;
        for (uint32_t s = 0; s < count; s++) {
            if (ata_wait(true) != 0) goto out;
            ata_insw(ATA_PORT_DATA, out, ATA_SECTOR_SIZE / 2);
            out += ATA_SECTOR_SIZE / 2;
        }
        rc = 0;
    } while (0);
out:
    spin_unlock_irqrestore(&ata_lock, fl);
    return rc;
}

int ata_write_sectors(uint32_t lba, uint32_t count, const void *buf) {
    if (!disk_present || count == 0 || count > 255 || !buf) return -1;
    if ((uint64_t)lba + count > nsectors) return -1;

    irq_flags_t fl;
    spin_lock_irqsave(&ata_lock, &fl);
    int rc = -1;
    do {
        ata_select(lba);
        outb(ATA_PORT_SECNT, (uint8_t)count);
        outb(ATA_PORT_LBA0, (uint8_t)lba);
        outb(ATA_PORT_LBA1, (uint8_t)(lba >> 8));
        outb(ATA_PORT_LBA2, (uint8_t)(lba >> 16));
        outb(ATA_PORT_COMMAND, ATA_CMD_WRITE_PIO);

        const uint16_t *in = (const uint16_t *)buf;
        for (uint32_t s = 0; s < count; s++) {
            if (ata_wait(true) != 0) goto out;
            ata_outsw(ATA_PORT_DATA, in, ATA_SECTOR_SIZE / 2);
            in += ATA_SECTOR_SIZE / 2;
        }
        /* flush through to the medium: this is what makes "poweroff right
         * after write" (the power-cycle test) honest */
        outb(ATA_PORT_COMMAND, ATA_CMD_FLUSH_CACHE);
        if (ata_wait(false) != 0) goto out;
        rc = 0;
    } while (0);
out:
    spin_unlock_irqrestore(&ata_lock, fl);
    return rc;
}
