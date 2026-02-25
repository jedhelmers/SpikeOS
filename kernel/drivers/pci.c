/*
 * PCI bus enumeration for SpikeOS.
 *
 * Scans bus 0 (sufficient for QEMU and most single-bus systems),
 * stores discovered devices, provides config space read/write.
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
