/*
 * udp_test.c — userland UDP networking test for SpikeOS.
 *
 * Creates a UDP socket, binds to port 9999, sends "hello" to
 * the gateway (10.0.2.2:12345), then waits for a reply.
 */
#include "libc/stdio.h"
#include "libc/unistd.h"
#include "libc/string.h"

/* Build a network-byte-order IPv4 address from 4 octets */
static unsigned int make_ip(unsigned char a, unsigned char b,
                            unsigned char c, unsigned char d) {
    unsigned int ip;
    unsigned char *bytes = (unsigned char *)&ip;
    bytes[0] = a;
    bytes[1] = b;
    bytes[2] = c;
    bytes[3] = d;
    return ip;
}

int main(void) {
    printf("[udp_test] PID %d\n", getpid());

    /* Bind a UDP socket to port 9999 */
    int sock = spike_bind(SOCK_UDP, 9999);
    if (sock < 0) {
        printf("[udp_test] bind failed\n");
        return 1;
    }
    printf("[udp_test] bound socket %d to port 9999\n", sock);

    /* Send "hello" to gateway:12345 */
    unsigned int gw_ip = make_ip(10, 0, 2, 2);
    const char *msg = "hello from userland!";
    struct sendto_args sa;
    sa.dst_ip   = gw_ip;
    sa.dst_port = 12345;
    sa.buf      = msg;
    sa.len      = (unsigned short)strlen(msg);

    int ret = spike_sendto(sock, &sa);
    if (ret < 0) {
        printf("[udp_test] sendto failed\n");
    } else {
        printf("[udp_test] sent %d bytes to 10.0.2.2:12345\n", sa.len);
    }

    printf("[udp_test] waiting for reply on port 9999...\n");

    /* Wait for a reply (blocks) */
    char buf[256];
    struct recvfrom_args ra;
    ra.buf     = buf;
    ra.max_len = sizeof(buf) - 1;

    ret = spike_recvfrom(sock, &ra);
    if (ret >= 0) {
        buf[ra.received] = '\0';
        unsigned char *ip = (unsigned char *)&ra.from_ip;
        printf("[udp_test] received %d bytes from %d.%d.%d.%d:%d: %s\n",
               ra.received, ip[0], ip[1], ip[2], ip[3], ra.from_port, buf);
    } else {
        printf("[udp_test] recvfrom failed\n");
    }

    spike_closesock(sock);
    printf("[udp_test] done\n");
    return 0;
}
