/* Yart OS - Ethernet/ARP/IPv4/ICMP/UDP/DHCP (row 16).
 *
 * All processing is driven by net_service() (called from the idle/main loop):
 * it drains the e1000 RX FIFO, handles inbound ARP/ICMP/UDP, drives the DHCP
 * client, and serves the small UDP socket API used by the boot test.
 * Addresses are stored in host byte order; they are converted (hton/ntoh) at
 * packet boundaries.
 */
#include <yart/net.h>
#include <yart/console.h>
#include <yart/string.h>
#include <yart/io.h>        /* pit_ticks for timers + entropy */
#include <yart/hal.h>
#include <yart/mm.h>

static void net_pump(void);   /* RX pump (defined with net_service) */

/* ---- state ---- */
static u8  g_mac[6];
static u32 g_ip, g_mask, g_gw, g_dns;     /* host order */
static u16 g_udp_src_port = 7777;

/* ARP cache (small) */
#define ARP_CACHE 8
static struct { u32 ip; u8 mac[6]; bool valid; } g_arp[ARP_CACHE];

/* DHCP */
#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5
#define DHCP_NAK      6
#define DHCP_DONE     99
static int  g_dhcp_state;
static u32  g_xid;
static u64  g_dhcp_last;      /* pit_ticks of last sent message */
static u32  g_offer_ip;
static u32  g_server_id;

/* UDP receive buffer (demo socket) */
#define UDP_RXBUF 2048
static u8   g_udp_rx[UDP_RXBUF];
static u16  g_udp_rx_len;

/* DNS client state: one outstanding transaction at a time.  The response
 * is demuxed in process_udp on its own client port. */
#define DNS_CLIENT_PORT 5353
#define DNS_RXBUF 512
static u8   g_dns_rx[DNS_RXBUF];
static u16  g_dns_rx_len;
static u16  g_dns_id;

static int udp_raw_send(u32 src_ip, u32 dst_ip, u16 sport, u16 dport,
                        const u8 *buf, u16 len);   /* defined below */

/* ---- DNS (RFC 1035): encode "www.example.com" -> 3www7example3com0 ---- */
static int dns_encode_name(const char *host, u8 *out) {
    int o = 0;
    const char *p = host;
    if (!p || !*p) return -1;
    while (*p) {
        const char *d = p;
        while (*d && *d != '.') d++;
        int l = (int)(d - p);
        if (l <= 0 || l > 63 || o + l + 2 > 250) return -1;
        out[o++] = (u8)l;
        memcpy(out + o, p, (size_t)l);
        o += l;
        p = (*d) ? d + 1 : d;
    }
    out[o++] = 0;
    return o;
}

/* Resolve a hostname to an IPv4 address (blocking, ~1 s worst case).
 * Sends the query via the normal UDP path, drives inbound processing
 * while waiting, then parses the answer (handles compression pointers
 * and CNAME chains).  Returns 0 + *out_ip on success. */
