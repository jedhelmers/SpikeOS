#ifndef _NET_H
#define _NET_H

#include <stdint.h>

/* ================================================================== */
/*  Byte-order helpers (x86 is little-endian, network is big-endian)  */
/* ================================================================== */

static inline uint16_t htons(uint16_t h) {
    return (uint16_t)((h >> 8) | (h << 8));
}
static inline uint16_t ntohs(uint16_t n) { return htons(n); }

static inline uint32_t htonl(uint32_t h) {
    return ((h >> 24) & 0x000000FFu) |
           ((h >>  8) & 0x0000FF00u) |
           ((h <<  8) & 0x00FF0000u) |
           ((h << 24) & 0xFF000000u);
}
static inline uint32_t ntohl(uint32_t n) { return htonl(n); }

/* ================================================================== */
/*  Ethernet                                                          */
/* ================================================================== */

#define ETH_ADDR_LEN  6
#define ETH_HDR_LEN   14
#define ETH_MTU       1500
#define ETH_FRAME_MAX (ETH_HDR_LEN + ETH_MTU)

#define ETH_TYPE_ARP  0x0806
#define ETH_TYPE_IP   0x0800

typedef struct __attribute__((packed)) {
    uint8_t  dst[ETH_ADDR_LEN];
    uint8_t  src[ETH_ADDR_LEN];
    uint16_t type;              /* big-endian */
} eth_header_t;

/* ================================================================== */
/*  ARP                                                               */
/* ================================================================== */

#define ARP_HW_ETHER   1
#define ARP_OP_REQUEST  1
#define ARP_OP_REPLY    2

typedef struct __attribute__((packed)) {
    uint16_t htype;     /* Hardware type (1 = Ethernet) */
    uint16_t ptype;     /* Protocol type (0x0800 = IPv4) */
    uint8_t  hlen;      /* Hardware address length (6) */
    uint8_t  plen;      /* Protocol address length (4) */
    uint16_t oper;      /* Operation (1=request, 2=reply) */
    uint8_t  sha[6];    /* Sender hardware address */
    uint32_t spa;       /* Sender protocol address */
    uint8_t  tha[6];    /* Target hardware address */
    uint32_t tpa;       /* Target protocol address */
} arp_header_t;

#define ARP_CACHE_SIZE 16

typedef struct {
    uint32_t ip;
    uint8_t  mac[6];
    uint32_t timestamp;  /* timer ticks when added */
    int      valid;
} arp_entry_t;

/* ================================================================== */
/*  IPv4                                                              */
/* ================================================================== */

#define IP_PROTO_ICMP  1
#define IP_PROTO_UDP   17

typedef struct __attribute__((packed)) {
    uint8_t  ver_ihl;     /* version (4) | IHL (5) */
    uint8_t  tos;
    uint16_t total_len;   /* big-endian */
    uint16_t id;
    uint16_t flags_frag;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    uint32_t src_ip;
    uint32_t dst_ip;
} ip_header_t;

/* ================================================================== */
/*  ICMP                                                              */
/* ================================================================== */

#define ICMP_ECHO_REPLY    0
#define ICMP_ECHO_REQUEST  8

typedef struct __attribute__((packed)) {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} icmp_header_t;

/* ================================================================== */
/*  UDP                                                               */
/* ================================================================== */

typedef struct __attribute__((packed)) {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;     /* header + data */
    uint16_t checksum;   /* 0 = no checksum (valid for UDP/IPv4) */
} udp_header_t;

/* ================================================================== */
/*  DHCP                                                              */
/* ================================================================== */

#define DHCP_SERVER_PORT  67
#define DHCP_CLIENT_PORT  68
#define DHCP_MAGIC        0x63825363u

#define DHCP_DISCOVER  1
#define DHCP_OFFER     2
#define DHCP_REQUEST   3
#define DHCP_ACK       5

typedef struct __attribute__((packed)) {
    uint8_t  op;         /* 1=request, 2=reply */
    uint8_t  htype;      /* 1=Ethernet */
    uint8_t  hlen;       /* 6 */
    uint8_t  hops;
    uint32_t xid;        /* Transaction ID */
    uint16_t secs;
    uint16_t flags;
    uint32_t ciaddr;     /* Client IP */
    uint32_t yiaddr;     /* Your IP (offered) */
    uint32_t siaddr;     /* Server IP */
    uint32_t giaddr;     /* Gateway IP */
    uint8_t  chaddr[16]; /* Client hardware address */
    uint8_t  sname[64];  /* Server name */
    uint8_t  file[128];  /* Boot file */
    uint32_t magic;      /* DHCP magic cookie */
} dhcp_header_t;

/* ================================================================== */
/*  Network configuration                                             */
/* ================================================================== */

typedef struct {
    uint32_t ip;         /* our IP (network byte order) */
    uint32_t subnet;     /* subnet mask */
    uint32_t gateway;    /* default gateway */
    uint32_t dns;        /* DNS server */
    int      configured; /* 1 after DHCP or manual config */
} net_config_t;

extern net_config_t net_cfg;

/* ================================================================== */
/*  API — net.c                                                       */
/* ================================================================== */

void net_init(void);
void net_rx_callback(const void *data, uint16_t len);
int  eth_send(const uint8_t *dst_mac, uint16_t type,
              const void *payload, uint16_t payload_len);

/* IP address parse/format helpers */
uint32_t ip_parse(const char *str);
const char *ip_fmt(uint32_t ip);

/* ================================================================== */
/*  API — arp.c                                                       */
/* ================================================================== */

void arp_init(void);
void arp_handle(const void *data, uint16_t len);
void arp_request(uint32_t target_ip);
int  arp_resolve(uint32_t ip, uint8_t *mac_out);

/* ================================================================== */
/*  API — ip.c                                                        */
/* ================================================================== */

void     ip_handle(const void *data, uint16_t len);
int      ip_send(uint32_t dst_ip, uint8_t protocol,
                 const void *payload, uint16_t payload_len);
uint16_t ip_checksum(const void *data, uint16_t len);

/* ================================================================== */
/*  API — icmp.c                                                      */
/* ================================================================== */

void icmp_handle(const void *data, uint16_t len, uint32_t src_ip);
int  icmp_send_echo(uint32_t dst_ip, uint16_t id, uint16_t seq);
int  net_ping(uint32_t dst_ip);

/* ================================================================== */
/*  API — udp.c                                                       */
/* ================================================================== */

void udp_init(void);
void udp_handle(const void *data, uint16_t len, uint32_t src_ip);
int  udp_send(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
              const void *data, uint16_t data_len);
int  udp_sendto(int sock, uint32_t dst_ip, uint16_t dst_port,
                const void *data, uint16_t data_len);
int  udp_bind(uint16_t port);
void udp_unbind(int sock);
int  udp_recv(int sock, void *buf, uint16_t max_len,
              uint32_t *from_ip, uint16_t *from_port);

/* ================================================================== */
/*  API — dhcp.c                                                      */
/* ================================================================== */

void dhcp_discover(void);
void dhcp_handle(const void *data, uint16_t len);

#endif
