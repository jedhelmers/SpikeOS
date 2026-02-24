#include <kernel/ata.h>
#include <kernel/io.h>
#include <kernel/hal.h>
#include <stdio.h>

static int disk_present = 0;
static uint32_t g_total_sectors = 0;

#define ATA_TIMEOUT 100000

/* ------------------------------------------------------------------ */
/*  Polling helpers                                                   */
/* ------------------------------------------------------------------ */

/* Wait for BSY to clear. Returns 0 on success, -1 on timeout. */
static int ata_poll_bsy(void) {
    for (int i = 0; i < ATA_TIMEOUT; i++) {
        uint8_t st = inb(ATA_PRIMARY_IO + ATA_REG_STATUS);
        if (!(st & ATA_SR_BSY)) {
            return 0;
        }
    }

    return -1;
}

/* Wait for BSY clear and DRQ set. Returns 0 on success, -1 on error/timeout. */
static int ata_poll_drq(void) {
    for (int i = 0; i < ATA_TIMEOUT; i++) {
        uint8_t st = inb(ATA_PRIMARY_IO + ATA_REG_STATUS);

        if (st & ATA_SR_ERR) return -1;

        if (st & ATA_SR_DF)  return -1;

        if (!(st & ATA_SR_BSY) && (st & ATA_SR_DRQ)) {
            return 0;
        }
    }

    return -1;
}