int net_dns_resolve(const char *host, u32 *out_ip) {
    if (!g_ip || !g_dns || !host || !out_ip) return -1;
    u8 q[300]; memset(q, 0, sizeof q);
    g_dns_id = (u16)(pit_ticks() ^ (host[0] << 8) ^ (u16)(u64)g_mac);
    q[0] = (u8)(g_dns_id >> 8); q[1] = (u8)(g_dns_id & 0xFF);
    q[2] = 0x01; q[3] = 0x00;              /* RD */
    q[5] = 0x01;                           /* QDCOUNT = 1 */
    int o = dns_encode_name(host, q + 12);
    if (o < 0) return -1;
    o += 12;
    q[o++] = 0; q[o++] = 1;                /* QTYPE = A   */
    q[o++] = 0; q[o++] = 1;                /* QCLASS = IN */

    g_dns_rx_len = 0;
    for (int attempt = 0; attempt < 2 && !g_dns_rx_len; attempt++) {
        if (udp_raw_send(g_ip, g_dns, DNS_CLIENT_PORT, UDP_DNS, q, (u16)o) != 0)
            break;
        u64 t0 = pit_ticks();
        while (pit_ticks() - t0 < 40 && !g_dns_rx_len) {
            net_pump();                    /* process the reply */
            __asm__ volatile("pause");
        }
    }
    if (!g_dns_rx_len) return -1;
    const u8 *r = g_dns_rx;
    int len = g_dns_rx_len;
    if (len < 12) return -1;
    if (r[0] != (u8)(g_dns_id >> 8) || r[1] != (u8)(g_dns_id & 0xFF)) return -1;
    if (!(r[2] & 0x80)) return -1;         /* QR (response) */
    if ((r[3] & 0x0F) != 0) return -1;     /* RCODE != 0 (NXDOMAIN etc) */
    u16 qd = (u16)((r[4] << 8) | r[5]);
    u16 an = (u16)((r[6] << 8) | r[7]);
    int off = 12;
    /* skip the question section */
    for (int i = 0; i < qd; i++) {
        while (off < len && r[off]) {
            if ((r[off] & 0xC0) == 0xC0) { off += 2; break; }
            off += r[off] + 1;
        }
        off += 1 + 4;
        if (off > len) return -1;
    }
    /* walk answers: follow CNAMEs, take the first A record */
    for (int i = 0; i < an; i++) {
        if (off >= len) return -1;
        if ((r[off] & 0xC0) == 0xC0) off += 2;      /* name = pointer */
        else {
            while (off < len && r[off]) {
                if ((r[off] & 0xC0) == 0xC0) { off += 2; break; }
                off += r[off] + 1;
            }
            off += 1;
        }
        if (off + 10 > len) return -1;
        u16 type  = (u16)((r[off] << 8) | r[off + 1]);
        u16 cls   = (u16)((r[off + 2] << 8) | r[off + 3]);
        u16 rdlen = (u16)((r[off + 8] << 8) | r[off + 9]);
        off += 10;
        if (off + rdlen > len) return -1;
        if (type == 1 && cls == 1 && rdlen == 4) {  /* A record */
            memcpy(out_ip, r + off, 4);
            return 0;
        }
        off += rdlen;                      /* skip CNAME/other */
    }
    return -1;
}

extern void nic_poll(void);

/* ---- internet checksum (RFC 1071), over big-endian words ---- */
u16 net_csum(const u8 *data, int len) {
    u32 sum = 0;
    for (int i = 0; i < len; i += 2) {
        u16 w = (i + 1 < len) ? (u16)((data[i] << 8) | data[i + 1])
                              : (u16)(data[i] << 8);
        sum += w;
        if (sum & 0xFFFF0000) { sum &= 0xFFFF; sum++; }
    }
    sum = (sum >> 16) + (sum & 0xFFFF);
    sum += (sum >> 16);
    return (u16)~sum;
}

/* ---- ARP cache ---- */
static u8 *arp_lookup(u32 ip) {
    for (int i = 0; i < ARP_CACHE; i++)
        if (g_arp[i].valid && g_arp[i].ip == ip) return g_arp[i].mac;
    return 0;
}
static void arp_store(u32 ip, const u8 mac[6]) {
    u8 *e = arp_lookup(ip);
    if (e) { memcpy(e, mac, 6); return; }
    for (int i = 0; i < ARP_CACHE; i++)
        if (!g_arp[i].valid) { g_arp[i].ip = ip; memcpy(g_arp[i].mac, mac, 6); g_arp[i].valid = true; return; }
}

/* ---- low-level frame send ---- */
static int send_frame(const u8 *frame, u16 len) { return nic_send(frame, len); }

/* ---- Ethernet ---- */
static const u8 BROADCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

