/*
 * PCI bus enumeration for SpikeOS.
 *
 * Scans bus 0 (sufficient for QEMU and most single-bus systems),
 * stores discovered devices, provides config space read/write.
 * Parses capability lists, sizes BARs, and detects 64-bit BARs.
 */

#include <kernel/pci.h>
#include <kernel/hal.h>
#include <stdio.h>
#include <string.h>

static pci_device_t pci_devices[PCI_MAX_DEVICES];
static int pci_device_count = 0;

/* ------------------------------------------------------------------ */
/*  PCI config space access                                           */
/* ------------------------------------------------------------------ */

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t addr = (1u << 31)               /* enable bit */
                  | ((uint32_t)bus << 16)
                  | ((uint32_t)slot << 11)
                  | ((uint32_t)func << 8)
                  | (offset & 0xFC);         /* dword-aligned */
    hal_outl(PCI_CONFIG_ADDR, addr);
    return hal_inl(PCI_CONFIG_DATA);
}

uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t dword = pci_config_read32(bus, slot, func, offset & 0xFC);
    return (uint16_t)(dword >> ((offset & 2) * 8));
}

uint8_t pci_config_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t dword = pci_config_read32(bus, slot, func, offset & 0xFC);
    return (uint8_t)(dword >> ((offset & 3) * 8));
}

void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val) {
    uint32_t addr = (1u << 31)
                  | ((uint32_t)bus << 16)
                  | ((uint32_t)slot << 11)
                  | ((uint32_t)func << 8)
                  | (offset & 0xFC);
    hal_outl(PCI_CONFIG_ADDR, addr);
    hal_outl(PCI_CONFIG_DATA, val);
}

void pci_config_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t val) {
    uint32_t dword = pci_config_read32(bus, slot, func, offset & 0xFC);
    int shift = (offset & 2) * 8;
    dword &= ~(0xFFFFu << shift);
    dword |= ((uint32_t)val << shift);
    pci_config_write32(bus, slot, func, offset & 0xFC, dword);
}

/* ------------------------------------------------------------------ */
/*  Capability list parsing                                           */
/* ------------------------------------------------------------------ */

static void pci_parse_caps(pci_device_t *dev) {
    dev->cap_count = 0;

    /* Check status register bit 4: capabilities list present */
    uint16_t status = pci_config_read16(dev->bus, dev->slot, dev->func, PCI_STATUS);
    if (!(status & PCI_STATUS_CAP_LIST)) return;

    /* Walk the capability linked list starting at PCI_CAP_PTR */
    uint8_t offset = pci_config_read8(dev->bus, dev->slot, dev->func, PCI_CAP_PTR);
    offset &= 0xFC;  /* must be dword-aligned */

    int limit = 48;   /* guard against infinite loops from broken hardware */
    while (offset >= 0x40 && limit-- > 0) {
        uint8_t id   = pci_config_read8(dev->bus, dev->slot, dev->func, offset);
        uint8_t next = pci_config_read8(dev->bus, dev->slot, dev->func, offset + 1);

        if (dev->cap_count < PCI_MAX_CAPS) {
            dev->caps[dev->cap_count].id     = id;
            dev->caps[dev->cap_count].offset = offset;
            dev->cap_count++;
        }

        offset = next & 0xFC;
    }
}

/* ------------------------------------------------------------------ */
/*  BAR sizing and 64-bit detection                                   */
/* ------------------------------------------------------------------ */

static void pci_size_bars(pci_device_t *dev) {
    dev->bar_is_64 = 0;
    memset(dev->bar_size, 0, sizeof(dev->bar_size));

    uint32_t flags = hal_irq_save();

    for (int b = 0; b < 6; b++) {
        uint32_t bar_val = dev->bar[b];

        /* Skip unimplemented BARs */
        if (bar_val == 0) continue;

        /* Skip if this is the upper half of a 64-bit BAR */
        if (b > 0 && (dev->bar_is_64 & (1 << (b - 1)))) continue;

        /* I/O BARs: we don't size them (not needed for GPU work) */
        if (bar_val & PCI_BAR_IO) continue;

        uint8_t reg = PCI_BAR0 + b * 4;

        /* Write all 1s to BAR, read back to determine size */
        pci_config_write32(dev->bus, dev->slot, dev->func, reg, 0xFFFFFFFF);
        uint32_t mask = pci_config_read32(dev->bus, dev->slot, dev->func, reg);

        /* Restore original BAR value */
        pci_config_write32(dev->bus, dev->slot, dev->func, reg, bar_val);

        /* Memory BAR: mask off type bits (low 4 bits) */
        mask &= ~0xFu;
        if (mask == 0) continue;

        /* Size = ~mask + 1 (invert and add 1) */
        dev->bar_size[b] = ~mask + 1;

        /* Check for 64-bit BAR */
        if ((bar_val & PCI_BAR_MEM_TYPE_MASK) == PCI_BAR_MEM_64BIT && b < 5) {
            dev->bar_is_64 |= (1 << b);
            /* Size the upper BAR too (write all 1s, read back, restore) */
            uint8_t reg_hi = PCI_BAR0 + (b + 1) * 4;
            pci_config_write32(dev->bus, dev->slot, dev->func, reg_hi, 0xFFFFFFFF);
            /* Read — for 32-bit addressable devices this will be 0 */
            pci_config_read32(dev->bus, dev->slot, dev->func, reg_hi);
            /* Restore upper BAR */
            pci_config_write32(dev->bus, dev->slot, dev->func, reg_hi, dev->bar[b + 1]);
        }
    }

    hal_irq_restore(flags);
}

