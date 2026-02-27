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
#define PCI_CAP_PTR      0x34   /* 8-bit: first capability pointer */
#define PCI_IRQ_LINE     0x3C   /* 8-bit */
#define PCI_IRQ_PIN      0x3D   /* 8-bit */

/* PCI command register bits */
#define PCI_CMD_IO_SPACE       0x0001
#define PCI_CMD_MEM_SPACE      0x0002
#define PCI_CMD_BUS_MASTER     0x0004
#define PCI_CMD_INT_DISABLE    0x0400

/* PCI status register bits */
#define PCI_STATUS_CAP_LIST    0x0010   /* bit 4: capabilities list present */

/* PCI capability IDs */
#define PCI_CAP_ID_MSI         0x05
#define PCI_CAP_ID_VENDOR      0x09    /* vendor-specific (used by VirtIO) */
#define PCI_CAP_ID_MSIX        0x11

/* BAR type bits */
#define PCI_BAR_IO             0x01    /* bit 0: 1 = I/O space, 0 = memory */
#define PCI_BAR_MEM_TYPE_MASK  0x06    /* bits 2:1: memory type */
#define PCI_BAR_MEM_32BIT      0x00    /* 00 = 32-bit */
#define PCI_BAR_MEM_64BIT      0x04    /* 10 = 64-bit */
#define PCI_BAR_MEM_PREFETCH   0x08    /* bit 3: prefetchable */

/* Maximum devices to track */
#define PCI_MAX_DEVICES  32

/* Maximum capabilities per device */
#define PCI_MAX_CAPS     16

/* PCI capability entry (parsed from capability linked list) */
typedef struct {
    uint8_t  id;        /* capability ID (e.g., PCI_CAP_ID_MSI) */
    uint8_t  offset;    /* offset in config space where this cap starts */
} pci_cap_t;

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
    uint32_t bar_size[6];   /* size of each BAR region in bytes (0 if unimplemented) */
    uint8_t  bar_is_64;     /* bitmask: bit N set if BAR[N] is lower half of 64-bit pair */
    uint8_t  cap_count;     /* number of capabilities found */
    pci_cap_t caps[PCI_MAX_CAPS];
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

/* Find a capability by ID on a device. Returns config space offset, or 0 if not found. */
uint8_t pci_find_cap(pci_device_t *dev, uint8_t cap_id);

/* Get the physical base address for a BAR (handles 64-bit BARs).
 * For memory BARs, masks off type bits. For 64-bit, combines BAR[n] and BAR[n+1].
 * Returns 0 if BAR is unimplemented or is I/O type. */
uint32_t pci_bar_addr(pci_device_t *dev, int bar_index);

#endif
