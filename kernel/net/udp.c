/*
 * UDP datagram send/receive for SpikeOS.
 *
 * Simple 8-slot socket table with per-socket receive buffer
 * and blocking recv via wait queues.
 */

#include <kernel/net.h>
#include <kernel/e1000.h>
#include <kernel/hal.h>
#include <kernel/wait.h>
#include <stdio.h>
#include <string.h>

#define MAX_UDP_SOCKETS 8
#define UDP_RECV_BUF    2048

typedef struct {
    int          in_use;
    uint16_t     local_port;
    uint8_t      recv_buf[UDP_RECV_BUF];
    uint16_t     recv_len;
    uint32_t     from_ip;
    uint16_t     from_port;
    int          has_data;
    wait_queue_t wq;
} udp_socket_t;

static udp_socket_t udp_sockets[MAX_UDP_SOCKETS];

/* ------------------------------------------------------------------ */
/*  Init                                                              */
/* ------------------------------------------------------------------ */

void udp_init(void) {
    memset(udp_sockets, 0, sizeof(udp_sockets));
}

/* ------------------------------------------------------------------ */
/*  Bind / unbind                                                     */
/* ------------------------------------------------------------------ */

int udp_bind(uint16_t port) {
    uint32_t flags = hal_irq_save();
    for (int i = 0; i < MAX_UDP_SOCKETS; i++) {
        if (!udp_sockets[i].in_use) {
            udp_sockets[i].in_use = 1;
            udp_sockets[i].local_port = port;
            udp_sockets[i].has_data = 0;
            udp_sockets[i].recv_len = 0;
            udp_sockets[i].wq = (wait_queue_t)WAIT_QUEUE_INIT;
            hal_irq_restore(flags);
            return i;
        }
    }
    hal_irq_restore(flags);
    return -1;  /* no free slots */
}

void udp_unbind(int sock) {
    if (sock < 0 || sock >= MAX_UDP_SOCKETS) return;
    uint32_t flags = hal_irq_save();
    udp_sockets[sock].in_use = 0;
    wake_up_all(&udp_sockets[sock].wq);
    hal_irq_restore(flags);
}

/* ------------------------------------------------------------------ */
/*  UDP send                                                          */
/* ------------------------------------------------------------------ */

int udp_send(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
             const void *data, uint16_t data_len) {
    uint16_t udp_total = sizeof(udp_header_t) + data_len;
    uint8_t buf[1500];

    if (udp_total > sizeof(buf)) return -1;

    udp_header_t *udp = (udp_header_t *)buf;
    udp->src_port = htons(src_port);
    udp->dst_port = htons(dst_port);
    udp->length   = htons(udp_total);
    udp->checksum  = 0;  /* checksum optional for UDP/IPv4 */

    memcpy(buf + sizeof(udp_header_t), data, data_len);

    return ip_send(dst_ip, IP_PROTO_UDP, buf, udp_total);
}

/* ------------------------------------------------------------------ */
/*  UDP sendto (via socket index)                                     */
/* ------------------------------------------------------------------ */

int udp_sendto(int sock, uint32_t dst_ip, uint16_t dst_port,
               const void *data, uint16_t data_len) {
    if (sock < 0 || sock >= MAX_UDP_SOCKETS) return -1;
    if (!udp_sockets[sock].in_use) return -1;
    return udp_send(dst_ip, udp_sockets[sock].local_port, dst_port,
                    data, data_len);
}

/* ------------------------------------------------------------------ */
/*  UDP recv (blocking)                                               */
/* ------------------------------------------------------------------ */

int udp_recv(int sock, void *buf, uint16_t max_len,
             uint32_t *from_ip, uint16_t *from_port) {
    if (sock < 0 || sock >= MAX_UDP_SOCKETS) return -1;
    udp_socket_t *s = &udp_sockets[sock];
    if (!s->in_use) return -1;

    /* Block until data arrives */
    while (!s->has_data && s->in_use) {
        sleep_on(&s->wq);
    }
    if (!s->in_use) return -1;  /* socket was closed while waiting */

    uint32_t flags = hal_irq_save();
    uint16_t copy = s->recv_len;
    if (copy > max_len) copy = max_len;
    memcpy(buf, s->recv_buf, copy);
    if (from_ip)   *from_ip   = s->from_ip;
    if (from_port) *from_port = s->from_port;
    s->has_data = 0;
    hal_irq_restore(flags);

    return copy;
}

/* ------------------------------------------------------------------ */
/*  UDP RX handler (called from ip_handle)                            */
/* ------------------------------------------------------------------ */

void udp_handle(const void *data, uint16_t len, uint32_t src_ip) {
    if (len < sizeof(udp_header_t)) return;

    const udp_header_t *udp = (const udp_header_t *)data;
    uint16_t dst_port = ntohs(udp->dst_port);
    uint16_t src_port = ntohs(udp->src_port);
    uint16_t udp_len  = ntohs(udp->length);

    if (udp_len < sizeof(udp_header_t) || udp_len > len) return;

    const void *payload = (const uint8_t *)data + sizeof(udp_header_t);
    uint16_t payload_len = udp_len - sizeof(udp_header_t);

    /* Route DHCP replies to DHCP handler */
    if (dst_port == DHCP_CLIENT_PORT) {
        dhcp_handle(payload, payload_len);
        return;
    }

    /* Deliver to bound socket matching destination port */
    uint32_t flags = hal_irq_save();
    for (int i = 0; i < MAX_UDP_SOCKETS; i++) {
        if (udp_sockets[i].in_use &&
            udp_sockets[i].local_port == dst_port) {

            uint16_t copy = payload_len;
            if (copy > UDP_RECV_BUF) copy = UDP_RECV_BUF;
            memcpy(udp_sockets[i].recv_buf, payload, copy);
            udp_sockets[i].recv_len  = copy;
            udp_sockets[i].from_ip   = src_ip;
            udp_sockets[i].from_port = src_port;
            udp_sockets[i].has_data  = 1;

            hal_irq_restore(flags);
            wake_up_one(&udp_sockets[i].wq);
            return;
        }
    }
    hal_irq_restore(flags);
}
