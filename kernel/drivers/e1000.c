/*
 * Intel e1000 NIC driver for SpikeOS.
 *
 * Supports the 82540EM (0x100E) used by QEMU's `-device e1000`, plus
 * several common real-hardware variants (0x100F, 0x1004, 0x10D3).
 *
 * MMIO registers are mapped at 0xC0C00000 (PDE[771]), following the
 * same pattern as the framebuffer (PDE[770]).
 *
 * TX/RX use legacy descriptors with DMA.  RX is IRQ-driven;
 * TX is synchronous (waits for DD).
 */

#include <kernel/e1000.h>
#include <kernel/pci.h>
#include <kernel/paging.h>
#include <kernel/heap.h>
#include <kernel/hal.h>
#include <kernel/isr.h>
#include <kernel/pic.h>
#include <kernel/timer.h>
#include <stdio.h>
#include <string.h>

/* Forward declaration — called by net layer when a packet arrives */
extern void net_rx_callback(const void *data, uint16_t len);

/* Weak stub so the driver compiles before the net layer exists.
   net.c will provide the real implementation. */
__attribute__((weak))
void net_rx_callback(const void *data, uint16_t len) {
    (void)data; (void)len;
}

/* ------------------------------------------------------------------ */
/*  MMIO mapping                                                      */
/* ------------------------------------------------------------------ */

#define E1000_MMIO_BASE  0xC0C00000u   /* PDE[771] */
#define E1000_MMIO_PAGES 32            /* 128 KB */

static volatile uint32_t *mmio_base;

static inline uint32_t e1000_read(uint32_t reg) {
    return mmio_base[reg / 4];
}

static inline void e1000_write(uint32_t reg, uint32_t val) {
    mmio_base[reg / 4] = val;
}

/* ------------------------------------------------------------------ */
/*  Descriptor rings                                                  */
/* ------------------------------------------------------------------ */

static e1000_tx_desc_t *tx_descs;
static e1000_rx_desc_t *rx_descs;

/* Static TX buffers — avoids kmalloc in the send path */
static uint8_t tx_buffers[E1000_NUM_TX_DESC][E1000_RX_BUF_SIZE]
    __attribute__((aligned(16)));

/* RX buffers — one 2KB buffer per descriptor */
static uint8_t *rx_buffers[E1000_NUM_RX_DESC];

static uint16_t tx_tail;
static uint16_t rx_tail;

/* ------------------------------------------------------------------ */
/*  NIC abstraction                                                   */
/* ------------------------------------------------------------------ */

static nic_t e1000_nic;
nic_t *nic = (nic_t *)0;

/* ------------------------------------------------------------------ */
/*  EEPROM read                                                       */
/* ------------------------------------------------------------------ */

static int eeprom_read(uint8_t addr, uint16_t *data_out) {
    e1000_write(E1000_EERD,
                ((uint32_t)addr << E1000_EERD_ADDR_SHIFT) | E1000_EERD_START);

    /* Poll for completion (timeout ~10ms) */
    for (int i = 0; i < 10000; i++) {
        uint32_t val = e1000_read(E1000_EERD);
        if (val & E1000_EERD_DONE) {
            *data_out = (uint16_t)(val >> E1000_EERD_DATA_SHIFT);
            return 0;
        }
    }
    return -1;  /* timeout */
}

/* ------------------------------------------------------------------ */
/*  MAC address                                                       */
/* ------------------------------------------------------------------ */

static uint8_t mac_addr[6];

static void read_mac_address(void) {
    /* Try EEPROM first */
    uint16_t w0, w1, w2;
    if (eeprom_read(0, &w0) == 0 &&
        eeprom_read(1, &w1) == 0 &&
        eeprom_read(2, &w2) == 0) {
        mac_addr[0] = w0 & 0xFF;
        mac_addr[1] = (w0 >> 8) & 0xFF;
        mac_addr[2] = w1 & 0xFF;
        mac_addr[3] = (w1 >> 8) & 0xFF;
        mac_addr[4] = w2 & 0xFF;
        mac_addr[5] = (w2 >> 8) & 0xFF;
        return;
    }

    /* Fall back to RAL/RAH registers */
    uint32_t ral = e1000_read(E1000_RAL);
    uint32_t rah = e1000_read(E1000_RAH);
    mac_addr[0] = (ral >>  0) & 0xFF;
    mac_addr[1] = (ral >>  8) & 0xFF;
    mac_addr[2] = (ral >> 16) & 0xFF;
    mac_addr[3] = (ral >> 24) & 0xFF;
    mac_addr[4] = (rah >>  0) & 0xFF;
    mac_addr[5] = (rah >>  8) & 0xFF;
}