/* ---- ARP ---- */
static void arp_send(u16 oper, u32 srcip, u32 dstip, const u8 *dstmac) {
    u8 f[64]; memset(f, 0, sizeof f);
    memcpy(f, BROADCAST, 6);
    memcpy(f + 6, g_mac, 6);
    f[12] = 0x08; f[13] = 0x06;
    u8 *a = f + 14;
    a[0] = 0; a[1] = 1;                  /* ethernet */
    a[2] = 0x08; a[3] = 0x00;            /* IPv4 */
    a[4] = 6; a[5] = 4;
    a[6] = oper >> 8; a[7] = oper & 0xFF;
    memcpy(a + 8, g_mac, 6);             /* sha */
    u32 s = hton32(srcip); memcpy(a + 14, &s, 4);
    memcpy(a + 18, dstmac, 6);           /* tha */
    u32 d = hton32(dstip); memcpy(a + 24, &d, 4);
    send_frame(f, 42);
}

/* ---- IPv4 + ICMP + UDP send ---- */

/* Next-hop selection: destinations on the local subnet go directly; the
 * rest go through the gateway (the IP header still carries the original
 * destination - only the ARP/Ethernet target changes). */
static u32 next_hop(u32 dst) {
    if (dst == 0xFFFFFFFFu) return 0xFFFFFFFFu;
    if (g_mask == 0 || g_ip == 0) return dst;          /* no config yet */
    if ((dst & g_mask) == (g_ip & g_mask)) return dst; /* local subnet  */
    if (g_gw == 0) return 0;                           /* no route      */
    return g_gw;
}

int net_ip_send(u32 src, u32 dst, u8 proto, const u8 *payload, u16 plen) {
    u8 *mac;
    u32 nh = next_hop(dst);
    if (nh == 0xFFFFFFFFu) {
        mac = (u8 *)BROADCAST;
    } else if (nh == 0) {
        return -1;   /* no route to host */
    } else if ((mac = arp_lookup(nh)) == 0) {
        return -1;   /* need ARP first */
    }
    u16 total = 20 + plen;
    u8 f[1518]; memset(f, 0, sizeof f);
    memcpy(f, mac, 6);
    memcpy(f + 6, g_mac, 6);
    f[12] = 0x08; f[13] = 0x00;
    u8 *ip = f + 14;
    ip[0] = 0x45;
    ip[2] = total >> 8; ip[3] = total & 0xFF;
    ip[8] = 64;                            /* TTL */
    ip[9] = proto;
    u32 s = hton32(src); memcpy(ip + 12, &s, 4);
    u32 d = hton32(dst);  memcpy(ip + 16, &d, 4);
    u16 chk = net_csum(ip, 20);
    ip[10] = chk >> 8; ip[11] = chk & 0xFF;
    memcpy(f + 34, payload, plen);
    return send_frame(f, 14 + total);
}
static int ip_send(u32 dst, u8 proto, const u8 *payload, u16 plen) {
    return net_ip_send(g_ip, dst, proto, payload, plen);
}

/* Raw UDP send used internally (does NOT require an address; DHCP uses it
 * before the host has an IP).  src_ip is the IP to put in the header (0
 * during DHCP).  Broadcasts skip ARP. */
/* Resolve the next hop's MAC for `ip` (ARP request + wait for the reply,
 * driving the stack while we wait).  0 = resolved / broadcast / no route
 * needed; -1 = ARP timeout.  Shared by UDP and TCP so both can transmit
 * to a fresh peer. */
int net_arp_resolve(u32 ip) {
    u32 nh = next_hop(ip);
    if (nh == 0 || nh == 0xFFFFFFFFu) return 0;   /* no route / broadcast */
    if (arp_lookup(nh)) return 0;
    arp_send(1, g_ip, nh, BROADCAST);
    u64 t0 = pit_ticks();
    while (!arp_lookup(nh)) {
        net_pump();                      /* process the ARP reply */
        if (pit_ticks() - t0 > 50) return -1;      /* timeout */
        __asm__ volatile("pause");
    }
    return 0;
}

