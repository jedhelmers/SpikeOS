#ifndef _E1000_H
#define _E1000_H

#include <stdint.h>

/* ------------------------------------------------------------------ */
/*  Intel e1000 register offsets (from datasheet)                      */
/* ------------------------------------------------------------------ */

#define E1000_CTRL       0x0000   /* Device Control */
#define E1000_STATUS     0x0008   /* Device Status */
#define E1000_EERD       0x0014   /* EEPROM Read */
#define E1000_ICR        0x00C0   /* Interrupt Cause Read */
#define E1000_IMS        0x00D0   /* Interrupt Mask Set */
#define E1000_IMC        0x00D8   /* Interrupt Mask Clear */
#define E1000_RCTL       0x0100   /* Receive Control */
#define E1000_TCTL       0x0400   /* Transmit Control */
#define E1000_TIPG       0x0410   /* Transmit Inter-Packet Gap */

/* Receive descriptor ring */
#define E1000_RDBAL      0x2800   /* RX Descriptor Base Low */
#define E1000_RDBAH      0x2804   /* RX Descriptor Base High */
#define E1000_RDLEN      0x2808   /* RX Descriptor Length */
#define E1000_RDH        0x2810   /* RX Descriptor Head */
#define E1000_RDT        0x2818   /* RX Descriptor Tail */

/* Transmit descriptor ring */
#define E1000_TDBAL      0x3800   /* TX Descriptor Base Low */
#define E1000_TDBAH      0x3804   /* TX Descriptor Base High */
#define E1000_TDLEN      0x3808   /* TX Descriptor Length */
#define E1000_TDH        0x3810   /* TX Descriptor Head */
#define E1000_TDT        0x3818   /* TX Descriptor Tail */

/* Receive address (MAC) */
#define E1000_RAL        0x5400   /* Receive Address Low */
#define E1000_RAH        0x5404   /* Receive Address High */

/* Multicast table array (128 dwords) */
#define E1000_MTA        0x5200

/* ------------------------------------------------------------------ */
/*  CTRL register bits                                                 */
/* ------------------------------------------------------------------ */

#define E1000_CTRL_SLU   (1u << 6)    /* Set Link Up */
#define E1000_CTRL_RST   (1u << 26)   /* Device Reset */

/* ------------------------------------------------------------------ */
/*  RCTL register bits                                                 */
/* ------------------------------------------------------------------ */

#define E1000_RCTL_EN        (1u << 1)    /* Receiver Enable */
#define E1000_RCTL_SBP       (1u << 2)    /* Store Bad Packets */
#define E1000_RCTL_UPE       (1u << 3)    /* Unicast Promiscuous */
#define E1000_RCTL_MPE       (1u << 4)    /* Multicast Promiscuous */
#define E1000_RCTL_BAM       (1u << 15)   /* Broadcast Accept Mode */
#define E1000_RCTL_BSIZE_2K  (0u << 16)   /* Buffer Size 2048 (default) */
#define E1000_RCTL_SECRC     (1u << 26)   /* Strip Ethernet CRC */

/* ------------------------------------------------------------------ */
/*  TCTL register bits                                                 */
/* ------------------------------------------------------------------ */

#define E1000_TCTL_EN        (1u << 1)    /* Transmit Enable */
#define E1000_TCTL_PSP       (1u << 3)    /* Pad Short Packets */
#define E1000_TCTL_CT_SHIFT  4            /* Collision Threshold */
#define E1000_TCTL_COLD_SHIFT 12          /* Collision Distance */

/* ------------------------------------------------------------------ */
/*  ICR / IMS / IMC interrupt bits                                     */
/* ------------------------------------------------------------------ */

#define E1000_ICR_TXDW    (1u << 0)    /* TX Descriptor Written Back */
#define E1000_ICR_TXQE    (1u << 1)    /* TX Queue Empty */
#define E1000_ICR_LSC     (1u << 2)    /* Link Status Change */
#define E1000_ICR_RXDMT0  (1u << 4)    /* RX Descriptor Minimum Threshold */
#define E1000_ICR_RXO     (1u << 6)    /* Receiver Overrun */
#define E1000_ICR_RXT0    (1u << 7)    /* Receiver Timer Interrupt */

/* ------------------------------------------------------------------ */
/*  EEPROM read bits                                                   */
/* ------------------------------------------------------------------ */

#define E1000_EERD_START  (1u << 0)    /* Start Read */
#define E1000_EERD_DONE   (1u << 4)    /* Read Done */
#define E1000_EERD_ADDR_SHIFT 8        /* Address shift */
#define E1000_EERD_DATA_SHIFT 16       /* Data shift */

/* ------------------------------------------------------------------ */
/*  TX command bits (in descriptor cmd field)                          */
/* ------------------------------------------------------------------ */

#define E1000_TXD_CMD_EOP   (1u << 0)   /* End of Packet */
#define E1000_TXD_CMD_IFCS  (1u << 1)   /* Insert FCS/CRC */
#define E1000_TXD_CMD_RS    (1u << 3)   /* Report Status */

/* TX status bits */
#define E1000_TXD_STAT_DD   (1u << 0)   /* Descriptor Done */

/* RX status bits */
#define E1000_RXD_STAT_DD   (1u << 0)   /* Descriptor Done */
#define E1000_RXD_STAT_EOP  (1u << 1)   /* End of Packet */

/* ------------------------------------------------------------------ */
/*  Descriptor counts and buffer size                                  */
/* ------------------------------------------------------------------ */

#define E1000_NUM_TX_DESC  16
#define E1000_NUM_RX_DESC  32
#define E1000_RX_BUF_SIZE  2048

/* ------------------------------------------------------------------ */
/*  Legacy TX descriptor (16 bytes, packed)                            */
/* ------------------------------------------------------------------ */

typedef struct __attribute__((packed)) {
    uint64_t addr;        /* Buffer address (physical) */
    uint16_t length;      /* Data length */
    uint8_t  cso;         /* Checksum offset */
    uint8_t  cmd;         /* Command */
    uint8_t  status;      /* Status (DD bit) */
    uint8_t  css;         /* Checksum start */
    uint16_t special;
} e1000_tx_desc_t;

/* ------------------------------------------------------------------ */
/*  Legacy RX descriptor (16 bytes, packed)                            */
/* ------------------------------------------------------------------ */

typedef struct __attribute__((packed)) {
    uint64_t addr;        /* Buffer address (physical) */
    uint16_t length;      /* Received length */
    uint16_t checksum;    /* Packet checksum */
    uint8_t  status;      /* Status (DD, EOP) */
    uint8_t  errors;      /* Errors */
    uint16_t special;
} e1000_rx_desc_t;

/* ------------------------------------------------------------------ */
/*  NIC abstraction — allows swapping e1000 for another driver later   */
/* ------------------------------------------------------------------ */

typedef struct nic {
    uint8_t  mac[6];
    int      link_up;
    int      (*send)(const void *data, uint16_t len);
} nic_t;

extern nic_t *nic;  /* Global NIC pointer (NULL if no NIC) */

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

/* Initialize the e1000 NIC. Returns 0 on success, -1 if not found. */
int e1000_init(void);

/* Send a raw Ethernet frame. Returns 0 on success, -1 on error. */
int e1000_send(const void *data, uint16_t len);

/* Get MAC address (copies 6 bytes to out) */
void e1000_get_mac(uint8_t *out);

/* Check if link is up */
int e1000_link_up(void);

#endif