/* ------------------------------------------------------------------ */
/*  TX ring initialization                                            */
/* ------------------------------------------------------------------ */

static void tx_init(void) {
    tx_descs = (e1000_tx_desc_t *)kcalloc(E1000_NUM_TX_DESC,
                                           sizeof(e1000_tx_desc_t));

    /* Point each descriptor at its static buffer and mark DD
       so the first send() sees them as "done" */
    for (int i = 0; i < E1000_NUM_TX_DESC; i++) {
        tx_descs[i].addr   = (uint64_t)virt_to_phys((uint32_t)tx_buffers[i]);
        tx_descs[i].status = E1000_TXD_STAT_DD;
    }

    uint32_t phys = virt_to_phys((uint32_t)tx_descs);
    e1000_write(E1000_TDBAL, phys);
    e1000_write(E1000_TDBAH, 0);
    e1000_write(E1000_TDLEN, E1000_NUM_TX_DESC * sizeof(e1000_tx_desc_t));
    e1000_write(E1000_TDH, 0);
    e1000_write(E1000_TDT, 0);
    tx_tail = 0;

    /* Enable transmitter: pad short packets, collision threshold, distance */
    e1000_write(E1000_TCTL,
                E1000_TCTL_EN | E1000_TCTL_PSP |
                (15u << E1000_TCTL_CT_SHIFT) |
                (64u << E1000_TCTL_COLD_SHIFT));

    /* Inter-packet gap: 10 | (8 << 10) | (6 << 20) — IEEE 802.3 */
    e1000_write(E1000_TIPG, 10u | (8u << 10) | (6u << 20));
}

/* ------------------------------------------------------------------ */
/*  RX ring initialization                                            */
/* ------------------------------------------------------------------ */

static void rx_init(void) {
    rx_descs = (e1000_rx_desc_t *)kcalloc(E1000_NUM_RX_DESC,
                                           sizeof(e1000_rx_desc_t));

    for (int i = 0; i < E1000_NUM_RX_DESC; i++) {
        rx_buffers[i] = (uint8_t *)kmalloc(E1000_RX_BUF_SIZE);
        rx_descs[i].addr = (uint64_t)virt_to_phys((uint32_t)rx_buffers[i]);
        rx_descs[i].status = 0;
    }

    uint32_t phys = virt_to_phys((uint32_t)rx_descs);
    e1000_write(E1000_RDBAL, phys);
    e1000_write(E1000_RDBAH, 0);
    e1000_write(E1000_RDLEN, E1000_NUM_RX_DESC * sizeof(e1000_rx_desc_t));
    e1000_write(E1000_RDH, 0);
    e1000_write(E1000_RDT, E1000_NUM_RX_DESC - 1);
    rx_tail = E1000_NUM_RX_DESC - 1;

    /* Enable receiver: accept broadcast, strip CRC, 2KB buffers */
    e1000_write(E1000_RCTL,
                E1000_RCTL_EN | E1000_RCTL_BAM |
                E1000_RCTL_BSIZE_2K | E1000_RCTL_SECRC);
}

/* ------------------------------------------------------------------ */
/*  IRQ handler                                                       */
/* ------------------------------------------------------------------ */