static int udp_raw_send(u32 src_ip, u32 dst_ip, u16 sport, u16 dport,
                        const u8 *buf, u16 len) {
    u32 nh = next_hop(dst_ip);
    if (nh == 0) return -1;              /* no route */
    if (nh != 0xFFFFFFFFu && net_arp_resolve(dst_ip) != 0)
        return -1;                       /* ARP timeout */
    u8 udp[8 + len]; memset(udp, 0, sizeof udp);
    udp[0] = sport >> 8; udp[1] = sport & 0xFF;
    udp[2] = dport >> 8; udp[3] = dport & 0xFF;
    u16 tl = (u16)(8 + len);
    udp[4] = tl >> 8; udp[5] = tl & 0xFF;
    udp[6] = 0; udp[7] = 0;              /* checksum 0 (allowed on IPv4) */
    memcpy(udp + 8, buf, len);
    return net_ip_send(src_ip, dst_ip, 17, udp, tl);
}

/* ---- public UDP socket-ish API ---- */
int net_udp_send(u32 dst_ip, u16 dport, const u8 *buf, u16 len) {
    if (!g_ip) return -1;                 /* no address yet */
    return udp_raw_send(g_ip, dst_ip, g_udp_src_port, dport, buf, len);
}

int net_udp_recv(u8 *buf, u16 cap) {
    if (!g_udp_rx_len) return 0;
    u16 n = g_udp_rx_len;
    if (cap < n) n = cap;
    memcpy(buf, g_udp_rx, n);
    g_udp_rx_len = 0;
    return n;
}

u32 net_own_ip(void) { return g_ip; }

void net_get_addrs(u32 *ip, u32 *gw, u32 *dns, u32 *mask) {
    if (ip)   *ip   = g_ip;
    if (gw)   *gw   = g_gw;
    if (dns)  *dns  = g_dns;
    if (mask) *mask = g_mask;
}

/* ---- DHCP ---- */
static void dhcp_build_msg(u8 *pkt, u8 msgtype, u32 req_ip, u32 server_id) {
    memset(pkt, 0, 300);
    pkt[0] = 1;                          /* BOOTREQUEST */
    pkt[1] = 1; pkt[2] = 6;
    u32 x = hton32(g_xid); memcpy(pkt + 4, &x, 4);
    pkt[10] = 0x80; pkt[11] = 0x00;      /* broadcast flag */
    memcpy(pkt + 28, g_mac, 6);
    u8 *o = pkt + 236;
    o[0] = 0x63; o[1] = 0x82; o[2] = 0x53; o[3] = 0x63;   /* magic */
    int p = 4;
    o[p++] = 53; o[p++] = 1; o[p++] = msgtype;
    if (msgtype == DHCP_REQUEST) {
        o[p++] = 50; o[p++] = 4; u32 r = hton32(req_ip); memcpy(o + p, &r, 4); p += 4;
        o[p++] = 54; o[p++] = 4; u32 s = hton32(server_id); memcpy(o + p, &s, 4); p += 4;
    }
    o[p++] = 55; o[p++] = 3;             /* param request list */
    o[p++] = 1;                          /* subnet mask */
    o[p++] = 3;                          /* router */
    o[p++] = 6;                          /* DNS */
    o[p++] = 255;                        /* end */
}

static void dhcp_send(u8 msgtype, u32 req_ip, u32 server_id) {
    u8 pkt[300];
    dhcp_build_msg(pkt, msgtype, req_ip, server_id);
    udp_raw_send(g_ip, 0xFFFFFFFFu, UDP_DHCP_CLIENT, UDP_DHCP_SERVER, pkt, 300);
    g_dhcp_last = pit_ticks();
}

