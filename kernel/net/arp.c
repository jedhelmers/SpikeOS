/*
 * ARP (Address Resolution Protocol) for SpikeOS.
 *
 * Maintains a 16-entry cache, sends requests, replies to queries
 * for our IP, and provides blocking resolution with timeout.
 */

#include <kernel/net.h>
#include <kernel/e1000.h>
#include <kernel/hal.h>
#include <kernel/timer.h>
#include <kernel/wait.h>
#include <stdio.h>
#include <string.h>

static arp_entry_t arp_cache[ARP_CACHE_SIZE];
static wait_queue_t arp_wq = WAIT_QUEUE_INIT;

static const uint8_t broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static const uint8_t zero_mac[6]      = {0, 0, 0, 0, 0, 0};

/* ------------------------------------------------------------------ */
/*  Cache management                                                  */
/* ------------------------------------------------------------------ */

void arp_init(void) {
    memset(arp_cache, 0, sizeof(arp_cache));
}

static void arp_cache_add(uint32_t ip, const uint8_t *mac) {
    uint32_t flags = hal_irq_save();

    /* Update existing entry */
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            memcpy(arp_cache[i].mac, mac, 6);
            arp_cache[i].timestamp = timer_ticks();
            hal_irq_restore(flags);
            wake_up_all(&arp_wq);
            return;
        }
    }

    /* Use first free slot */
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!arp_cache[i].valid) {
            arp_cache[i].ip = ip;
            memcpy(arp_cache[i].mac, mac, 6);
            arp_cache[i].timestamp = timer_ticks();
            arp_cache[i].valid = 1;
            hal_irq_restore(flags);
            wake_up_all(&arp_wq);
            return;
        }
    }

    /* Cache full — overwrite oldest entry */
    int oldest = 0;
    uint32_t oldest_time = arp_cache[0].timestamp;
    for (int i = 1; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].timestamp < oldest_time) {
            oldest_time = arp_cache[i].timestamp;
            oldest = i;
        }
    }
    arp_cache[oldest].ip = ip;
    memcpy(arp_cache[oldest].mac, mac, 6);
    arp_cache[oldest].timestamp = timer_ticks();
    arp_cache[oldest].valid = 1;

    hal_irq_restore(flags);
    wake_up_all(&arp_wq);
}

static int arp_cache_lookup(uint32_t ip, uint8_t *mac_out) {
    uint32_t flags = hal_irq_save();
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            memcpy(mac_out, arp_cache[i].mac, 6);
            hal_irq_restore(flags);
            return 0;
        }
    }
    hal_irq_restore(flags);
    return -1;
}

/* ------------------------------------------------------------------ */
/*  ARP request                                                       */
/* ------------------------------------------------------------------ */

void arp_request(uint32_t target_ip) {
    if (!nic) return;

    arp_header_t arp;
    arp.htype = htons(ARP_HW_ETHER);
    arp.ptype = htons(ETH_TYPE_IP);
    arp.hlen  = 6;
    arp.plen  = 4;
    arp.oper  = htons(ARP_OP_REQUEST);

    memcpy(arp.sha, nic->mac, 6);
    arp.spa = net_cfg.ip;
    memcpy(arp.tha, zero_mac, 6);
    arp.tpa = target_ip;

    eth_send(broadcast_mac, ETH_TYPE_ARP, &arp, sizeof(arp));
}

/* ------------------------------------------------------------------ */
/*  ARP RX handler                                                    */
/* ------------------------------------------------------------------ */

void arp_handle(const void *data, uint16_t len) {
    if (len < sizeof(arp_header_t)) return;

    const arp_header_t *arp = (const arp_header_t *)data;

    /* Only handle Ethernet + IPv4 */
    if (ntohs(arp->htype) != ARP_HW_ETHER) return;
    if (ntohs(arp->ptype) != ETH_TYPE_IP)   return;

    /* Always cache the sender's IP→MAC mapping */
    arp_cache_add(arp->spa, arp->sha);

    /* If this is a request for our IP, send a reply */
    if (ntohs(arp->oper) == ARP_OP_REQUEST &&
        net_cfg.configured && arp->tpa == net_cfg.ip) {

        arp_header_t reply;
        reply.htype = htons(ARP_HW_ETHER);
        reply.ptype = htons(ETH_TYPE_IP);
        reply.hlen  = 6;
        reply.plen  = 4;
        reply.oper  = htons(ARP_OP_REPLY);

        memcpy(reply.sha, nic->mac, 6);
        reply.spa = net_cfg.ip;
        memcpy(reply.tha, arp->sha, 6);
        reply.tpa = arp->spa;

        eth_send(arp->sha, ETH_TYPE_ARP, &reply, sizeof(reply));
    }
}

/* ------------------------------------------------------------------ */
/*  Blocking ARP resolve                                              */
/* ------------------------------------------------------------------ */

int arp_resolve(uint32_t ip, uint8_t *mac_out) {
    /* Check cache first */
    if (arp_cache_lookup(ip, mac_out) == 0)
        return 0;

    /* Send ARP request and busy-wait up to 3 seconds */
    arp_request(ip);

    uint32_t deadline = timer_ticks() + 300;  /* 3 seconds at 100Hz */
    while (timer_ticks() < deadline) {
        if (arp_cache_lookup(ip, mac_out) == 0)
            return 0;

        /* Wait a bit then retry */
        hal_irq_enable();
        hal_halt();
    }

    return -1;  /* timeout */
}
