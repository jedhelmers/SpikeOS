#ifndef _VIRTIO_H
#define _VIRTIO_H

#include <stdint.h>

/*
 * VirtIO PCI transport definitions (legacy + modern).
 * Based on VirtIO 1.1 specification, sections 4.1 (PCI transport).
 */

/* VirtIO PCI vendor ID */
#define VIRTIO_PCI_VENDOR       0x1AF4

/* VirtIO PCI device IDs (transitional: 0x1000-0x103F) */
#define VIRTIO_PCI_DEV_NET      0x1000
#define VIRTIO_PCI_DEV_BLK      0x1001
#define VIRTIO_PCI_DEV_GPU      0x1050   /* non-transitional GPU */

/* VirtIO PCI capability types (found via PCI capability list, cap ID = 0x09) */
#define VIRTIO_PCI_CAP_COMMON_CFG   1   /* Common configuration */
#define VIRTIO_PCI_CAP_NOTIFY_CFG   2   /* Notifications */
#define VIRTIO_PCI_CAP_ISR_CFG      3   /* ISR access */
#define VIRTIO_PCI_CAP_DEVICE_CFG   4   /* Device-specific config */

/* VirtIO PCI capability structure (in PCI config space) */
typedef struct {
    uint8_t  cap_vndr;       /* PCI capability ID (0x09 = vendor-specific) */
    uint8_t  cap_next;       /* next capability offset */
    uint8_t  cap_len;        /* length of this capability */
    uint8_t  cfg_type;       /* VIRTIO_PCI_CAP_* type */
    uint8_t  bar;            /* BAR index for this structure */
    uint8_t  padding[3];
    uint32_t offset;         /* offset within BAR */
    uint32_t length;         /* length of structure */
} __attribute__((packed)) virtio_pci_cap_t;

/* VirtIO device status bits */
#define VIRTIO_STATUS_ACKNOWLEDGE    1
#define VIRTIO_STATUS_DRIVER         2
#define VIRTIO_STATUS_DRIVER_OK      4
#define VIRTIO_STATUS_FEATURES_OK    8
#define VIRTIO_STATUS_FAILED         128

/* ------------------------------------------------------------------ */
/*  VirtIO common configuration (mapped via VIRTIO_PCI_CAP_COMMON_CFG)*/
/* ------------------------------------------------------------------ */

/* Offsets within the common configuration structure */
#define VIRTIO_COMMON_DFSELECT       0x00  /* uint32: device feature select */
#define VIRTIO_COMMON_DF             0x04  /* uint32: device feature bits */
#define VIRTIO_COMMON_GFSELECT       0x08  /* uint32: driver (guest) feature select */
#define VIRTIO_COMMON_GF             0x0C  /* uint32: driver (guest) feature bits */
#define VIRTIO_COMMON_MSIX_CFG       0x10  /* uint16: MSI-X config vector */
#define VIRTIO_COMMON_NUM_QUEUES     0x12  /* uint16: number of queues */
#define VIRTIO_COMMON_STATUS         0x14  /* uint8: device status */
#define VIRTIO_COMMON_CFG_GEN        0x15  /* uint8: config generation */
#define VIRTIO_COMMON_Q_SELECT       0x16  /* uint16: queue select */
#define VIRTIO_COMMON_Q_SIZE         0x18  /* uint16: queue size */
#define VIRTIO_COMMON_Q_MSIX_VEC     0x1A  /* uint16: queue MSI-X vector */
#define VIRTIO_COMMON_Q_ENABLE       0x1C  /* uint16: queue enable */
#define VIRTIO_COMMON_Q_NOTIFY_OFF   0x1E  /* uint16: queue notify offset */
#define VIRTIO_COMMON_Q_DESC_LO      0x20  /* uint32: descriptor table addr low */
#define VIRTIO_COMMON_Q_DESC_HI      0x24  /* uint32: descriptor table addr high */
#define VIRTIO_COMMON_Q_AVAIL_LO     0x28  /* uint32: available ring addr low */
#define VIRTIO_COMMON_Q_AVAIL_HI     0x2C  /* uint32: available ring addr high */
#define VIRTIO_COMMON_Q_USED_LO      0x30  /* uint32: used ring addr low */
#define VIRTIO_COMMON_Q_USED_HI      0x34  /* uint32: used ring addr high */

