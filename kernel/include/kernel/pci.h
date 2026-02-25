#ifndef _PCI_H
#define _PCI_H

#include <stdint.h>

/* PCI configuration space I/O ports */
#define PCI_CONFIG_ADDR  0x0CF8
#define PCI_CONFIG_DATA  0x0CFC

/* PCI config space register offsets */
#define PCI_VENDOR_ID    0x00   /* 16-bit */
#define PCI_DEVICE_ID    0x02   /* 16-bit */
#define PCI_COMMAND      0x04   /* 16-bit */
#define PCI_STATUS       0x06   /* 16-bit */
#define PCI_REVISION     0x08   /* 8-bit */
#define PCI_PROG_IF      0x09   /* 8-bit */
#define PCI_SUBCLASS     0x0A   /* 8-bit */
#define PCI_CLASS        0x0B   /* 8-bit */
#define PCI_HEADER_TYPE  0x0E   /* 8-bit */
#define PCI_BAR0         0x10   /* 32-bit */
#define PCI_BAR1         0x14
#define PCI_BAR2         0x18
#define PCI_BAR3         0x1C
#define PCI_BAR4         0x20
#define PCI_BAR5         0x24
#define PCI_IRQ_LINE     0x3C   /* 8-bit */
#define PCI_IRQ_PIN      0x3D   /* 8-bit */

/* PCI command register bits */
#define PCI_CMD_IO_SPACE       0x0001
#define PCI_CMD_MEM_SPACE      0x0002
#define PCI_CMD_BUS_MASTER     0x0004
#define PCI_CMD_INT_DISABLE    0x0400

/* Maximum devices to track */
#define PCI_MAX_DEVICES  32

typedef struct {
    uint8_t  bus;
    uint8_t  slot;
    uint8_t  func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  irq_line;
    uint32_t bar[6];
} pci_device_t;

/* Initialize PCI and scan all buses */
void pci_init(void);

/* Read/write PCI configuration space */
uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint8_t  pci_config_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void     pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val);
void     pci_config_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t val);

/* Find a device by vendor/device ID. Returns pointer or NULL. */
pci_device_t *pci_find_device(uint16_t vendor_id, uint16_t device_id);

/* Get the list of all discovered devices */
pci_device_t *pci_get_devices(int *count_out);

/* Enable bus mastering for a device (needed for DMA) */
void pci_enable_bus_master(pci_device_t *dev);

#endif
