/* Yart OS - networking (row 16).
 *
 * A minimal but real IP stack: an e1000 NIC driver (QEMU's default 82540EM)
 * + Ethernet + ARP + IPv4 + ICMP echo + UDP + a DHCP client.  Together they
 * let the OS get an address and talk UDP over QEMU's user-mode (slirp)
 * networking - which the boot test uses to prove a real round-trip (DHCP ->
 * ARP -> IPv4 -> UDP out and back).
 *
 * Frames are handled by polling (net_service, called from the idle/main loop)
 * rather than interrupts: simpler and robust under QEMU/TCG.  The e1000 driver
 * maintains its own RX/TX descriptor rings.
 */
#pragma once
#include <yart/types.h>
#include <yart/hal.h>   /* cpu_regs_t for the e1000 IRQ handler */

/* ---- NIC driver (drivers/e1000.c) ---- */
void   nic_init(void);                /* probe + bring up the e1000        */
bool   nic_present(void);
void   nic_mac(u8 out[6]);            /* the burned-in MAC                  */
void   e1000_irq_handler(cpu_regs_t *r);  /* clear the device IRQ (polled) */
u8     e1000_irq_line(void);              /* PCI INTx line (0 = none)      */
int    nic_send(const u8 *frame, u16 len);   /* 0 = OK, -1 = busy/drop     */
/* Pull the next received frame out of the driver's queue (0 = none). */
int    nic_rx(u8 *out, u16 cap);

/* ---- net_service (net/net.c): handle inbound + drive DHCP/TX ---- */
void   net_init(void);                /* after nic_init; starts DHCP        */
void   net_service(void);             /* called from the main loop           */
void   net_get_addrs(u32 *ip, u32 *gw, u32 *dns, u32 *mask);  /* host order */

/* UDP userland-facing API (minimal sockets) */
int    net_udp_send(u32 dst_ip, u16 dport, const u8 *buf, u16 len);
int    net_udp_recv(u8 *buf, u16 cap);   /* 0 = nothing yet, >0 = datagram */

/* ---- byte-order helpers (little-endian host) ---- */
static inline u16 ntoh16(u16 x) { return (u16)((x >> 8) | (x << 8)); }
static inline u16 hton16(u16 x) { return ntoh16(x); }
static inline u32 ntoh32(u32 x) {
    return ((x & 0xFF) << 24) | ((x & 0xFF00) << 8) |
           ((x & 0xFF0000) >> 8) | ((x >> 24) & 0xFF);
}
static inline u32 hton32(u32 x) { return ntoh32(x); }

/* ethertypes */
#define ETH_IPV4 0x0800
#define ETH_ARP  0x0806

/* UDP service ports */
#define UDP_DHCP_CLIENT 68
#define UDP_DHCP_SERVER 67
#define UDP_DNS         53