/* ------------------------------------------------------------------ */
/*  Virtqueue structures (in guest physical memory)                   */
/* ------------------------------------------------------------------ */

/* Virtqueue descriptor flags */
#define VIRTQ_DESC_F_NEXT      1   /* descriptor is chained (has next) */
#define VIRTQ_DESC_F_WRITE     2   /* device writes to this descriptor (response) */

/* Virtqueue descriptor (16 bytes) */
typedef struct {
    uint64_t addr;     /* guest-physical address of buffer */
    uint32_t len;      /* length of buffer in bytes */
    uint16_t flags;    /* VIRTQ_DESC_F_* */
    uint16_t next;     /* index of next descriptor if NEXT is set */
} __attribute__((packed)) virtq_desc_t;

/* Available ring (driver → device) */
typedef struct {
    uint16_t flags;
    uint16_t idx;      /* next index driver will write */
    uint16_t ring[];   /* ring of descriptor chain head indices */
} __attribute__((packed)) virtq_avail_t;

/* Used ring element */
typedef struct {
    uint32_t id;       /* descriptor chain head index */
    uint32_t len;      /* total bytes written by device */
} __attribute__((packed)) virtq_used_elem_t;

/* Used ring (device → driver) */
typedef struct {
    uint16_t flags;
    uint16_t idx;      /* next index device will write */
    virtq_used_elem_t ring[];
} __attribute__((packed)) virtq_used_t;

/* Virtqueue state (kernel bookkeeping) */
typedef struct {
    uint16_t size;         /* number of descriptors (power of 2) */
    uint16_t free_head;    /* head of free descriptor list */
    uint16_t last_used;    /* last used index we processed */
    uint16_t num_free;     /* number of free descriptors */

    virtq_desc_t  *desc;   /* descriptor table (guest-physical mapped) */
    virtq_avail_t *avail;  /* available ring */
    virtq_used_t  *used;   /* used ring */

    uint32_t desc_phys;    /* physical address of descriptor table */
    uint32_t avail_phys;   /* physical address of available ring */
    uint32_t used_phys;    /* physical address of used ring */

    uint16_t notify_off;   /* notification offset for this queue */
} virtq_t;

/* ------------------------------------------------------------------ */
/*  VirtIO transport API                                              */
/* ------------------------------------------------------------------ */

/*
 * Initialize a virtqueue: allocate descriptor table, available ring,
 * and used ring in contiguous physical memory.
 * Returns 0 on success, -1 on failure.
 */
int virtq_init(virtq_t *vq, uint16_t size);

/*
 * Free a virtqueue's allocated memory.
 */
void virtq_destroy(virtq_t *vq);

/*
 * Allocate a descriptor from the free list.
 * Returns descriptor index, or 0xFFFF if none available.
 */
uint16_t virtq_alloc_desc(virtq_t *vq);

/*
 * Return a descriptor to the free list.
 */
void virtq_free_desc(virtq_t *vq, uint16_t idx);

/*
 * Submit a descriptor chain head to the available ring.
 */
void virtq_submit(virtq_t *vq, uint16_t head);

/*
 * Check if the device has placed entries in the used ring.
 * Returns 1 if there are used entries to process, 0 otherwise.
 */
int virtq_has_used(virtq_t *vq);

/*
 * Pop a used entry (returns the descriptor chain head index and length).
 * Returns 0xFFFF if no used entries.
 */
uint16_t virtq_pop_used(virtq_t *vq, uint32_t *len_out);

#endif
