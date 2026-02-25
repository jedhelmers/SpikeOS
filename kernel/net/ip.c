/*
 * IPv4 send/receive for SpikeOS.
 */

#include <kernel/net.h>
#include <kernel/e1000.h>
#include <kernel/hal.h>
#include <stdio.h>
#include <string.h>

static uint16_t ip_id_counter = 1;

/* ------------------------------------------------------------------ */
/*  IP checksum (RFC 1071)                                            */
/* ------------------------------------------------------------------ */

uint16_t ip_checksum(const void *data, uint16_t len) {
    const uint16_t *words = (const uint16_t *)data;
    uint32_t sum = 0;

    while (len > 1) {
        sum += *words++;
        len -= 2;
    }
    if (len == 1) {
        sum += *(const uint8_t *)words;
    }

    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);

    return (uint16_t)(~sum);
}

/* ------------------------------------------------------------------ */
/*  IPv4 receive                                                      */
/* ------------------------------------------------------------------ */

void ip_handle(const void *data, uint16_t len) {
    if (len < sizeof(ip_header_t)) return;

    const ip_header_t *ip = (const ip_header_t *)data;

    /* Validate: must be IPv4 */
    if ((ip->ver_ihl >> 4) != 4) return;

    /* Validate checksum */
    uint16_t ihl = (ip->ver_ihl & 0x0F) * 4;
    if (ihl < 20 || ihl > len) return;
    if (ip_checksum(ip, ihl) != 0) return;

    /* Accept packets for our IP, or broadcast (for DHCP before config) */
    if (net_cfg.configured && ip->dst_ip != net_cfg.ip &&
        ip->dst_ip != 0xFFFFFFFFu) return;

    uint16_t payload_len = ntohs(ip->total_len) - ihl;
    const void *payload = (const uint8_t *)data + ihl;
    uint32_t src_ip = ip->src_ip;

    switch (ip->protocol) {
    case IP_PROTO_ICMP:
        icmp_handle(payload, payload_len, src_ip);
        break;
    case IP_PROTO_UDP:
        udp_handle(payload, payload_len, src_ip);
        break;
    }
}

/* ------------------------------------------------------------------ */
/*  IPv4 send                                                         */
/* ------------------------------------------------------------------ */

int ip_send(uint32_t dst_ip, uint8_t protocol,
            const void *payload, uint16_t payload_len) {
    if (!nic || !net_cfg.configured) return -1;

    uint16_t total_len = 20 + payload_len;
    uint8_t packet[ETH_MTU];
    if (total_len > ETH_MTU) return -1;

    ip_header_t *ip = (ip_header_t *)packet;
    ip->ver_ihl    = 0x45;  /* IPv4, IHL=5 (20 bytes) */
    ip->tos        = 0;
    ip->total_len  = htons(total_len);
    ip->id         = htons(ip_id_counter++);
    ip->flags_frag = 0;
    ip->ttl        = 64;
    ip->protocol   = protocol;
    ip->checksum   = 0;
    ip->src_ip     = net_cfg.ip;
    ip->dst_ip     = dst_ip;

    ip->checksum = ip_checksum(ip, 20);

    memcpy(packet + 20, payload, payload_len);

    /* Determine next-hop: same subnet → direct, else gateway */
    uint32_t next_hop = dst_ip;
    if (net_cfg.subnet != 0 &&
        (dst_ip & net_cfg.subnet) != (net_cfg.ip & net_cfg.subnet)) {
        next_hop = net_cfg.gateway;
    }
    /* Broadcast is always direct */
    if (dst_ip == 0xFFFFFFFFu) {
        static const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        return eth_send(bcast, ETH_TYPE_IP, packet, total_len);
    }

    /* ARP resolve the next-hop */
    uint8_t dst_mac[6];
    if (arp_resolve(next_hop, dst_mac) != 0) {
        return -1;  /* ARP timeout */
    }

    return eth_send(dst_mac, ETH_TYPE_IP, packet, total_len);
}
