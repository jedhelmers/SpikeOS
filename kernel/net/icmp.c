/*
 * ICMP echo request/reply for SpikeOS.
 *
 * Handles incoming echo requests (sends reply) and echo replies
 * (wakes up net_ping waiter). Provides net_ping() for the shell.
 */

#include <kernel/net.h>
#include <kernel/e1000.h>
#include <kernel/hal.h>
#include <kernel/timer.h>
#include <kernel/wait.h>
#include <stdio.h>
#include <string.h>

static volatile int ping_received;
static volatile uint16_t ping_recv_seq;
static wait_queue_t ping_wq = WAIT_QUEUE_INIT;

/* ------------------------------------------------------------------ */
/*  ICMP RX handler                                                   */
/* ------------------------------------------------------------------ */

void icmp_handle(const void *data, uint16_t len, uint32_t src_ip) {
    if (len < sizeof(icmp_header_t)) return;

    const icmp_header_t *icmp = (const icmp_header_t *)data;

    if (icmp->type == ICMP_ECHO_REQUEST && icmp->code == 0) {
        /* Send echo reply — swap type, recompute checksum */
        uint8_t reply_buf[ETH_MTU];
        if (len > ETH_MTU - 20) return;  /* too large */

        memcpy(reply_buf, data, len);
        icmp_header_t *reply = (icmp_header_t *)reply_buf;
        reply->type = ICMP_ECHO_REPLY;
        reply->checksum = 0;
        reply->checksum = ip_checksum(reply, len);

        ip_send(src_ip, IP_PROTO_ICMP, reply_buf, len);
    }
    else if (icmp->type == ICMP_ECHO_REPLY && icmp->code == 0) {
        ping_recv_seq = ntohs(icmp->seq);
        ping_received = 1;
        wake_up_all(&ping_wq);
    }
}

/* ------------------------------------------------------------------ */
/*  Send ICMP echo request                                            */
/* ------------------------------------------------------------------ */

int icmp_send_echo(uint32_t dst_ip, uint16_t id, uint16_t seq) {
    /* Build ICMP echo request with 32 bytes of payload */
    uint8_t buf[sizeof(icmp_header_t) + 32];
    icmp_header_t *icmp = (icmp_header_t *)buf;

    icmp->type     = ICMP_ECHO_REQUEST;
    icmp->code     = 0;
    icmp->checksum = 0;
    icmp->id       = htons(id);
    icmp->seq      = htons(seq);

    /* Fill payload with pattern */
    uint8_t *payload = buf + sizeof(icmp_header_t);
    for (int i = 0; i < 32; i++)
        payload[i] = (uint8_t)('A' + (i % 26));

    icmp->checksum = ip_checksum(buf, sizeof(buf));

    return ip_send(dst_ip, IP_PROTO_ICMP, buf, sizeof(buf));
}

/* ------------------------------------------------------------------ */
/*  net_ping — send 4 pings, print results                           */
/* ------------------------------------------------------------------ */

int net_ping(uint32_t dst_ip) {
    if (!nic || !net_cfg.configured) {
        printf("Network not configured\n");
        return -1;
    }

    printf("PING %s\n", ip_fmt(dst_ip));

    int sent = 0, received = 0;
    uint16_t id = (uint16_t)(timer_ticks() & 0xFFFF);

    for (int seq = 1; seq <= 4; seq++) {
        ping_received = 0;
        uint32_t start = timer_ticks();

        if (icmp_send_echo(dst_ip, id, seq) != 0) {
            printf("  send failed (seq=%d)\n", seq);
            sent++;
            continue;
        }
        sent++;

        /* Wait up to 2 seconds for reply */
        uint32_t deadline = start + 200;
        while (!ping_received && timer_ticks() < deadline) {
            hal_irq_enable();
            hal_halt();
        }

        if (ping_received) {
            uint32_t elapsed = timer_ticks() - start;
            uint32_t ms = elapsed * 10;  /* 100Hz → 10ms per tick */
            printf("Reply from %s: seq=%d time=%dms\n",
                   ip_fmt(dst_ip), seq, ms);
            received++;
        } else {
            printf("Request timeout (seq=%d)\n", seq);
        }

        /* 1-second delay between pings (except after last) */
        if (seq < 4) {
            uint32_t wait_until = timer_ticks() + 100;
            while (timer_ticks() < wait_until) {
                hal_irq_enable();
                hal_halt();
            }
        }
    }

    printf("--- %d packets sent, %d received ---\n", sent, received);
    return received > 0 ? 0 : -1;
}
