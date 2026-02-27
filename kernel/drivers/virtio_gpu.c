/*
 * VirtIO GPU driver for SpikeOS.
 *
 * Implements 2D resource management and scanout via the VirtIO GPU
 * protocol. Uses the modern PCI transport (capability-based MMIO).
 *
 * Supports: GET_DISPLAY_INFO, RESOURCE_CREATE_2D, RESOURCE_ATTACH_BACKING,
 * TRANSFER_TO_HOST_2D, SET_SCANOUT, RESOURCE_FLUSH, RESOURCE_UNREF.
 */

#include <kernel/virtio.h>
#include <kernel/virtio_gpu.h>
#include <kernel/pci.h>
#include <kernel/paging.h>
#include <kernel/hal.h>
#include <kernel/heap.h>
#include <kernel/uart.h>
#include <stdio.h>
#include <string.h>

/* Serial debug helper — writes string to COM1 for .debug.log */
static void serial_puts(const char *s) {
    while (*s) uart_write((uint8_t)*s++);
}
static void serial_hex(uint32_t val) {
    const char *hex = "0123456789abcdef";
    serial_puts("0x");
    for (int i = 28; i >= 0; i -= 4)
        uart_write(hex[(val >> i) & 0xF]);
}

/* ------------------------------------------------------------------ */
/*  Internal state                                                    */
/* ------------------------------------------------------------------ */

static volatile uint8_t *common_cfg;    /* VIRTIO_PCI_CAP_COMMON_CFG mapped addr */
static volatile uint8_t *notify_base;   /* VIRTIO_PCI_CAP_NOTIFY_CFG mapped addr */
static volatile uint8_t *isr_cfg;       /* VIRTIO_PCI_CAP_ISR_CFG mapped addr */
static volatile uint8_t *device_cfg;    /* VIRTIO_PCI_CAP_DEVICE_CFG mapped addr */
static uint32_t notify_off_multiplier;  /* from notify cap */

static virtq_t controlq;               /* queue 0: GPU commands */
static int gpu_ready = 0;
static int gpu_has_virgl = 0;          /* 1 if VIRTIO_GPU_F_VIRGL negotiated */

/* VirtIO GPU feature bits */
#define VIRTIO_GPU_F_VIRGL  0          /* bit 0: VirGL 3D support */

/* Display info cached from GET_DISPLAY_INFO */
static uint32_t display_width = 0;
static uint32_t display_height = 0;

/* Command/response buffer (physically contiguous, reusable) */
#define CMD_BUF_SIZE 4096
static uint8_t  *cmd_buf;              /* virtual address */
static uint32_t  cmd_buf_phys;         /* physical address */

/* ------------------------------------------------------------------ */
/*  MMIO helpers                                                      */
/* ------------------------------------------------------------------ */

static inline void mmio_write8(volatile uint8_t *base, uint32_t off, uint8_t val) {
    base[off] = val;
}
static inline void mmio_write16(volatile uint8_t *base, uint32_t off, uint16_t val) {
    *(volatile uint16_t *)(base + off) = val;
}
static inline void mmio_write32(volatile uint8_t *base, uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(base + off) = val;
}
static inline uint8_t mmio_read8(volatile uint8_t *base, uint32_t off) {
    return base[off];
}
static inline uint16_t mmio_read16(volatile uint8_t *base, uint32_t off) {
    return *(volatile uint16_t *)(base + off);
}
static inline uint32_t mmio_read32(volatile uint8_t *base, uint32_t off) {
    return *(volatile uint32_t *)(base + off);
}

/* ------------------------------------------------------------------ */
/*  Notify the device about a virtqueue                               */
/* ------------------------------------------------------------------ */

static void virtq_notify(virtq_t *vq) {
    uint32_t off = vq->notify_off * notify_off_multiplier;
    mmio_write16(notify_base, off, 0);  /* queue index 0 */
}

/* ------------------------------------------------------------------ */
/*  Send a GPU command and wait for response (polling)                */
/* ------------------------------------------------------------------ */