/* 400ns delay by reading the status register 4 times */
static void ata_delay(void) {
    inb(ATA_PRIMARY_CTRL);
    inb(ATA_PRIMARY_CTRL);
    inb(ATA_PRIMARY_CTRL);
    inb(ATA_PRIMARY_CTRL);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

int ata_init(void) {
    /* Select primary master drive */
    outb(ATA_PRIMARY_IO + ATA_REG_DRIVE, 0xA0);
    ata_delay();

    /* Zero out sector count and LBA registers */
    outb(ATA_PRIMARY_IO + ATA_REG_SECCOUNT, 0);
    outb(ATA_PRIMARY_IO + ATA_REG_LBA_LO, 0);
    outb(ATA_PRIMARY_IO + ATA_REG_LBA_MID, 0);
    outb(ATA_PRIMARY_IO + ATA_REG_LBA_HI, 0);

    /* Send IDENTIFY command */
    outb(ATA_PRIMARY_IO + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    ata_delay();

    /* Check if device exists */
    uint8_t status = inb(ATA_PRIMARY_IO + ATA_REG_STATUS);
    if (status == 0) {
        printf("[ata] no device on primary master\n");
        return -1;
    }

    /* Wait for BSY to clear */
    if (ata_poll_bsy() != 0) {
        printf("[ata] timeout waiting for IDENTIFY\n");
        return -1;
    }

    /* Check for non-ATA device (ATAPI, SATA, etc.) */
    uint8_t lba_mid = inb(ATA_PRIMARY_IO + ATA_REG_LBA_MID);
    uint8_t lba_hi  = inb(ATA_PRIMARY_IO + ATA_REG_LBA_HI);
    if (lba_mid != 0 || lba_hi != 0) {
        printf("[ata] non-ATA device (mid=%x hi=%x)\n", lba_mid, lba_hi);
        return -1;
    }

    /* Wait for DRQ or ERR */
    if (ata_poll_drq() != 0) {
        printf("[ata] IDENTIFY failed\n");
        return -1;
    }

    /* Read 256 words of identify data (discard — we just need to confirm it works) */
    uint16_t identify[256];
    insw(ATA_PRIMARY_IO + ATA_REG_DATA, identify, 256);

    /* Extract disk size in sectors (words 60-61 = total LBA28 sectors) */
    uint32_t total_sectors = (uint32_t)identify[61] << 16 | identify[60];

    g_total_sectors = total_sectors;
    disk_present = 1;
    printf("[ata] primary master: %d sectors (%d KB)\n",
           total_sectors, total_sectors / 2);

    return 0;
}

int ata_read_sectors(uint32_t lba, uint8_t count, void *buf) {
    if (!disk_present) return -1;
    if (count == 0) return -1;

    /* Disable interrupts during transfer, restore caller's state on exit */
    uint32_t irqflags = hal_irq_save();

    /* Wait for drive ready */
    if (ata_poll_bsy() != 0) {
        hal_irq_restore(irqflags);
        return -1;
    }

    /* Select drive + LBA bits 24-27 */
    outb(ATA_PRIMARY_IO + ATA_REG_DRIVE,
         0xE0 | ((lba >> 24) & 0x0F));

    /* Set sector count and LBA */
    outb(ATA_PRIMARY_IO + ATA_REG_SECCOUNT, count);
    outb(ATA_PRIMARY_IO + ATA_REG_LBA_LO, (uint8_t)(lba));
    outb(ATA_PRIMARY_IO + ATA_REG_LBA_MID, (uint8_t)(lba >> 8));
    outb(ATA_PRIMARY_IO + ATA_REG_LBA_HI, (uint8_t)(lba >> 16));

    /* Send READ SECTORS command */
    outb(ATA_PRIMARY_IO + ATA_REG_COMMAND, ATA_CMD_READ_PIO);

    /* Read each sector */
    uint16_t *ptr = (uint16_t *)buf;
    for (uint8_t i = 0; i < count; i++) {
        if (ata_poll_drq() != 0) {
            hal_irq_restore(irqflags);
            return -1;
        }
        insw(ATA_PRIMARY_IO + ATA_REG_DATA, ptr, 256);
        ptr += 256;
    }

    hal_irq_restore(irqflags);
    return 0;
}

int ata_write_sectors(uint32_t lba, uint8_t count, const void *buf) {
    if (!disk_present) return -1;
    if (count == 0) return -1;

    /* Disable interrupts during transfer, restore caller's state on exit */
    uint32_t irqflags = hal_irq_save();

    /* Wait for drive ready */
    if (ata_poll_bsy() != 0) {
        hal_irq_restore(irqflags);
        return -1;
    }

    /* Select drive + LBA bits 24-27 */
    outb(ATA_PRIMARY_IO + ATA_REG_DRIVE,
         0xE0 | ((lba >> 24) & 0x0F));

    /* Set sector count and LBA */
    outb(ATA_PRIMARY_IO + ATA_REG_SECCOUNT, count);
    outb(ATA_PRIMARY_IO + ATA_REG_LBA_LO, (uint8_t)(lba));
    outb(ATA_PRIMARY_IO + ATA_REG_LBA_MID, (uint8_t)(lba >> 8));
    outb(ATA_PRIMARY_IO + ATA_REG_LBA_HI, (uint8_t)(lba >> 16));

    /* Send WRITE SECTORS command */
    outb(ATA_PRIMARY_IO + ATA_REG_COMMAND, ATA_CMD_WRITE_PIO);

    /* Write each sector */
    const uint16_t *ptr = (const uint16_t *)buf;
    for (uint8_t i = 0; i < count; i++) {
        if (ata_poll_drq() != 0) {
            hal_irq_restore(irqflags);
            return -1;
        }
        outsw(ATA_PRIMARY_IO + ATA_REG_DATA, ptr, 256);
        ptr += 256;
    }

    hal_irq_restore(irqflags);

    /* Flush write cache */
    return ata_flush();
}

int ata_flush(void) {
    if (!disk_present) return -1;

    outb(ATA_PRIMARY_IO + ATA_REG_DRIVE, 0xE0);
    outb(ATA_PRIMARY_IO + ATA_REG_COMMAND, ATA_CMD_FLUSH);

    if (ata_poll_bsy() != 0)
        return -1;

    uint8_t st = inb(ATA_PRIMARY_IO + ATA_REG_STATUS);
    if (st & ATA_SR_ERR)
        return -1;

    return 0;
}

int ata_present(void) {
    return disk_present;
}

uint32_t ata_total_sectors(void) {
    return g_total_sectors;
}
