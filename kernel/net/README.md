# kernel/net/

Custom networking stack: Ethernet, ARP, IPv4, ICMP, UDP, DHCP. No external libraries — every protocol implemented from scratch.

## What's Here

- **net.c** — Ethernet frame TX/RX, network init, RX callback dispatcher, IP address helpers
- **arp.c** — ARP cache (16 entries), request/reply, blocking resolve with 3-second timeout
- **ip.c** — IPv4 send/receive, RFC 1071 checksum, next-hop routing (same-subnet direct, else gateway)
- **icmp.c** — ICMP echo request/reply, `net_ping()` sends 4 pings at 1-second intervals
- **udp.c** — UDP sockets (8-slot table), blocking recv via wait queues, DHCP port routing
- **dhcp.c** — DHCP client state machine (DISCOVER/OFFER/REQUEST/ACK), raw frame building

## Protocol Stack

```
┌─────────────────────────────────────┐
│           Applications              │
│   ping    udpsend    dhcp_discover  │
├─────────────┬───────────────────────┤
│    ICMP     │         UDP           │
│  icmp.c     │       udp.c           │
├─────────────┴───────────────────────┤
│              IPv4                   │
│             ip.c                    │
├──────────────────┬──────────────────┤
│    Ethernet      │      ARP         │
│    net.c         │     arp.c        │
├──────────────────┴──────────────────┤
│          e1000 NIC Driver           │
│     kernel/drivers/e1000.c          │
└─────────────────────────────────────┘
```

## Packet Flow

```
TX (outbound):
app -> udp_send()/icmp_send() -> ip_send() -> arp_resolve() -> eth_send() -> nic->send()

RX (inbound):
IRQ -> e1000_irq() -> net_rx_callback() -+- ARP (0x0806) -> arp_handle()
                                         +- IPv4 (0x0800) -> ip_handle() -+- ICMP (1) -> icmp_handle()
                                                                          +- UDP (17) -> udp_handle()
```

## DHCP Sequence

```
Client                          Server (QEMU 10.0.2.2)
  |                                |
  |---- DISCOVER (broadcast) ----->|
  |<---- OFFER (10.0.2.15) -------|
  |---- REQUEST (10.0.2.15) ----->|
  |<---- ACK --------------------|
  |                                |
  |  net_cfg: ip=10.0.2.15        |
  |           gw=10.0.2.2         |
  |          dns=10.0.2.3         |
```

## How It Fits Together

All IP addresses are stored in network byte order using direct byte access to avoid endianness confusion. The e1000 NIC driver (`kernel/drivers/e1000.c`) provides the hardware interface — TX via `nic->send()`, RX via `net_rx_callback()` called from the IRQ handler.

DHCP builds raw Ethernet+IP+UDP frames directly (bypassing `ip_send()`) because IP isn't configured during discovery. Once DHCP completes, `net_cfg.configured` is set and the normal `ip_send()` path works.

ARP resolution is blocking (busy-wait up to 3 seconds with `hal_halt()`). UDP receive is truly blocking via `sleep_on()` wait queues. All cache/socket table updates are interrupt-safe via `hal_irq_save/restore`.
