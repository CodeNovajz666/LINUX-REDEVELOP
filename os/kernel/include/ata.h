#ifndef ATA_H
#define ATA_H

#include <stdint.h>
#include <stdbool.h>

/*
 * ATA PIO driver, primary channel master (v0.8.0). LBA28 only, polled --
 * no IRQs, no DMA: the teaching goal is the smallest disk path that is
 * still a real protocol handshake with a real device.
 */

#define ATA_SECTOR_SIZE 512

/* bring up the primary-master disk (IDENTIFY); 0 = disk found */
int ata_init(void);
bool ata_present(void);
uint32_t ata_sector_count(void);

/* raw sector access; buf must hold count*512 bytes. 0 on success. */
int ata_read_sectors(uint32_t lba, uint32_t count, void *buf);
int ata_write_sectors(uint32_t lba, uint32_t count, const void *buf);

#endif /* ATA_H */