/* Parse a DHCP reply (a UDP payload on port 68). */
static void dhcp_handle(const u8 *p, u16 len) {
    if (len < 240) return;
    u32 xid = ntoh32(*(const u32 *)(p + 4));
    if (xid != g_xid) return;
    /* options after 236 + 4 magic */
    const u8 *o = p + 240;
    u8 msgtype = 0; u32 mask = 0, router = 0, dns = 0, srv = 0;
    while (o < p + len && *o != 255) {
        u8 code = o[0]; u8 olen = o[1];
        if ((u32)(olen + 2) > (u32)(p + len - o)) break;
        if (code == 53 && olen >= 1) msgtype = o[2];
        if (code == 1 && olen >= 4)  memcpy(&mask, o + 2, 4);
        if (code == 3 && olen >= 4)  memcpy(&router, o + 2, 4);
        if (code == 6 && olen >= 4)  memcpy(&dns, o + 2, 4);
        if (code == 54 && olen >= 4) memcpy(&srv, o + 2, 4);
        o += 2 + olen;
    }
    u32 yiaddr; memcpy(&yiaddr, p + 16, 4); yiaddr = ntoh32(yiaddr);

    /* Once we have an address, ignore further OFFER/ACK from the queued burst. */
    if (g_dhcp_state == DHCP_DONE) return;

    if (msgtype == DHCP_OFFER && g_dhcp_state == DHCP_DISCOVER) {
        g_offer_ip = yiaddr;
        g_server_id = ntoh32(srv);
        g_dhcp_state = DHCP_REQUEST;
        dhcp_send(DHCP_REQUEST, g_offer_ip, g_server_id);
        kprintf("net: DHCP OFFER %u.%u.%u.%u -> REQUEST\n",
                (g_offer_ip >> 24) & 255, (g_offer_ip >> 16) & 255,
                (g_offer_ip >> 8) & 255, g_offer_ip & 255);
    } else if (msgtype == DHCP_ACK && yiaddr != 0) {
        g_ip = yiaddr;
        g_mask = ntoh32(mask);
        g_gw = ntoh32(router);
        g_dns = ntoh32(dns);
        g_dhcp_state = DHCP_DONE;
        kprintf("net: DHCP ACK - ip=%u.%u.%u.%u gw=%u.%u.%u.%u dns=%u.%u.%u.%u mask=%u.%u.%u.%u\n",
                (g_ip>>24)&255,(g_ip>>16)&255,(g_ip>>8)&255,g_ip&255,
                (g_gw>>24)&255,(g_gw>>16)&255,(g_gw>>8)&255,g_gw&255,
                (g_dns>>24)&255,(g_dns>>16)&255,(g_dns>>8)&255,g_dns&255,
                (g_mask>>24)&255,(g_mask>>16)&255,(g_mask>>8)&255,g_mask&255);
    }
}

/* ---- inbound ---- */
static void process_arp(const u8 *a, u16 len) {
    if (len < 28) return;
    u16 oper = (u16)((a[6] << 8) | a[7]);
    u32 spa = ntoh32(*(const u32 *)(a + 14));
    u32 tpa = ntoh32(*(const u32 *)(a + 24));
    arp_store(spa, a + 8);               /* sha */
    if (oper == 1 && tpa == g_ip) {      /* someone asks for our MAC */
        u8 mac[6]; memcpy(mac, a + 8, 6);   /* requester */
        arp_send(2, g_ip, spa, mac);
    }
}

static void process_icmp(const u8 *p, u16 len, u32 src) {
    if (len < 8) return;
    if (p[0] == 8) {                     /* echo request -> reply */
        u8 r[64]; memset(r, 0, sizeof r);
        r[0] = 0; r[1] = 0;              /* echo reply */
        memcpy(r + 4, p + 4, 8);         /* id/seq */
        u16 dlen = len > 4 ? len - 4 : 0;
        if (dlen > 56) dlen = 56;
        memcpy(r + 8, p + 8, dlen);
        u16 chk = net_csum(r, 8 + dlen);
        r[2] = chk >> 8; r[3] = chk & 0xFF;
        ip_send(src, 1, r, 8 + dlen);
    }
}

