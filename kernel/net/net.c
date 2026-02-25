/*
 * Ethernet frame handling and network layer glue.
 */

#include <kernel/net.h>
#include <kernel/e1000.h>
#include <kernel/hal.h>
#include <stdio.h>
#include <string.h>

net_config_t net_cfg;

/* ------------------------------------------------------------------ */
/*  Initialization                                                    */
/* ------------------------------------------------------------------ */

void net_init(void) {
    memset(&net_cfg, 0, sizeof(net_cfg));
    arp_init();
    udp_init();
}

/* ------------------------------------------------------------------ */
/*  Ethernet TX                                                       */
/* ------------------------------------------------------------------ */

int eth_send(const uint8_t *dst_mac, uint16_t type,
             const void *payload, uint16_t payload_len) {
    if (!nic || payload_len > ETH_MTU) return -1;

    uint8_t frame[ETH_FRAME_MAX];
    eth_header_t *eth = (eth_header_t *)frame;

    memcpy(eth->dst, dst_mac, ETH_ADDR_LEN);
    memcpy(eth->src, nic->mac, ETH_ADDR_LEN);
    eth->type = htons(type);

    memcpy(frame + ETH_HDR_LEN, payload, payload_len);

    uint16_t total = ETH_HDR_LEN + payload_len;
    /* Pad to minimum Ethernet frame size (64 bytes including CRC,
       but CRC is added by hardware, so pad to 60) */
    if (total < 60) {
        memset(frame + total, 0, 60 - total);
        total = 60;
    }

    return nic->send(frame, total);
}

/* ------------------------------------------------------------------ */
/*  Ethernet RX dispatch (called from e1000 IRQ handler)              */
/* ------------------------------------------------------------------ */

void net_rx_callback(const void *data, uint16_t len) {
    if (len < ETH_HDR_LEN) return;

    const eth_header_t *eth = (const eth_header_t *)data;
    uint16_t type = ntohs(eth->type);
    const void *payload = (const uint8_t *)data + ETH_HDR_LEN;
    uint16_t payload_len = len - ETH_HDR_LEN;

    switch (type) {
    case ETH_TYPE_ARP:
        arp_handle(payload, payload_len);
        break;
    case ETH_TYPE_IP:
        ip_handle(payload, payload_len);
        break;
    }
}

/* ------------------------------------------------------------------ */
/*  IP address parse / format helpers                                 */
/* ------------------------------------------------------------------ */

/*
 * IP addresses are stored in network byte order throughout the stack.
 * We use direct byte access to avoid host/network endianness issues.
 */

/* Parse "a.b.c.d" to network-byte-order uint32_t */
uint32_t ip_parse(const char *str) {
    uint32_t parts[4] = {0, 0, 0, 0};
    int idx = 0;
    const char *p = str;

    while (*p && idx < 4) {
        if (*p >= '0' && *p <= '9') {
            parts[idx] = parts[idx] * 10 + (*p - '0');
        } else if (*p == '.') {
            idx++;
        }
        p++;
    }

    /* Store as bytes: a.b.c.d → byte[0]=a, byte[1]=b, etc.
       This is network byte order regardless of host endianness. */
    uint32_t ip;
    uint8_t *bytes = (uint8_t *)&ip;
    bytes[0] = (uint8_t)parts[0];
    bytes[1] = (uint8_t)parts[1];
    bytes[2] = (uint8_t)parts[2];
    bytes[3] = (uint8_t)parts[3];
    return ip;
}

/* Small helper: write decimal uint8_t to buffer, return chars written */
static int uint_to_str(char *buf, uint8_t val) {
    if (val >= 100) {
        buf[0] = '0' + val / 100;
        buf[1] = '0' + (val / 10) % 10;
        buf[2] = '0' + val % 10;
        return 3;
    } else if (val >= 10) {
        buf[0] = '0' + val / 10;
        buf[1] = '0' + val % 10;
        return 2;
    } else {
        buf[0] = '0' + val;
        return 1;
    }
}

/* Format network-byte-order IP to static "a.b.c.d" string */
const char *ip_fmt(uint32_t ip) {
    static char buf[16];
    const uint8_t *bytes = (const uint8_t *)&ip;

    int pos = 0;
    pos += uint_to_str(buf + pos, bytes[0]);
    buf[pos++] = '.';
    pos += uint_to_str(buf + pos, bytes[1]);
    buf[pos++] = '.';
    pos += uint_to_str(buf + pos, bytes[2]);
    buf[pos++] = '.';
    pos += uint_to_str(buf + pos, bytes[3]);
    buf[pos] = '\0';

    return buf;
}