/* ------------------------------------------------------------------ */
/*  Bus scanning                                                      */
/* ------------------------------------------------------------------ */

static void pci_scan_bus(uint8_t bus) {
    for (uint8_t slot = 0; slot < 32; slot++) {
        for (uint8_t func = 0; func < 8; func++) {
            uint32_t id = pci_config_read32(bus, slot, func, 0);
            uint16_t vendor = id & 0xFFFF;

            if (vendor == 0xFFFF) {
                if (func == 0) break;  /* no device at this slot */
                continue;
            }

            if (pci_device_count >= PCI_MAX_DEVICES) return;

            pci_device_t *dev = &pci_devices[pci_device_count++];
            dev->bus = bus;
            dev->slot = slot;
            dev->func = func;
            dev->vendor_id = vendor;
            dev->device_id = (id >> 16) & 0xFFFF;

            uint32_t class_reg = pci_config_read32(bus, slot, func, 0x08);
            dev->class_code = (class_reg >> 24) & 0xFF;
            dev->subclass   = (class_reg >> 16) & 0xFF;
            dev->irq_line   = pci_config_read8(bus, slot, func, PCI_IRQ_LINE);

            for (int b = 0; b < 6; b++)
                dev->bar[b] = pci_config_read32(bus, slot, func, PCI_BAR0 + b * 4);

            /* Parse capability list and size BARs */
            pci_parse_caps(dev);
            pci_size_bars(dev);

            /* If not multifunction device, skip remaining functions */
            if (func == 0) {
                uint8_t header = pci_config_read8(bus, slot, func, PCI_HEADER_TYPE);
                if (!(header & 0x80)) break;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

void pci_init(void) {
    pci_device_count = 0;
    pci_scan_bus(0);
    /* Bus 0 is sufficient for QEMU; real hardware may need multi-bus scan */
}

pci_device_t *pci_find_device(uint16_t vendor_id, uint16_t device_id) {
    for (int i = 0; i < pci_device_count; i++) {
        if (pci_devices[i].vendor_id == vendor_id &&
            pci_devices[i].device_id == device_id)
            return &pci_devices[i];
    }
    return NULL;
}

pci_device_t *pci_get_devices(int *count_out) {
    if (count_out) *count_out = pci_device_count;
    return pci_devices;
}

void pci_enable_bus_master(pci_device_t *dev) {
    uint16_t cmd = pci_config_read16(dev->bus, dev->slot, dev->func, PCI_COMMAND);
    cmd |= PCI_CMD_BUS_MASTER | PCI_CMD_MEM_SPACE;
    cmd &= ~PCI_CMD_INT_DISABLE;  /* ensure PCI interrupts are not suppressed */
    pci_config_write16(dev->bus, dev->slot, dev->func, PCI_COMMAND, cmd);
}

uint8_t pci_find_cap(pci_device_t *dev, uint8_t cap_id) {
    for (int i = 0; i < dev->cap_count; i++) {
        if (dev->caps[i].id == cap_id)
            return dev->caps[i].offset;
    }
    return 0;
}

uint32_t pci_bar_addr(pci_device_t *dev, int bar_index) {
    if (bar_index < 0 || bar_index > 5) return 0;
    uint32_t bar_val = dev->bar[bar_index];
    if (bar_val == 0) return 0;
    if (bar_val & PCI_BAR_IO) return 0;  /* I/O BAR, not memory */
    return bar_val & ~0xFu;  /* mask off type/prefetch bits */
}