static void process_udp(const u8 *p, u16 len, u32 src) {
    if (len < 8) return;
    u16 dport = (u16)((p[2] << 8) | p[3]);
    if (dport == UDP_DHCP_CLIENT) { dhcp_handle(p + 8, len - 8); return; }
    if (dport == DNS_CLIENT_PORT) {      /* DNS response */
        u16 dlen = len - 8;
        if (dlen > DNS_RXBUF) dlen = DNS_RXBUF;
        memcpy(g_dns_rx, p + 8, dlen);
        g_dns_rx_len = dlen;
        return;
    }
    if (dport == g_udp_src_port) {       /* demo socket */
        u16 dlen = len - 8;
        if (dlen > UDP_RXBUF) dlen = UDP_RXBUF;
        memcpy(g_udp_rx, p + 8, dlen);
        g_udp_rx_len = dlen;
    }
}

static void process_ip(const u8 *ip, u16 len) {
    if (len < 20) return;
    u8 ihl = (ip[0] & 0x0F) * 4;
    u16 total = (u16)((ip[2] << 8) | ip[3]);
    u8 proto = ip[9];
    u32 src = ntoh32(*(const u32 *)(ip + 12));
    u32 dst = ntoh32(*(const u32 *)(ip + 16));
    if (dst != g_ip && dst != 0xFFFFFFFFu && g_ip != 0) return;  /* not for us */
    if (total < ihl || total > len) return;
    const u8 *payload = ip + ihl;
    u16 plen = total - ihl;
    if (proto == 1) process_icmp(payload, plen, src);
    else if (proto == 17) process_udp(payload, plen, src);
    else if (proto == 6) net_tcp_deliver(payload, plen, src);
}

static void process_eth(const u8 *f, u16 len) {
    if (len < 14) return;
    u16 type = (u16)((f[12] << 8) | f[13]);
    /* learn senders' MACs on multicast/broadcast too */
    const u8 *src = f + 6;
    (void)src;
    if (type == ETH_ARP) process_arp(f + 14, len - 14);
    else if (type == ETH_IPV4) process_ip(f + 14, len - 14);
}

/* ---- RX pump: drain the NIC + process inbound frames.  Split out of
 * net_service so ARP resolution can drive inbound processing WITHOUT the
 * DHCP/TCP-timer machinery (net_arp_resolve may be called from inside a
 * TCP path that already holds the TCP lock - recursion through tcp_poll
 * would self-deadlock).  Safe to call recursively: each call drains the
 * driver FIFO completely. ---- */
static void net_pump(void) {
    nic_poll();
    u8 frame[2048];
    int n;
    while ((n = nic_rx(frame, sizeof frame)) > 0)
        process_eth(frame, (u16)n);
}

/* ---- public entry: drain NIC + drive DHCP ---- */
void net_service(void) {
    net_pump();

    /* drive DHCP until we have an address */
    if (g_dhcp_state != DHCP_DONE) {
        if (g_dhcp_state == 0) {
            /* entropy for the xid */
            u64 tsc; __asm__ volatile("rdtsc" : "=A"(tsc) :: "memory");
            g_xid = (u32)(tsc ^ (pit_ticks() << 16) ^ (u32)(u64)g_mac);
            g_dhcp_state = DHCP_DISCOVER;
            dhcp_send(DHCP_DISCOVER, 0, 0);
            kprintf("net: DHCP DISCOVER (xid=0x%x)\n", g_xid);
        } else if (pit_ticks() - g_dhcp_last > 20) {
            if (g_dhcp_state == DHCP_DISCOVER) dhcp_send(DHCP_DISCOVER, 0, 0);
            else if (g_dhcp_state == DHCP_REQUEST)
                dhcp_send(DHCP_REQUEST, g_offer_ip, g_server_id);
        }
    }

    /* drive TCP retransmission / timeouts */
    tcp_poll();
}

void net_init(void) {
    nic_mac(g_mac);
    g_ip = g_mask = g_gw = g_dns = 0;
    g_dhcp_state = 0;
    g_udp_rx_len = 0;
    memset(g_arp, 0, sizeof g_arp);
    tcp_init();
    kprintf("net: stack up (e1000), starting DHCP\n");
}
