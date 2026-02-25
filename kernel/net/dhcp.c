/*
 * DHCP client for SpikeOS.
 *
 * Implements DISCOVER → OFFER → REQUEST → ACK state machine.
 * Builds raw Ethernet+IP+UDP frames since IP isn't configured yet
 * at the time of DISCOVER/REQUEST (src=0.0.0.0, dst=255.255.255.255).
 */

#include <kernel/net.h>
#include <kernel/e1000.h>
#include <kernel/hal.h>
#include <kernel/timer.h>
#include <stdio.h>
#include <string.h>

/* DHCP state */
enum { DHCP_IDLE, DHCP_DISCOVERING, DHCP_REQUESTING, DHCP_DONE };
static int dhcp_state = DHCP_IDLE;
static uint32_t dhcp_xid;
static uint32_t dhcp_offered_ip;
static uint32_t dhcp_server_ip;

/* DHCP option types */
#define DHCP_OPT_SUBNET     1
#define DHCP_OPT_ROUTER     3
#define DHCP_OPT_DNS        6
#define DHCP_OPT_REQ_IP     50
#define DHCP_OPT_MSG_TYPE   53
#define DHCP_OPT_SERVER_ID  54
#define DHCP_OPT_PARAM_LIST 55
#define DHCP_OPT_END        255

/* ------------------------------------------------------------------ */
/*  Send a raw DHCP packet (bypasses ip_send since we have no IP yet) */
/* ------------------------------------------------------------------ */

static void dhcp_send(uint8_t msg_type, uint32_t req_ip, uint32_t srv_ip) {
    if (!nic) return;

    /* Build a complete Ethernet + IP + UDP + DHCP frame */
    uint8_t frame[600];
    memset(frame, 0, sizeof(frame));

    /* Ethernet header */
    eth_header_t *eth = (eth_header_t *)frame;
    memset(eth->dst, 0xFF, 6);  /* broadcast */
    memcpy(eth->src, nic->mac, 6);
    eth->type = htons(ETH_TYPE_IP);

    /* IP header */
    ip_header_t *ip = (ip_header_t *)(frame + ETH_HDR_LEN);
    ip->ver_ihl    = 0x45;
    ip->tos        = 0;
    ip->ttl        = 64;
    ip->protocol   = IP_PROTO_UDP;
    ip->src_ip     = 0;           /* 0.0.0.0 */
    ip->dst_ip     = 0xFFFFFFFFu; /* 255.255.255.255 */
    ip->flags_frag = 0;
    ip->id         = 0;

    /* UDP header */
    udp_header_t *udp = (udp_header_t *)(frame + ETH_HDR_LEN + 20);
    udp->src_port = htons(DHCP_CLIENT_PORT);
    udp->dst_port = htons(DHCP_SERVER_PORT);
    udp->checksum = 0;

    /* DHCP header */
    dhcp_header_t *dhcp = (dhcp_header_t *)(frame + ETH_HDR_LEN + 20 + 8);
    dhcp->op    = 1;  /* BOOTREQUEST */
    dhcp->htype = 1;  /* Ethernet */
    dhcp->hlen  = 6;
    dhcp->hops  = 0;
    dhcp->xid   = dhcp_xid;
    dhcp->secs  = 0;
    dhcp->flags = htons(0x8000);  /* broadcast flag */
    memcpy(dhcp->chaddr, nic->mac, 6);
    dhcp->magic = htonl(DHCP_MAGIC);

    /* DHCP options */
    uint8_t *opts = (uint8_t *)(dhcp + 1);
    int pos = 0;

    /* Option 53: DHCP Message Type */
    opts[pos++] = DHCP_OPT_MSG_TYPE;
    opts[pos++] = 1;
    opts[pos++] = msg_type;

    if (msg_type == DHCP_REQUEST) {
        /* Option 50: Requested IP */
        opts[pos++] = DHCP_OPT_REQ_IP;
        opts[pos++] = 4;
        memcpy(&opts[pos], &req_ip, 4);
        pos += 4;

        /* Option 54: Server Identifier */
        opts[pos++] = DHCP_OPT_SERVER_ID;
        opts[pos++] = 4;
        memcpy(&opts[pos], &srv_ip, 4);
        pos += 4;
    }

    /* Option 55: Parameter Request List */
    opts[pos++] = DHCP_OPT_PARAM_LIST;
    opts[pos++] = 3;
    opts[pos++] = DHCP_OPT_SUBNET;
    opts[pos++] = DHCP_OPT_ROUTER;
    opts[pos++] = DHCP_OPT_DNS;

    /* End */
    opts[pos++] = DHCP_OPT_END;

    /* Fill in lengths */
    uint16_t dhcp_len = sizeof(dhcp_header_t) + pos;
    uint16_t udp_len  = 8 + dhcp_len;
    uint16_t ip_len   = 20 + udp_len;
    uint16_t frame_len = ETH_HDR_LEN + ip_len;

    udp->length    = htons(udp_len);
    ip->total_len  = htons(ip_len);
    ip->checksum   = 0;
    ip->checksum   = ip_checksum(ip, 20);

    /* Pad to minimum frame size */
    if (frame_len < 60) frame_len = 60;

    nic->send(frame, frame_len);
}