static int gpu_send_cmd(void *cmd, uint32_t cmd_size,
                        void *resp, uint32_t resp_size) {
    /* Copy command into the DMA-accessible buffer */
    memcpy(cmd_buf, cmd, cmd_size);

    /* Set up a 2-descriptor chain: [0]=request (device reads), [1]=response (device writes) */
    uint16_t d0 = virtq_alloc_desc(&controlq);
    uint16_t d1 = virtq_alloc_desc(&controlq);
    if (d0 == 0xFFFF || d1 == 0xFFFF) return -1;

    /* Descriptor 0: command (device reads from this) */
    controlq.desc[d0].addr  = cmd_buf_phys;
    controlq.desc[d0].len   = cmd_size;
    controlq.desc[d0].flags = VIRTQ_DESC_F_NEXT;
    controlq.desc[d0].next  = d1;

    /* Descriptor 1: response (device writes to this) */
    uint32_t resp_offset = (cmd_size + 15) & ~15u;  /* 16-byte align response */
    controlq.desc[d1].addr  = cmd_buf_phys + resp_offset;
    controlq.desc[d1].len   = resp_size;
    controlq.desc[d1].flags = VIRTQ_DESC_F_WRITE;
    controlq.desc[d1].next  = 0xFFFF;

    /* Submit and notify */
    virtq_submit(&controlq, d0);
    virtq_notify(&controlq);

    /* Poll for completion */
    int timeout = 1000000;
    while (!virtq_has_used(&controlq) && --timeout > 0) {
        __asm__ volatile("pause");
    }

    if (timeout == 0) {
        virtq_free_desc(&controlq, d1);
        virtq_free_desc(&controlq, d0);
        return -1;
    }

    /* Pop the used entry */
    uint32_t used_len;
    virtq_pop_used(&controlq, &used_len);

    /* Copy response back to caller */
    memcpy(resp, cmd_buf + resp_offset, resp_size);

    /* Free descriptors */
    virtq_free_desc(&controlq, d1);
    virtq_free_desc(&controlq, d0);

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Find VirtIO PCI capabilities and map them                         */
/* ------------------------------------------------------------------ */

static int map_virtio_caps(pci_device_t *dev) {
    serial_puts("[virtio-gpu] mapping caps, count=");
    serial_hex(dev->cap_count);
    serial_puts("\n");

    /* Walk all vendor-specific capabilities looking for VirtIO cap types */
    for (int i = 0; i < dev->cap_count; i++) {
        if (dev->caps[i].id != PCI_CAP_ID_VENDOR) continue;

        uint8_t off = dev->caps[i].offset;

        /* Read the VirtIO PCI capability fields from config space */
        uint8_t cfg_type = pci_config_read8(dev->bus, dev->slot, dev->func, off + 3);
        uint8_t bar_idx  = pci_config_read8(dev->bus, dev->slot, dev->func, off + 4);
        uint32_t bar_off = pci_config_read32(dev->bus, dev->slot, dev->func, off + 8);
        uint32_t bar_len = pci_config_read32(dev->bus, dev->slot, dev->func, off + 12);

        serial_puts("  cap: type=");
        serial_hex(cfg_type);
        serial_puts(" bar=");
        serial_hex(bar_idx);
        serial_puts(" off=");
        serial_hex(bar_off);
        serial_puts(" len=");
        serial_hex(bar_len);
        serial_puts("\n");

        /* Get the BAR physical address */
        uint32_t bar_phys = pci_bar_addr(dev, bar_idx);
        if (bar_phys == 0) {
            serial_puts("  -> bar_phys=0, skip\n");
            continue;
        }

        /* Map the BAR region if not already mapped */
        uint32_t region_phys = bar_phys + bar_off;
        uint32_t virt;
        serial_puts("  -> region_phys=");
        serial_hex(region_phys);
        serial_puts("\n");

        if (map_mmio_region(region_phys, bar_len, &virt) != 0) {
            serial_puts("  -> map failed\n");
            continue;
        }

        volatile uint8_t *mapped = (volatile uint8_t *)virt;

        switch (cfg_type) {
        case VIRTIO_PCI_CAP_COMMON_CFG:
            common_cfg = mapped;
            serial_puts("  -> COMMON_CFG\n");
            break;
        case VIRTIO_PCI_CAP_NOTIFY_CFG:
            notify_base = mapped;
            /* Read notify_off_multiplier from bytes 16-19 of the cap */
            notify_off_multiplier = pci_config_read32(dev->bus, dev->slot,
                                                      dev->func, off + 16);
            serial_puts("  -> NOTIFY_CFG mult=");
            serial_hex(notify_off_multiplier);
            serial_puts("\n");
            break;
        case VIRTIO_PCI_CAP_ISR_CFG:
            isr_cfg = mapped;
            serial_puts("  -> ISR_CFG\n");
            break;
        case VIRTIO_PCI_CAP_DEVICE_CFG:
            device_cfg = mapped;
            serial_puts("  -> DEVICE_CFG\n");
            break;
        default:
            serial_puts("  -> unknown type\n");
            break;
        }
    }

    serial_puts("[virtio-gpu] common=");
    serial_hex((uint32_t)common_cfg);
    serial_puts(" notify=");
    serial_hex((uint32_t)notify_base);
    serial_puts(" isr=");
    serial_hex((uint32_t)isr_cfg);
    serial_puts("\n");

    return (common_cfg && notify_base && isr_cfg) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/*  Device initialization (VirtIO 1.1 section 3.1)                    */
/* ------------------------------------------------------------------ */

static int virtio_device_init(pci_device_t *dev) {
    (void)dev;
    /* 1. Reset device */
    mmio_write8(common_cfg, VIRTIO_COMMON_STATUS, 0);
    /* Small delay for reset */
    for (volatile int i = 0; i < 10000; i++);

    /* 2. Set ACKNOWLEDGE */
    mmio_write8(common_cfg, VIRTIO_COMMON_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);

    /* 3. Set DRIVER */
    uint8_t status = mmio_read8(common_cfg, VIRTIO_COMMON_STATUS);
    mmio_write8(common_cfg, VIRTIO_COMMON_STATUS, status | VIRTIO_STATUS_DRIVER);

    /* 4. Negotiate features: request VIRGL if device supports it */
    mmio_write32(common_cfg, VIRTIO_COMMON_DFSELECT, 0);
    uint32_t dev_features = mmio_read32(common_cfg, VIRTIO_COMMON_DF);
    serial_puts("[virtio-gpu] device features=");
    serial_hex(dev_features);
    serial_puts("\n");

    uint32_t guest_features = 0;
    if (dev_features & (1u << VIRTIO_GPU_F_VIRGL)) {
        guest_features |= (1u << VIRTIO_GPU_F_VIRGL);
        gpu_has_virgl = 1;
        serial_puts("[virtio-gpu] VIRGL 3D supported\n");
    } else {
        serial_puts("[virtio-gpu] VIRGL 3D not available (2D only)\n");
    }
    mmio_write32(common_cfg, VIRTIO_COMMON_GFSELECT, 0);
    mmio_write32(common_cfg, VIRTIO_COMMON_GF, guest_features);

    /* 5. Set FEATURES_OK */
    status = mmio_read8(common_cfg, VIRTIO_COMMON_STATUS);
    mmio_write8(common_cfg, VIRTIO_COMMON_STATUS, status | VIRTIO_STATUS_FEATURES_OK);

    /* 6. Re-read status to confirm FEATURES_OK is still set */
    status = mmio_read8(common_cfg, VIRTIO_COMMON_STATUS);
    if (!(status & VIRTIO_STATUS_FEATURES_OK)) {
        printf("[virtio-gpu] features negotiation failed\n");
        mmio_write8(common_cfg, VIRTIO_COMMON_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }

    /* 7. Set up controlq (queue 0) */
    mmio_write16(common_cfg, VIRTIO_COMMON_Q_SELECT, 0);
    uint16_t qsize = mmio_read16(common_cfg, VIRTIO_COMMON_Q_SIZE);
    if (qsize == 0) qsize = 64;
    if (qsize > 256) qsize = 256;  /* cap at reasonable size */

    if (virtq_init(&controlq, qsize) != 0) {
        printf("[virtio-gpu] failed to allocate controlq\n");
        mmio_write8(common_cfg, VIRTIO_COMMON_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }

    /* Get notification offset for this queue */
    controlq.notify_off = mmio_read16(common_cfg, VIRTIO_COMMON_Q_NOTIFY_OFF);

    /* Write queue addresses to device */
    mmio_write32(common_cfg, VIRTIO_COMMON_Q_DESC_LO,  controlq.desc_phys);
    mmio_write32(common_cfg, VIRTIO_COMMON_Q_DESC_HI,  0);
    mmio_write32(common_cfg, VIRTIO_COMMON_Q_AVAIL_LO, controlq.avail_phys);
    mmio_write32(common_cfg, VIRTIO_COMMON_Q_AVAIL_HI, 0);
    mmio_write32(common_cfg, VIRTIO_COMMON_Q_USED_LO,  controlq.used_phys);
    mmio_write32(common_cfg, VIRTIO_COMMON_Q_USED_HI,  0);

    /* Disable MSI-X for this queue (use legacy interrupt) */
    mmio_write16(common_cfg, VIRTIO_COMMON_Q_MSIX_VEC, 0xFFFF);

    /* Enable the queue */
    mmio_write16(common_cfg, VIRTIO_COMMON_Q_ENABLE, 1);

    /* Suppress device-generated interrupts via the available ring flags.
     * We use polling for command completion, and the PCI IRQ line may be
     * shared with other devices (e.g. e1000 NIC) — installing our own
     * handler would replace theirs and break their interrupt handling. */
    controlq.avail->flags = 1;  /* VIRTQ_AVAIL_F_NO_INTERRUPT */

    /* 9. Set DRIVER_OK — device is live */
    status = mmio_read8(common_cfg, VIRTIO_COMMON_STATUS);
    mmio_write8(common_cfg, VIRTIO_COMMON_STATUS, status | VIRTIO_STATUS_DRIVER_OK);

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

int virtio_gpu_init(void) {
    serial_puts("[virtio-gpu] init start\n");

    /* Try to find VirtIO GPU on PCI bus */
    pci_device_t *dev = pci_find_device(VIRTIO_PCI_VENDOR, VIRTIO_PCI_DEV_GPU);
    if (!dev) {
        serial_puts("[virtio-gpu] device not found\n");
        return -1;
    }

    serial_puts("[virtio-gpu] found, IRQ=");
    serial_hex(dev->irq_line);
    serial_puts("\n");

    /* Enable PCI bus mastering */
    pci_enable_bus_master(dev);

    /* Map VirtIO PCI capabilities */
    if (map_virtio_caps(dev) != 0) {
        serial_puts("[virtio-gpu] failed to map capabilities\n");
        return -1;
    }
    serial_puts("[virtio-gpu] caps mapped\n");

    /* Allocate DMA command buffer */
    cmd_buf_phys = alloc_frames_contiguous(1, 1);
    if (cmd_buf_phys == FRAME_ALLOC_FAIL) {
        serial_puts("[virtio-gpu] failed to alloc cmd buffer\n");
        return -1;
    }
    uint32_t cmd_virt;
    if (map_mmio_region(cmd_buf_phys, CMD_BUF_SIZE, &cmd_virt) != 0) {
        serial_puts("[virtio-gpu] failed to map cmd buffer\n");
        return -1;
    }
    cmd_buf = (uint8_t *)cmd_virt;
    memset(cmd_buf, 0, CMD_BUF_SIZE);
    serial_puts("[virtio-gpu] cmd buf at ");
    serial_hex(cmd_buf_phys);
    serial_puts("\n");

    /* Initialize the VirtIO device */
    if (virtio_device_init(dev) != 0) {
        serial_puts("[virtio-gpu] device init failed\n");
        return -1;
    }
    serial_puts("[virtio-gpu] device init OK\n");

    /* Note: we deliberately do NOT unmask the device's IRQ.
     * We use polling for command completion. Unmasking would cause an
     * IRQ storm that blocks the timer and other interrupts. */

    /* Query display info */
    virtio_gpu_ctrl_hdr_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;

    virtio_gpu_resp_display_info_t resp;
    memset(&resp, 0, sizeof(resp));

    serial_puts("[virtio-gpu] sending GET_DISPLAY_INFO\n");
    if (gpu_send_cmd(&cmd, sizeof(cmd), &resp, sizeof(resp)) == 0) {
        serial_puts("[virtio-gpu] resp type=");
        serial_hex(resp.hdr.type);
        serial_puts("\n");
        if (resp.hdr.type == VIRTIO_GPU_RESP_OK_DISPLAY_INFO) {
            for (int i = 0; i < VIRTIO_GPU_MAX_SCANOUTS; i++) {
                if (resp.pmodes[i].enabled) {
                    display_width  = resp.pmodes[i].r.width;
                    display_height = resp.pmodes[i].r.height;
                    serial_puts("[virtio-gpu] display: ");
                    serial_hex(display_width);
                    serial_puts("x");
                    serial_hex(display_height);
                    serial_puts("\n");
                    break;
                }
            }
        }
    } else {
        serial_puts("[virtio-gpu] GET_DISPLAY_INFO failed\n");
    }

    gpu_ready = 1;
    serial_puts("[virtio-gpu] ready\n");

    return 0;
}

int virtio_gpu_get_display_size(uint32_t *width, uint32_t *height) {
    if (!gpu_ready || display_width == 0) return -1;
    if (width) *width = display_width;
    if (height) *height = display_height;
    return 0;
}

int virtio_gpu_create_resource(uint32_t resource_id, uint32_t format,
                               uint32_t width, uint32_t height) {
    if (!gpu_ready) return -1;

    virtio_gpu_resource_create_2d_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    cmd.resource_id = resource_id;
    cmd.format      = format;
    cmd.width       = width;
    cmd.height      = height;

    virtio_gpu_ctrl_hdr_t resp;
    memset(&resp, 0, sizeof(resp));

    if (gpu_send_cmd(&cmd, sizeof(cmd), &resp, sizeof(resp)) != 0) return -1;
    return (resp.type == VIRTIO_GPU_RESP_OK_NODATA) ? 0 : -1;
}

int virtio_gpu_attach_backing(uint32_t resource_id,
                              uint32_t phys_addr, uint32_t size) {
    if (!gpu_ready) return -1;

    /* Build the attach_backing command + 1 mem entry in cmd_buf directly
     * since the struct has a variable-length trailing array */
    virtio_gpu_resource_attach_backing_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    cmd.resource_id = resource_id;
    cmd.nr_entries  = 1;

    virtio_gpu_mem_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    entry.addr   = phys_addr;
    entry.length = size;

    /* Pack command + entry into cmd_buf */
    uint32_t total_cmd_size = sizeof(cmd) + sizeof(entry);
    memcpy(cmd_buf, &cmd, sizeof(cmd));
    memcpy(cmd_buf + sizeof(cmd), &entry, sizeof(entry));

    /* Response area after the command */
    uint32_t resp_offset = (total_cmd_size + 15) & ~15u;
    memset(cmd_buf + resp_offset, 0, sizeof(virtio_gpu_ctrl_hdr_t));

    /* Build descriptor chain manually */
    uint16_t d0 = virtq_alloc_desc(&controlq);
    uint16_t d1 = virtq_alloc_desc(&controlq);
    if (d0 == 0xFFFF || d1 == 0xFFFF) return -1;

    controlq.desc[d0].addr  = cmd_buf_phys;
    controlq.desc[d0].len   = total_cmd_size;
    controlq.desc[d0].flags = VIRTQ_DESC_F_NEXT;
    controlq.desc[d0].next  = d1;

    controlq.desc[d1].addr  = cmd_buf_phys + resp_offset;
    controlq.desc[d1].len   = sizeof(virtio_gpu_ctrl_hdr_t);
    controlq.desc[d1].flags = VIRTQ_DESC_F_WRITE;
    controlq.desc[d1].next  = 0xFFFF;

    virtq_submit(&controlq, d0);
    virtq_notify(&controlq);

    /* Poll for completion */
    int timeout = 1000000;
    while (!virtq_has_used(&controlq) && --timeout > 0)
        __asm__ volatile("pause");

    uint32_t used_len;
    virtq_pop_used(&controlq, &used_len);

    virtio_gpu_ctrl_hdr_t *resp = (virtio_gpu_ctrl_hdr_t *)(cmd_buf + resp_offset);
    int result = (resp->type == VIRTIO_GPU_RESP_OK_NODATA) ? 0 : -1;

    virtq_free_desc(&controlq, d1);
    virtq_free_desc(&controlq, d0);

    return result;
}

int virtio_gpu_transfer_to_host(uint32_t resource_id,
                                uint32_t x, uint32_t y,
                                uint32_t w, uint32_t h) {
    if (!gpu_ready) return -1;

    virtio_gpu_transfer_to_host_2d_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type    = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    cmd.r.x         = x;
    cmd.r.y         = y;
    cmd.r.width     = w;
    cmd.r.height    = h;
    cmd.offset      = 0;
    cmd.resource_id = resource_id;

    virtio_gpu_ctrl_hdr_t resp;
    memset(&resp, 0, sizeof(resp));

    if (gpu_send_cmd(&cmd, sizeof(cmd), &resp, sizeof(resp)) != 0) return -1;
    return (resp.type == VIRTIO_GPU_RESP_OK_NODATA) ? 0 : -1;
}

int virtio_gpu_set_scanout(uint32_t resource_id,
                           uint32_t x, uint32_t y,
                           uint32_t w, uint32_t h) {
    if (!gpu_ready) return -1;

    virtio_gpu_set_scanout_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type    = VIRTIO_GPU_CMD_SET_SCANOUT;
    cmd.r.x         = x;
    cmd.r.y         = y;
    cmd.r.width     = w;
    cmd.r.height    = h;
    cmd.scanout_id  = 0;  /* display 0 */
    cmd.resource_id = resource_id;

    virtio_gpu_ctrl_hdr_t resp;
    memset(&resp, 0, sizeof(resp));

    if (gpu_send_cmd(&cmd, sizeof(cmd), &resp, sizeof(resp)) != 0) return -1;
    return (resp.type == VIRTIO_GPU_RESP_OK_NODATA) ? 0 : -1;
}

int virtio_gpu_flush(uint32_t resource_id,
                     uint32_t x, uint32_t y,
                     uint32_t w, uint32_t h) {
    if (!gpu_ready) return -1;

    virtio_gpu_resource_flush_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    cmd.r.x         = x;
    cmd.r.y         = y;
    cmd.r.width     = w;
    cmd.r.height    = h;
    cmd.resource_id = resource_id;

    virtio_gpu_ctrl_hdr_t resp;
    memset(&resp, 0, sizeof(resp));

    if (gpu_send_cmd(&cmd, sizeof(cmd), &resp, sizeof(resp)) != 0) return -1;
    return (resp.type == VIRTIO_GPU_RESP_OK_NODATA) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/*  Fence support                                                     */
/* ------------------------------------------------------------------ */

static uint64_t next_fence_id = 1;

/*
 * Send a GPU command with a fence attached.
 * The device will signal completion by returning the fence_id
 * in the response header. We poll until the command completes.
 */
static int gpu_send_cmd_fenced(void *cmd, uint32_t cmd_size,
                               void *resp, uint32_t resp_size) {
    /* Set fence flag and ID on the command header */
    virtio_gpu_ctrl_hdr_t *hdr = (virtio_gpu_ctrl_hdr_t *)cmd;
    hdr->flags   = VIRTIO_GPU_FLAG_FENCE;
    hdr->fence_id = next_fence_id++;

    return gpu_send_cmd(cmd, cmd_size, resp, resp_size);
}

/*
 * Transfer to host with fence — waits for DMA completion.
 */
static int virtio_gpu_transfer_to_host_fenced(uint32_t resource_id,
                                               uint32_t x, uint32_t y,
                                               uint32_t w, uint32_t h) {
    if (!gpu_ready) return -1;

    virtio_gpu_transfer_to_host_2d_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type    = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    cmd.r.x         = x;
    cmd.r.y         = y;
    cmd.r.width     = w;
    cmd.r.height    = h;
    cmd.offset      = 0;
    cmd.resource_id = resource_id;

    virtio_gpu_ctrl_hdr_t resp;
    memset(&resp, 0, sizeof(resp));

    if (gpu_send_cmd_fenced(&cmd, sizeof(cmd), &resp, sizeof(resp)) != 0) return -1;
    return (resp.type == VIRTIO_GPU_RESP_OK_NODATA) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/*  Double-buffered scanout management                                */
/* ------------------------------------------------------------------ */

#define SCANOUT_RES_FRONT  1
#define SCANOUT_RES_BACK   2

typedef struct {
    uint32_t  resource_id;
    uint32_t  phys;       /* physical address of backing buffer */
    uint32_t *pixels;     /* virtual address of backing buffer */
} gpu_buffer_t;

static gpu_buffer_t buf[2];         /* [0] = front, [1] = back */
static int back_idx = 1;            /* index of current back buffer */
static uint32_t scanout_width = 0;
static uint32_t scanout_height = 0;
static uint32_t scanout_fb_size = 0;
static uint32_t scanout_num_pages = 0;
static int scanout_active = 0;

/*
 * Allocate a single GPU buffer: contiguous physical memory + resource + backing.
 */
static int alloc_gpu_buffer(gpu_buffer_t *b, uint32_t res_id,
                            uint32_t w, uint32_t h, uint32_t fb_size,
                            uint32_t num_pages) {
    b->resource_id = res_id;

    b->phys = alloc_frames_contiguous(num_pages, 1);
    if (b->phys == FRAME_ALLOC_FAIL) {
        serial_puts("[virtio-gpu] failed to alloc buffer\n");
        return -1;
    }

    uint32_t virt;
    if (map_mmio_region(b->phys, fb_size, &virt) != 0) {
        serial_puts("[virtio-gpu] failed to map buffer\n");
        free_frames_contiguous(b->phys, num_pages);
        return -1;
    }
    b->pixels = (uint32_t *)virt;
    memset(b->pixels, 0, fb_size);

    serial_puts("[virtio-gpu] buf res=");
    serial_hex(res_id);
    serial_puts(" phys=");
    serial_hex(b->phys);
    serial_puts(" virt=");
    serial_hex(virt);
    serial_puts("\n");

    /* Create GPU resource */
    if (virtio_gpu_create_resource(res_id, VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM,
                                   w, h) != 0) {
        serial_puts("[virtio-gpu] create resource failed\n");
        return -1;
    }

    /* Attach backing memory */
    if (virtio_gpu_attach_backing(res_id, b->phys, fb_size) != 0) {
        serial_puts("[virtio-gpu] attach backing failed\n");
        return -1;
    }

    return 0;
}

int virtio_gpu_setup_scanout(void) {
    if (!gpu_ready || display_width == 0 || display_height == 0) return -1;

    uint32_t w = display_width;
    uint32_t h = display_height;
    scanout_fb_size = w * h * 4;  /* XRGB8888 */
    scanout_num_pages = (scanout_fb_size + PAGE_SIZE - 1) / PAGE_SIZE;

    serial_puts("[virtio-gpu] setup double-buffered scanout ");
    serial_hex(w);
    serial_puts("x");
    serial_hex(h);
    serial_puts("\n");

    /* Allocate front and back buffers */
    if (alloc_gpu_buffer(&buf[0], SCANOUT_RES_FRONT, w, h,
                         scanout_fb_size, scanout_num_pages) != 0)
        return -1;
    serial_puts("[virtio-gpu] front buffer ready\n");

    if (alloc_gpu_buffer(&buf[1], SCANOUT_RES_BACK, w, h,
                         scanout_fb_size, scanout_num_pages) != 0)
        return -1;
    serial_puts("[virtio-gpu] back buffer ready\n");

    /* Set front buffer as the initial scanout */
    if (virtio_gpu_set_scanout(SCANOUT_RES_FRONT, 0, 0, w, h) != 0) {
        serial_puts("[virtio-gpu] set scanout failed\n");
        return -1;
    }

    scanout_width = w;
    scanout_height = h;
    back_idx = 1;
    scanout_active = 1;

    serial_puts("[virtio-gpu] double-buffered scanout active\n");
    return 0;
}

void virtio_gpu_present(surface_t *compositor) {
    if (!scanout_active || !compositor || !compositor->pixels) return;

    gpu_buffer_t *back = &buf[back_idx];
    uint32_t w = scanout_width;
    uint32_t h = scanout_height;
    if (compositor->width < w) w = compositor->width;
    if (compositor->height < h) h = compositor->height;

    /* Copy compositor pixels to back buffer */
    if (compositor->width == scanout_width) {
        memcpy(back->pixels, compositor->pixels, w * h * 4);
    } else {
        for (uint32_t row = 0; row < h; row++) {
            memcpy(&back->pixels[row * scanout_width],
                   &compositor->pixels[row * compositor->width],
                   w * 4);
        }
    }

    /* Transfer back buffer to host with fence (waits for DMA completion) */
    virtio_gpu_transfer_to_host_fenced(back->resource_id, 0, 0,
                                       scanout_width, scanout_height);

    /* Swap: set back buffer as the active scanout */
    virtio_gpu_set_scanout(back->resource_id, 0, 0,
                           scanout_width, scanout_height);

    /* Flush to display (vsync point) */
    virtio_gpu_flush(back->resource_id, 0, 0,
                     scanout_width, scanout_height);

    /* Swap buffer indices: old back becomes front, old front becomes back */
    back_idx = 1 - back_idx;
}

int virtio_gpu_scanout_active(void) {
    return scanout_active;
}

int virtio_gpu_has_virgl(void) {
    return gpu_has_virgl;
}

/* ------------------------------------------------------------------ */
/*  3D / VirGL API                                                    */
/* ------------------------------------------------------------------ */

#include <kernel/virgl.h>

int virtio_gpu_ctx_create(uint32_t ctx_id, const char *debug_name) {
    if (!gpu_ready) return -1;

    virtio_gpu_ctx_create_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type   = VIRTIO_GPU_CMD_CTX_CREATE;
    cmd.hdr.ctx_id = ctx_id;
    cmd.nlen = 0;
    if (debug_name) {
        uint32_t len = 0;
        while (debug_name[len] && len < 63) {
            cmd.debug_name[len] = debug_name[len];
            len++;
        }
        cmd.nlen = len;
    }

    virtio_gpu_ctrl_hdr_t resp;
    memset(&resp, 0, sizeof(resp));

    if (gpu_send_cmd(&cmd, sizeof(cmd), &resp, sizeof(resp)) != 0) return -1;
    return (resp.type == VIRTIO_GPU_RESP_OK_NODATA) ? 0 : -1;
}

int virtio_gpu_ctx_destroy(uint32_t ctx_id) {
    if (!gpu_ready) return -1;

    virtio_gpu_ctrl_hdr_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type   = VIRTIO_GPU_CMD_CTX_DESTROY;
    cmd.ctx_id = ctx_id;

    virtio_gpu_ctrl_hdr_t resp;
    memset(&resp, 0, sizeof(resp));

    if (gpu_send_cmd(&cmd, sizeof(cmd), &resp, sizeof(resp)) != 0) return -1;
    return (resp.type == VIRTIO_GPU_RESP_OK_NODATA) ? 0 : -1;
}

int virtio_gpu_ctx_attach_resource(uint32_t ctx_id, uint32_t resource_id) {
    if (!gpu_ready) return -1;

    virtio_gpu_ctx_resource_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type       = VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE;
    cmd.hdr.ctx_id     = ctx_id;
    cmd.resource_id    = resource_id;

    virtio_gpu_ctrl_hdr_t resp;
    memset(&resp, 0, sizeof(resp));

    if (gpu_send_cmd(&cmd, sizeof(cmd), &resp, sizeof(resp)) != 0) return -1;
    return (resp.type == VIRTIO_GPU_RESP_OK_NODATA) ? 0 : -1;
}

int virtio_gpu_ctx_detach_resource(uint32_t ctx_id, uint32_t resource_id) {
    if (!gpu_ready) return -1;

    virtio_gpu_ctx_resource_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type       = VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE;
    cmd.hdr.ctx_id     = ctx_id;
    cmd.resource_id    = resource_id;

    virtio_gpu_ctrl_hdr_t resp;
    memset(&resp, 0, sizeof(resp));

    if (gpu_send_cmd(&cmd, sizeof(cmd), &resp, sizeof(resp)) != 0) return -1;
    return (resp.type == VIRTIO_GPU_RESP_OK_NODATA) ? 0 : -1;
}

int virtio_gpu_create_resource_3d(uint32_t resource_id, uint32_t target,
                                  uint32_t format, uint32_t bind,
                                  uint32_t width, uint32_t height,
                                  uint32_t depth, uint32_t array_size,
                                  uint32_t last_level, uint32_t nr_samples) {
    if (!gpu_ready) return -1;

    virtio_gpu_resource_create_3d_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
    cmd.resource_id = resource_id;
    cmd.target      = target;
    cmd.format      = format;
    cmd.bind        = bind;
    cmd.width       = width;
    cmd.height      = height;
    cmd.depth       = depth;
    cmd.array_size  = array_size;
    cmd.last_level  = last_level;
    cmd.nr_samples  = nr_samples;

    virtio_gpu_ctrl_hdr_t resp;
    memset(&resp, 0, sizeof(resp));

    if (gpu_send_cmd(&cmd, sizeof(cmd), &resp, sizeof(resp)) != 0) return -1;
    return (resp.type == VIRTIO_GPU_RESP_OK_NODATA) ? 0 : -1;
}

/*
 * Submit a VirGL 3D command buffer.
 * The command data is copied into our DMA buffer inline after the header.
 */
int virtio_gpu_submit_3d(uint32_t ctx_id, const uint32_t *cmdbuf,
                         uint32_t size_bytes) {
    if (!gpu_ready || !cmdbuf || size_bytes == 0) return -1;

    /* Build the submit header */
    virtio_gpu_cmd_submit_t submit_hdr;
    memset(&submit_hdr, 0, sizeof(submit_hdr));
    submit_hdr.hdr.type   = VIRTIO_GPU_CMD_SUBMIT_3D;
    submit_hdr.hdr.ctx_id = ctx_id;
    submit_hdr.size       = size_bytes;

    /* Pack header + command data into cmd_buf */
    uint32_t total_cmd_size = sizeof(submit_hdr) + size_bytes;
    if (total_cmd_size > CMD_BUF_SIZE / 2) return -1;  /* too large */

    memcpy(cmd_buf, &submit_hdr, sizeof(submit_hdr));
    memcpy(cmd_buf + sizeof(submit_hdr), cmdbuf, size_bytes);

    /* Response area after the command */
    uint32_t resp_offset = (total_cmd_size + 15) & ~15u;
    memset(cmd_buf + resp_offset, 0, sizeof(virtio_gpu_ctrl_hdr_t));

    /* Build descriptor chain manually (like attach_backing) */
    uint16_t d0 = virtq_alloc_desc(&controlq);
    uint16_t d1 = virtq_alloc_desc(&controlq);
    if (d0 == 0xFFFF || d1 == 0xFFFF) return -1;

    controlq.desc[d0].addr  = cmd_buf_phys;
    controlq.desc[d0].len   = total_cmd_size;
    controlq.desc[d0].flags = VIRTQ_DESC_F_NEXT;
    controlq.desc[d0].next  = d1;

    controlq.desc[d1].addr  = cmd_buf_phys + resp_offset;
    controlq.desc[d1].len   = sizeof(virtio_gpu_ctrl_hdr_t);
    controlq.desc[d1].flags = VIRTQ_DESC_F_WRITE;
    controlq.desc[d1].next  = 0xFFFF;

    virtq_submit(&controlq, d0);
    virtq_notify(&controlq);

    /* Poll for completion */
    int timeout = 1000000;
    while (!virtq_has_used(&controlq) && --timeout > 0)
        __asm__ volatile("pause");

    uint32_t used_len;
    virtq_pop_used(&controlq, &used_len);

    virtio_gpu_ctrl_hdr_t *resp = (virtio_gpu_ctrl_hdr_t *)(cmd_buf + resp_offset);
    int result = (resp->type == VIRTIO_GPU_RESP_OK_NODATA) ? 0 : -1;

    if (result != 0) {
        serial_puts("[virgl] submit_3d failed, resp=");
        serial_hex(resp->type);
        serial_puts("\n");
    }

    virtq_free_desc(&controlq, d1);
    virtq_free_desc(&controlq, d0);

    return result;
}

/* ------------------------------------------------------------------ */
/*  VirGL triangle demo                                               */
/* ------------------------------------------------------------------ */

/* Helper to append a uint32 word to a command buffer */
static uint32_t *virgl_emit(uint32_t *p, uint32_t val) {
    *p++ = val;
    return p;
}

/* Helper to pack a TGSI text string into uint32 words (little-endian) */
static uint32_t *virgl_emit_shader_text(uint32_t *p, const char *tgsi) {
    uint32_t len = 0;
    while (tgsi[len]) len++;

    /* Shader header in CREATE_OBJECT: handle, type, text_len, num_tokens, num_so */
    /* The text_len field encodes the string length (bit 31=0 for single chunk) */

    uint32_t words = (len + 3) / 4;
    for (uint32_t i = 0; i < words; i++) {
        uint32_t w = 0;
        for (int b = 0; b < 4; b++) {
            uint32_t idx = i * 4 + b;
            if (idx < len) w |= ((uint32_t)(uint8_t)tgsi[idx]) << (b * 8);
        }
        p = virgl_emit(p, w);
    }
    return p;
}

/*
 * Draw a solid red triangle using the VirGL 3D pipeline.
 * This is a kernel-mode demo — no userspace or Mesa involved.
 */
void virtio_gpu_3d_demo(void) {
    if (!gpu_ready || display_width == 0) {
        serial_puts("[virgl-demo] GPU not ready\n");
        return;
    }
    if (!gpu_has_virgl) {
        serial_puts("[virgl-demo] VIRGL 3D not supported by host\n");
        return;
    }

    serial_puts("[virgl-demo] starting 3D triangle demo\n");

    /* Resource and object IDs (must not collide with scanout resources 1,2) */
    #define DEMO_CTX_ID        1
    #define DEMO_FB_RES_ID     10   /* framebuffer texture */
    #define DEMO_VB_RES_ID     11   /* vertex buffer */
    #define DEMO_BLEND_HANDLE  1
    #define DEMO_DSA_HANDLE    2
    #define DEMO_RAST_HANDLE   3
    #define DEMO_VS_HANDLE     4
    #define DEMO_FS_HANDLE     5
    #define DEMO_SURF_HANDLE   6
    #define DEMO_VE_HANDLE     7

    uint32_t w = display_width;
    uint32_t h = display_height;

    /* Step 1: Create VirGL context */
    if (virtio_gpu_ctx_create(DEMO_CTX_ID, "spikeos") != 0) {
        serial_puts("[virgl-demo] ctx_create failed\n");
        return;
    }
    serial_puts("[virgl-demo] context created\n");

    /* Step 2: Create 3D resources */
    /* Framebuffer texture */
    if (virtio_gpu_create_resource_3d(DEMO_FB_RES_ID, PIPE_TEXTURE_2D,
            VIRGL_FORMAT_B8G8R8A8_UNORM, VIRGL_BIND_RENDER_TARGET,
            w, h, 1, 1, 0, 0) != 0) {
        serial_puts("[virgl-demo] create fb resource failed\n");
        return;
    }

    /* Vertex buffer (3 vertices * 4 floats * 4 bytes = 48 bytes) */
    if (virtio_gpu_create_resource_3d(DEMO_VB_RES_ID, PIPE_BUFFER,
            VIRGL_FORMAT_R8G8B8A8_UNORM, VIRGL_BIND_VERTEX_BUFFER,
            48, 1, 1, 1, 0, 0) != 0) {
        serial_puts("[virgl-demo] create vb resource failed\n");
        return;
    }
    serial_puts("[virgl-demo] resources created\n");

    /* Attach backing memory for the framebuffer */
    uint32_t fb_size = w * h * 4;
    uint32_t fb_pages = (fb_size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint32_t fb_phys = alloc_frames_contiguous(fb_pages, 1);
    if (fb_phys == FRAME_ALLOC_FAIL) {
        serial_puts("[virgl-demo] alloc fb backing failed\n");
        return;
    }
    virtio_gpu_attach_backing(DEMO_FB_RES_ID, fb_phys, fb_size);

    /* Attach backing for vertex buffer */
    uint32_t vb_phys = alloc_frames_contiguous(1, 1);
    if (vb_phys == FRAME_ALLOC_FAIL) {
        serial_puts("[virgl-demo] alloc vb backing failed\n");
        return;
    }
    virtio_gpu_attach_backing(DEMO_VB_RES_ID, vb_phys, PAGE_SIZE);

    /* Attach resources to context */
    virtio_gpu_ctx_attach_resource(DEMO_CTX_ID, DEMO_FB_RES_ID);
    virtio_gpu_ctx_attach_resource(DEMO_CTX_ID, DEMO_VB_RES_ID);
    serial_puts("[virgl-demo] resources attached to context\n");

    /* Step 3: Build and submit VirGL command buffer */
    uint32_t cmdbuf[512];
    uint32_t *p = cmdbuf;

    /* --- Upload vertex data via RESOURCE_INLINE_WRITE --- */
    /* 3 vertices, 4 floats each (x, y, z, w) in clip space */
    uint32_t vtx_data_words = 12;  /* 48 bytes / 4 */
    p = virgl_emit(p, VIRGL_CMD0(VIRGL_CCMD_RESOURCE_INLINE_WRITE, 0,
                                  11 + vtx_data_words));
    p = virgl_emit(p, DEMO_VB_RES_ID);  /* resource handle */
    p = virgl_emit(p, 0);               /* level */
    p = virgl_emit(p, 0);               /* usage */
    p = virgl_emit(p, 0);               /* stride */
    p = virgl_emit(p, 0);               /* layer_stride */
    p = virgl_emit(p, 0);               /* x */
    p = virgl_emit(p, 0);               /* y */
    p = virgl_emit(p, 0);               /* z */
    p = virgl_emit(p, 48);              /* w (width in bytes) */
    p = virgl_emit(p, 1);               /* h */
    p = virgl_emit(p, 1);               /* d */
    /* Vertex 0: top center */
    p = virgl_emit(p, virgl_float_bits(0.0f));
    p = virgl_emit(p, virgl_float_bits(0.5f));
    p = virgl_emit(p, virgl_float_bits(0.0f));
    p = virgl_emit(p, virgl_float_bits(1.0f));
    /* Vertex 1: bottom left */
    p = virgl_emit(p, virgl_float_bits(-0.5f));
    p = virgl_emit(p, virgl_float_bits(-0.5f));
    p = virgl_emit(p, virgl_float_bits(0.0f));
    p = virgl_emit(p, virgl_float_bits(1.0f));
    /* Vertex 2: bottom right */
    p = virgl_emit(p, virgl_float_bits(0.5f));
    p = virgl_emit(p, virgl_float_bits(-0.5f));
    p = virgl_emit(p, virgl_float_bits(0.0f));
    p = virgl_emit(p, virgl_float_bits(1.0f));

    /* --- Create blend state (no blending, write RGBA) --- */
    p = virgl_emit(p, VIRGL_CMD0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_BLEND, 11));
    p = virgl_emit(p, DEMO_BLEND_HANDLE);
    p = virgl_emit(p, 0);                /* S0: no flags */
    p = virgl_emit(p, 0);                /* S1: logicop func */
    p = virgl_emit(p, 0x78000000u);      /* S2[0]: colormask=0xF (bits 27-30) */
    for (int i = 0; i < 7; i++)
        p = virgl_emit(p, 0);            /* S2[1-7] */

    /* --- Create depth-stencil-alpha state (all disabled) --- */
    p = virgl_emit(p, VIRGL_CMD0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_DSA, 5));
    p = virgl_emit(p, DEMO_DSA_HANDLE);
    p = virgl_emit(p, 0);                /* S0: depth disabled */
    p = virgl_emit(p, 0);                /* S1: stencil front */
    p = virgl_emit(p, 0);                /* S2: stencil back */
    p = virgl_emit(p, 0);                /* alpha ref */

    /* --- Create rasterizer state (filled, no cull) --- */
    p = virgl_emit(p, VIRGL_CMD0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_RASTERIZER, 9));
    p = virgl_emit(p, DEMO_RAST_HANDLE);
    p = virgl_emit(p, 0x00002002u);      /* depth_clip | fill_front=solid | fill_back=solid */
    p = virgl_emit(p, virgl_float_bits(1.0f));  /* point_size */
    p = virgl_emit(p, 0);                /* sprite_coord_enable */
    p = virgl_emit(p, 0);                /* S3 */
    p = virgl_emit(p, virgl_float_bits(1.0f));  /* line_width */
    p = virgl_emit(p, virgl_float_bits(0.0f));  /* offset_units */
    p = virgl_emit(p, virgl_float_bits(0.0f));  /* offset_scale */
    p = virgl_emit(p, virgl_float_bits(0.0f));  /* offset_clamp */

    /* --- Create vertex shader --- */
    static const char vs_tgsi[] =
        "VERT\n"
        "DCL IN[0]\n"
        "DCL OUT[0], POSITION\n"
        "  0: MOV OUT[0], IN[0]\n"
        "  1: END\n";
    uint32_t vs_len = 0;
    while (vs_tgsi[vs_len]) vs_len++;
    uint32_t vs_words = (vs_len + 3) / 4;
    p = virgl_emit(p, VIRGL_CMD0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SHADER,
                                  5 + vs_words));
    p = virgl_emit(p, DEMO_VS_HANDLE);
    p = virgl_emit(p, PIPE_SHADER_VERTEX);
    p = virgl_emit(p, vs_len);           /* text length */
    p = virgl_emit(p, 0);                /* num_tokens (0 = text mode) */
    p = virgl_emit(p, 0);                /* num_so_outputs */
    p = virgl_emit_shader_text(p, vs_tgsi);

    /* --- Create fragment shader --- */
    static const char fs_tgsi[] =
        "FRAG\n"
        "PROPERTY FS_COLOR0_WRITES_ALL_CBUFS 1\n"
        "DCL OUT[0], COLOR\n"
        "IMM[0] FLT32 {1.0, 0.0, 0.0, 1.0}\n"
        "  0: MOV OUT[0], IMM[0]\n"
        "  1: END\n";
    uint32_t fs_len = 0;
    while (fs_tgsi[fs_len]) fs_len++;
    uint32_t fs_words = (fs_len + 3) / 4;
    p = virgl_emit(p, VIRGL_CMD0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SHADER,
                                  5 + fs_words));
    p = virgl_emit(p, DEMO_FS_HANDLE);
    p = virgl_emit(p, PIPE_SHADER_FRAGMENT);
    p = virgl_emit(p, fs_len);
    p = virgl_emit(p, 0);
    p = virgl_emit(p, 0);
    p = virgl_emit_shader_text(p, fs_tgsi);

    /* --- Create surface (wraps FB resource) --- */
    p = virgl_emit(p, VIRGL_CMD0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SURFACE, 5));
    p = virgl_emit(p, DEMO_SURF_HANDLE);
    p = virgl_emit(p, DEMO_FB_RES_ID);   /* res_handle */
    p = virgl_emit(p, VIRGL_FORMAT_B8G8R8A8_UNORM);  /* format */
    p = virgl_emit(p, 0);                /* texture level */
    p = virgl_emit(p, 0);                /* texture layers */

    /* --- Create vertex elements (1 element: vec4 position) --- */
    p = virgl_emit(p, VIRGL_CMD0(VIRGL_CCMD_CREATE_OBJECT,
                                  VIRGL_OBJECT_VERTEX_ELEMENTS, 5));
    p = virgl_emit(p, DEMO_VE_HANDLE);
    p = virgl_emit(p, 0);                /* src_offset */
    p = virgl_emit(p, 0);                /* instance_divisor */
    p = virgl_emit(p, 0);                /* vertex_buffer_index */
    p = virgl_emit(p, VIRGL_FORMAT_R32G32B32A32_FLOAT);  /* src_format */

    /* Submit object creation batch */
    uint32_t batch1_size = (uint32_t)((uintptr_t)p - (uintptr_t)cmdbuf);
    if (virtio_gpu_submit_3d(DEMO_CTX_ID, cmdbuf, batch1_size) != 0) {
        serial_puts("[virgl-demo] object creation submit failed\n");
        return;
    }
    serial_puts("[virgl-demo] objects created\n");

    /* --- Batch 2: bind objects, set state, clear, draw --- */
    p = cmdbuf;

    /* Bind blend */
    p = virgl_emit(p, VIRGL_CMD0(VIRGL_CCMD_BIND_OBJECT, VIRGL_OBJECT_BLEND, 1));
    p = virgl_emit(p, DEMO_BLEND_HANDLE);

    /* Bind DSA */
    p = virgl_emit(p, VIRGL_CMD0(VIRGL_CCMD_BIND_OBJECT, VIRGL_OBJECT_DSA, 1));
    p = virgl_emit(p, DEMO_DSA_HANDLE);

    /* Bind rasterizer */
    p = virgl_emit(p, VIRGL_CMD0(VIRGL_CCMD_BIND_OBJECT, VIRGL_OBJECT_RASTERIZER, 1));
    p = virgl_emit(p, DEMO_RAST_HANDLE);

    /* Bind vertex elements */
    p = virgl_emit(p, VIRGL_CMD0(VIRGL_CCMD_BIND_OBJECT, VIRGL_OBJECT_VERTEX_ELEMENTS, 1));
    p = virgl_emit(p, DEMO_VE_HANDLE);

    /* Bind vertex shader */
    p = virgl_emit(p, VIRGL_CMD0(VIRGL_CCMD_BIND_SHADER, 0, 2));
    p = virgl_emit(p, DEMO_VS_HANDLE);
    p = virgl_emit(p, PIPE_SHADER_VERTEX);

    /* Bind fragment shader */
    p = virgl_emit(p, VIRGL_CMD0(VIRGL_CCMD_BIND_SHADER, 0, 2));
    p = virgl_emit(p, DEMO_FS_HANDLE);
    p = virgl_emit(p, PIPE_SHADER_FRAGMENT);

    /* Set vertex buffer (stride=16, offset=0, res=DEMO_VB_RES_ID) */
    p = virgl_emit(p, VIRGL_CMD0(VIRGL_CCMD_SET_VERTEX_BUFFERS, 0, 3));
    p = virgl_emit(p, 16);               /* stride */
    p = virgl_emit(p, 0);                /* offset */
    p = virgl_emit(p, DEMO_VB_RES_ID);   /* resource handle */

    /* Set framebuffer state (1 color buffer, no depth) */
    p = virgl_emit(p, VIRGL_CMD0(VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0, 3));
    p = virgl_emit(p, 1);                /* nr_cbufs */
    p = virgl_emit(p, 0);                /* zsurf_handle (none) */
    p = virgl_emit(p, DEMO_SURF_HANDLE); /* cbuf[0] */

    /* Set viewport */
    float half_w = (float)w / 2.0f;
    float half_h = (float)h / 2.0f;
    p = virgl_emit(p, VIRGL_CMD0(VIRGL_CCMD_SET_VIEWPORT_STATE, 0, 7));
    p = virgl_emit(p, 0);                /* start_slot */
    p = virgl_emit(p, virgl_float_bits(half_w));       /* scale_x */
    p = virgl_emit(p, virgl_float_bits(-half_h));      /* scale_y (flip Y) */
    p = virgl_emit(p, virgl_float_bits(0.5f));         /* scale_z */
    p = virgl_emit(p, virgl_float_bits(half_w));       /* translate_x */
    p = virgl_emit(p, virgl_float_bits(half_h));       /* translate_y */
    p = virgl_emit(p, virgl_float_bits(0.5f));         /* translate_z */

    /* Clear framebuffer (dark blue background) */
    p = virgl_emit(p, VIRGL_CMD0(VIRGL_CCMD_CLEAR, 0, 8));
    p = virgl_emit(p, PIPE_CLEAR_COLOR0);
    p = virgl_emit(p, virgl_float_bits(0.1f));    /* R */
    p = virgl_emit(p, virgl_float_bits(0.1f));    /* G */
    p = virgl_emit(p, virgl_float_bits(0.3f));    /* B */
    p = virgl_emit(p, virgl_float_bits(1.0f));    /* A */
    p = virgl_emit(p, 0);                         /* depth_lo */
    p = virgl_emit(p, 0);                         /* depth_hi */
    p = virgl_emit(p, 0);                         /* stencil */

    /* Draw triangle */
    p = virgl_emit(p, VIRGL_CMD0(VIRGL_CCMD_DRAW_VBO, 0, 12));
    p = virgl_emit(p, 0);                /* start */
    p = virgl_emit(p, 3);                /* count */
    p = virgl_emit(p, PIPE_PRIM_TRIANGLES);  /* mode */
    p = virgl_emit(p, 0);                /* indexed */
    p = virgl_emit(p, 1);                /* instance_count */
    p = virgl_emit(p, 0);                /* index_bias */
    p = virgl_emit(p, 0);                /* start_instance */
    p = virgl_emit(p, 0);                /* primitive_restart */
    p = virgl_emit(p, 0);                /* restart_index */
    p = virgl_emit(p, 0);                /* min_index */
    p = virgl_emit(p, 2);                /* max_index */
    p = virgl_emit(p, 0);                /* count_from_so */

    /* Submit draw batch */
    uint32_t batch2_size = (uint32_t)((uintptr_t)p - (uintptr_t)cmdbuf);
    if (virtio_gpu_submit_3d(DEMO_CTX_ID, cmdbuf, batch2_size) != 0) {
        serial_puts("[virgl-demo] draw submit failed\n");
        return;
    }
    serial_puts("[virgl-demo] draw submitted\n");

    /* Step 4: Display the result */
    /* Set the 3D framebuffer resource as scanout and flush */
    virtio_gpu_set_scanout(DEMO_FB_RES_ID, 0, 0, w, h);
    virtio_gpu_flush(DEMO_FB_RES_ID, 0, 0, w, h);

    serial_puts("[virgl-demo] triangle displayed!\n");
}