static void e1000_irq_handler(trapframe *tf) {
    (void)tf;

    uint32_t icr = e1000_read(E1000_ICR);  /* reading clears bits */

    /* Link status change */
    if (icr & E1000_ICR_LSC) {
        uint32_t status = e1000_read(E1000_STATUS);
        e1000_nic.link_up = (status & 0x2) ? 1 : 0;
    }

    /* Receive: process all descriptors with DD set */
    if (icr & (E1000_ICR_RXT0 | E1000_ICR_RXDMT0 | E1000_ICR_RXO)) {
        while (1) {
            uint16_t next = (rx_tail + 1) % E1000_NUM_RX_DESC;
            if (!(rx_descs[next].status & E1000_RXD_STAT_DD))
                break;

            uint16_t len = rx_descs[next].length;
            if ((rx_descs[next].status & E1000_RXD_STAT_EOP) && len > 0) {
                net_rx_callback(rx_buffers[next], len);
            }

            /* Reset descriptor and advance tail */
            rx_descs[next].status = 0;
            rx_tail = next;
            e1000_write(E1000_RDT, rx_tail);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Public API: send                                                  */
/* ------------------------------------------------------------------ */

int e1000_send(const void *data, uint16_t len) {
    if (len == 0 || len > E1000_RX_BUF_SIZE) return -1;

    uint32_t flags = hal_irq_save();

    /* Wait for previous descriptor at this slot to finish */
    if (!(tx_descs[tx_tail].status & E1000_TXD_STAT_DD)) {
        hal_irq_restore(flags);
        return -1;  /* ring full */
    }

    /* Copy data into the static TX buffer */
    memcpy(tx_buffers[tx_tail], data, len);

    /* Set up the descriptor */
    tx_descs[tx_tail].addr   = (uint64_t)virt_to_phys((uint32_t)tx_buffers[tx_tail]);
    tx_descs[tx_tail].length = len;
    tx_descs[tx_tail].cmd    = E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS |
                               E1000_TXD_CMD_RS;
    tx_descs[tx_tail].status = 0;

    /* Advance tail — tells hardware there's a new packet */
    tx_tail = (tx_tail + 1) % E1000_NUM_TX_DESC;
    e1000_write(E1000_TDT, tx_tail);

    hal_irq_restore(flags);
    return 0;
}

void e1000_get_mac(uint8_t *out) {
    memcpy(out, mac_addr, 6);
}

int e1000_link_up(void) {
    return e1000_nic.link_up;
}

/* ------------------------------------------------------------------ */
/*  Initialization                                                    */
/* ------------------------------------------------------------------ */

int e1000_init(void) {
    /* Try several known e1000 device IDs */
    static const uint16_t device_ids[] = {
        0x100E,  /* 82540EM — QEMU default */
        0x100F,  /* 82545EM */
        0x1004,  /* 82543GC */
        0x10D3,  /* 82574L — common on real laptops */
    };

    pci_device_t *dev = (pci_device_t *)0;
    for (int i = 0; i < 4; i++) {
        dev = pci_find_device(0x8086, device_ids[i]);
        if (dev) break;
    }

    if (!dev) {
#ifdef VERBOSE_BOOT
        printf("[e1000] no supported NIC found\n");
#endif
        return -1;
    }

#ifdef VERBOSE_BOOT
    printf("[e1000] found %04x:%04x at %02x:%02x.%x IRQ=%d\n",
           dev->vendor_id, dev->device_id,
           dev->bus, dev->slot, dev->func, dev->irq_line);
#endif

    /* Enable PCI bus mastering (needed for DMA) */
    pci_enable_bus_master(dev);

    /* Map MMIO region: BAR0 holds the physical base address */
    uint32_t bar0 = dev->bar[0] & ~0xFu;  /* mask type bits */

    for (int i = 0; i < E1000_MMIO_PAGES; i++) {
        uint32_t virt = E1000_MMIO_BASE + i * PAGE_SIZE;
        uint32_t phys = bar0 + i * PAGE_SIZE;
        if (map_page(virt, phys,
                     PAGE_PRESENT | PAGE_WRITABLE | PAGE_CACHE_DISABLE) != 0) {
            printf("[e1000] map_page failed at 0x%x\n", virt);
            return -1;
        }
    }
    mmio_base = (volatile uint32_t *)E1000_MMIO_BASE;

    /* Reset the device */
    e1000_write(E1000_CTRL, e1000_read(E1000_CTRL) | E1000_CTRL_RST);
    /* Wait for reset to complete (~1ms) */
    for (volatile int i = 0; i < 100000; i++);

    /* Set Link Up */
    e1000_write(E1000_CTRL, e1000_read(E1000_CTRL) | E1000_CTRL_SLU);

    /* Clear the multicast table array (128 dwords) */
    for (int i = 0; i < 128; i++)
        e1000_write(E1000_MTA + i * 4, 0);

    /* Read MAC address */
    read_mac_address();

    /* Initialize TX and RX descriptor rings */
    tx_init();
    rx_init();

    /* Clear any pending interrupts */
    e1000_read(E1000_ICR);

    /* Enable interrupts we care about */
    e1000_write(E1000_IMS,
                E1000_ICR_RXT0 | E1000_ICR_LSC |
                E1000_ICR_RXDMT0 | E1000_ICR_RXO);

    /* Install IRQ handler — the e1000 in QEMU typically uses IRQ 11,
       but we read it from PCI config to be safe */
    irq_install_handler(dev->irq_line, e1000_irq_handler);
    pic_clear_mask(dev->irq_line);

    /* Check link status */
    uint32_t status = e1000_read(E1000_STATUS);
    e1000_nic.link_up = (status & 0x2) ? 1 : 0;

    /* Populate NIC abstraction */
    memcpy(e1000_nic.mac, mac_addr, 6);
    e1000_nic.send = e1000_send;
    nic = &e1000_nic;

#ifdef VERBOSE_BOOT
    printf("[e1000] MAC=%02x:%02x:%02x:%02x:%02x:%02x link=%s\n",
           mac_addr[0], mac_addr[1], mac_addr[2],
           mac_addr[3], mac_addr[4], mac_addr[5],
           e1000_nic.link_up ? "UP" : "DOWN");
#endif

    return 0;
}