/* ------------------------------------------------------------------ */
/*  DHCP discover — start the process                                 */
/* ------------------------------------------------------------------ */

void dhcp_discover(void) {
    if (!nic) return;

    dhcp_xid = (uint32_t)timer_ticks() ^ 0xDEAD;
    dhcp_state = DHCP_DISCOVERING;

    dhcp_send(DHCP_DISCOVER, 0, 0);
}

/* ------------------------------------------------------------------ */
/*  DHCP RX handler (called from udp_handle for port 68)              */
/* ------------------------------------------------------------------ */

void dhcp_handle(const void *data, uint16_t len) {
    if (len < sizeof(dhcp_header_t)) return;

    const dhcp_header_t *dhcp = (const dhcp_header_t *)data;

    /* Verify this is a reply to our transaction */
    if (dhcp->op != 2) return;           /* must be BOOTREPLY */
    if (dhcp->xid != dhcp_xid) return;   /* must match our xid */
    if (ntohl(dhcp->magic) != DHCP_MAGIC) return;

    /* Parse DHCP options */
    const uint8_t *opts = (const uint8_t *)(dhcp + 1);
    uint16_t opts_len = len - sizeof(dhcp_header_t);

    uint8_t  msg_type = 0;
    uint32_t subnet   = 0;
    uint32_t router   = 0;
    uint32_t dns      = 0;
    uint32_t server_id = 0;

    int i = 0;
    while (i < opts_len) {
        uint8_t opt = opts[i++];
        if (opt == DHCP_OPT_END) break;
        if (opt == 0) continue;  /* padding */

        if (i >= opts_len) break;
        uint8_t opt_len = opts[i++];
        if (i + opt_len > opts_len) break;

        switch (opt) {
        case DHCP_OPT_MSG_TYPE:
            if (opt_len >= 1) msg_type = opts[i];
            break;
        case DHCP_OPT_SUBNET:
            if (opt_len >= 4) memcpy(&subnet, &opts[i], 4);
            break;
        case DHCP_OPT_ROUTER:
            if (opt_len >= 4) memcpy(&router, &opts[i], 4);
            break;
        case DHCP_OPT_DNS:
            if (opt_len >= 4) memcpy(&dns, &opts[i], 4);
            break;
        case DHCP_OPT_SERVER_ID:
            if (opt_len >= 4) memcpy(&server_id, &opts[i], 4);
            break;
        }
        i += opt_len;
    }

    if (dhcp_state == DHCP_DISCOVERING && msg_type == DHCP_OFFER) {
        dhcp_offered_ip = dhcp->yiaddr;
        dhcp_server_ip  = server_id ? server_id : dhcp->siaddr;
        dhcp_state = DHCP_REQUESTING;

        dhcp_send(DHCP_REQUEST, dhcp_offered_ip, dhcp_server_ip);
    }
    else if (dhcp_state == DHCP_REQUESTING && msg_type == DHCP_ACK) {
        net_cfg.ip        = dhcp->yiaddr;
        net_cfg.subnet    = subnet;
        net_cfg.gateway   = router;
        net_cfg.dns       = dns;
        net_cfg.configured = 1;
        dhcp_state = DHCP_DONE;

        printf("[net] DHCP: IP=%s", ip_fmt(net_cfg.ip));
        printf(" GW=%s", ip_fmt(net_cfg.gateway));
        printf(" DNS=%s\n", ip_fmt(net_cfg.dns));
    }
}
