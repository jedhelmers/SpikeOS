#ifndef _ATA_H
#define _ATA_H

#include <stdint.h>

/* Primary IDE controller ports */
#define ATA_PRIMARY_IO      0x1F0
#define ATA_PRIMARY_CTRL    0x3F6

/* Register offsets from ATA_PRIMARY_IO */
#define ATA_REG_DATA        0   /* 16-bit read/write */
#define ATA_REG_ERROR       1   /* read */
#define ATA_REG_FEATURES    1   /* write */
#define ATA_REG_SECCOUNT    2
#define ATA_REG_LBA_LO      3
#define ATA_REG_LBA_MID     4
#define ATA_REG_LBA_HI      5
#define ATA_REG_DRIVE       6
#define ATA_REG_STATUS      7   /* read */
#define ATA_REG_COMMAND     7   /* write */

/* Commands */
#define ATA_CMD_READ_PIO    0x20
#define ATA_CMD_WRITE_PIO   0x30
#define ATA_CMD_FLUSH       0xE7
#define ATA_CMD_IDENTIFY    0xEC

/* Status register bits */
#define ATA_SR_BSY          0x80
#define ATA_SR_DRDY         0x40
#define ATA_SR_DF           0x20
#define ATA_SR_DRQ          0x08
#define ATA_SR_ERR          0x01

/* Initialize ATA driver. Returns 0 if disk found, -1 if not. */
int ata_init(void);

/* Read 'count' sectors starting at LBA into buf. Returns 0 on success. */
int ata_read_sectors(uint32_t lba, uint8_t count, void *buf);

/* Write 'count' sectors from buf to disk at LBA. Returns 0 on success. */
int ata_write_sectors(uint32_t lba, uint8_t count, const void *buf);

/* Flush disk write cache. Returns 0 on success. */
int ata_flush(void);

/* Returns 1 if a disk was detected during init, 0 otherwise. */
int ata_present(void);

/* Returns total disk size in 512-byte sectors (0 if no disk). */
uint32_t ata_total_sectors(void);

#endif
