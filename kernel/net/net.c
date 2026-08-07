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
static void process_ip(const u8 *ip, u16 len);   /* loopback self-delivery */

/* ---- state ---- */
static u8  g_mac[6];
static u32 g_ip, g_mask, g_gw, g_dns;     /* host order */
static u16 g_udp_src_port = 7777;

/* ---- routing table (longest-prefix-match) ----
 * Real routes instead of the old hardcoded "local subnet or gateway"
 * heuristic: 127.0.0.0/8 -> local (loopback) is always present; DHCP
 * installs the link route and the default-via-gateway route. */
#define RT_MAX 8
#define RT_LOCAL   1
#define RT_LINK    2
#define RT_DEFAULT 4
typedef struct { u32 dst, mask, gw; u8 flags; } route_t;
static route_t g_routes[RT_MAX];
static int  g_route_count;

static void route_add(u32 dst, u32 mask, u32 gw, u8 flags) {
    for (int i = 0; i < g_route_count; i++) {
        route_t *r = &g_routes[i];
        if (r->dst == dst && r->mask == mask) { r->gw = gw; r->flags = flags; return; }
    }
    if (g_route_count < RT_MAX) {
        g_routes[g_route_count].dst = dst;
        g_routes[g_route_count].mask = mask;
        g_routes[g_route_count].gw = gw;
        g_routes[g_route_count].flags = flags;
        g_route_count++;
    }
}

static u32 route_next_hop(u32 dst) {
    if (dst == 0xFFFFFFFFu) return 0xFFFFFFFFu;        /* broadcast */
    if ((dst & 0xFF000000u) == 0x7F000000u) return dst; /* loopback  */
    int best = -1; u32 best_mask = 0;
    for (int i = 0; i < g_route_count; i++) {
        route_t *r = &g_routes[i];
        if ((dst & r->mask) == (r->dst & r->mask) && r->mask >= best_mask) {
            best = i; best_mask = r->mask;
        }
    }
    if (best < 0) return 0;
    route_t *r = &g_routes[best];
    if (r->flags & RT_DEFAULT) return r->gw ? r->gw : 0;
    return dst;                          /* local / link: direct delivery */
}

static unsigned route_bits(u32 mask) {   /* popcount without libgcc */
    unsigned n = 0;
    while (mask) { n += mask & 1; mask >>= 1; }
    return n;
}

static void route_dump(void) {
    kprintf("net: routes:\n");
    for (int i = 0; i < g_route_count; i++) {
        route_t *r = &g_routes[i];
        kprintf("net:   %u.%u.%u.%u/%u %s\n",
                (r->dst >> 24) & 255, (r->dst >> 16) & 255, (r->dst >> 8) & 255, r->dst & 255,
                route_bits(r->mask),
                (r->flags & RT_LOCAL) ? "lo" :
                (r->flags & RT_DEFAULT) ? "via gw" : "link");
    }
}

/* ---- packet firewall (rule list; default ACCEPT) ----
 * Rules: {proto: 0=any|1=icmp|6=tcp|17=udp, dip: 0=any, dport: 0=any,
 * drop: true=DROP}.  Checked outbound (net_ip_send) and inbound
 * (process_ip) so both directions can be policed. */
#define FW_MAX_RULES 16
typedef struct { bool used; u8 proto; u32 dip; u16 dport; bool drop; } fw_rule_t;
static fw_rule_t g_fw[FW_MAX_RULES];
static u64 g_fw_dropped;

static bool net_fw_check(u8 proto, u32 dip, u16 dport) {
    for (int i = 0; i < FW_MAX_RULES; i++) {
        fw_rule_t *r = &g_fw[i];
        if (!r->used) continue;
        if (r->proto && r->proto != proto) continue;
        if (r->dip && r->dip != dip) continue;
        if (r->dport && r->dport != dport) continue;
        return r->drop;
    }
    return false;
}

int net_fw_add(u8 proto, u32 dip, u16 dport, bool drop) {
    for (int i = 0; i < FW_MAX_RULES; i++) {
        if (!g_fw[i].used) {
            g_fw[i].used = true;
            g_fw[i].proto = proto;
            g_fw[i].dip = dip;
            g_fw[i].dport = dport;
            g_fw[i].drop = drop;
            return 0;
        }
    }
    return -1;
}
int net_fw_clear(void) {
    memset(g_fw, 0, sizeof g_fw);
    return 0;
}
u64 net_fw_dropped_count(void) { return g_fw_dropped; }

/* ---- loopback "wire": a ring buffer drained by the RX pump ----
 * Delivering loopback packets SYNCHRONOUSLY inside net_ip_send re-enters
 * the TCP receive path while the caller may hold g_tcp_lock (the connect
 * path sends its SYN under the lock) - a self-deadlock.  Real NICs make
 * the transmit asynchronous; so does this: the packet goes onto a small
 * ring and the RX pump (net_pump, never called with the TCP lock held)
 * delivers it back to process_ip. */
#define LOOPBACK_QUEUE 8
static u8  g_lo_q[LOOPBACK_QUEUE][1528];
static u16 g_lo_len[LOOPBACK_QUEUE];
static int g_lo_head, g_lo_tail, g_lo_count;

/* ---- ICMP ping client state ---- */
static u32  g_ping_id;
static u16  g_ping_seq;
static bool g_ping_hit;
static u32  g_ping_reply_src;
static u16  g_ping_reply_id, g_ping_reply_seq;

/* ---- ARP cache (small, with TTL) ---- */
#define ARP_CACHE 8
#define ARP_TTL 300                       /* 3 s at 100 Hz */
static struct { u32 ip; u8 mac[6]; bool valid; u64 time; } g_arp[ARP_CACHE];

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
    u64 now = pit_ticks();
    for (int i = 0; i < ARP_CACHE; i++)
        if (g_arp[i].valid && g_arp[i].ip == ip && now - g_arp[i].time < ARP_TTL)
            return g_arp[i].mac;
    return 0;
}
static void arp_store(u32 ip, const u8 mac[6]) {
    u8 *e = arp_lookup(ip);
    if (e) { memcpy(e, mac, 6); return; }
    for (int i = 0; i < ARP_CACHE; i++)
        if (!g_arp[i].valid) {
            g_arp[i].ip = ip; memcpy(g_arp[i].mac, mac, 6);
            g_arp[i].valid = true; g_arp[i].time = pit_ticks(); return;
        }
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
/* Next-hop selection via the routing table (longest-prefix match). */
static u32 next_hop(u32 dst) { return route_next_hop(dst); }

int net_ip_send(u32 src, u32 dst, u8 proto, const u8 *payload, u16 plen) {
    /* outbound firewall check (dest port: tcp/udp headers are
     * [sport][dport]; icmp has no port) */
    if (plen >= 4 && (proto == 6 || proto == 17)) {
        u16 dp = (u16)((payload[2] << 8) | payload[3]);
        if (net_fw_check(proto, dst, dp)) { g_fw_dropped++; return -2; }
    }
    /* LOOPBACK: 127.0.0.0/8 is delivered locally - no Ethernet, no ARP.
     * The source is forced to 127.0.0.1 (RFC 1122). */
    if ((dst & 0xFF000000u) == 0x7F000000u) {
        u16 total = (u16)(20 + plen);
        static u8 ip[1528]; memset(ip, 0, sizeof ip);   /* kstack is 16K */
        ip[0] = 0x45;
        ip[2] = (u8)(total >> 8); ip[3] = (u8)(total & 0xFF);
        ip[8] = 64;                            /* TTL */
        ip[9] = proto;
        u32 s = hton32(0x7F000001); memcpy(ip + 12, &s, 4);
        u32 d = hton32(dst);       memcpy(ip + 16, &d, 4);
        u16 chk = net_csum(ip, 20);
        ip[10] = (u8)(chk >> 8); ip[11] = (u8)(chk & 0xFF);
        if (plen) memcpy(ip + 20, payload, plen);
        if (g_lo_count >= LOOPBACK_QUEUE) return -1;   /* drop (busy) */
        memcpy(g_lo_q[g_lo_tail], ip, total);
        g_lo_len[g_lo_tail] = total;
        g_lo_tail = (g_lo_tail + 1) % LOOPBACK_QUEUE;
        g_lo_count++;
        return 0;
    }
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
    static u8 f[1518]; memset(f, 0, sizeof f);          /* kstack is 16K */
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
    if ((ip & 0xFF000000u) == 0x7F000000u) return 0;  /* loopback: no ARP */
    u32 nh = next_hop(ip);
    if (nh == 0 || nh == 0xFFFFFFFFu) return 0;   /* no route / broadcast */
    if (arp_lookup(nh)) return 0;
    arp_send(1, g_ip, nh, BROADCAST);
    u64 t0 = pit_ticks();
    while (!arp_lookup(nh)) {
        net_pump();                      /* process the ARP reply */
        /* 1.5 s timeout: 0.5 s was too tight under emulation and after
         * the 3 s ARP-TTL expiry (the reply was still in flight). */
        if (pit_ticks() - t0 > 150) return -1;
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
    /* loopback needs no address (real-OS semantics: UDP to 127.0.0.1
     * works with zero network config) */
    if (!g_ip && (dst_ip & 0xFF000000u) != 0x7F000000u) return -1;
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

/* Ping (ICMP echo request) with a wait-for-reply loop; returns 0 on
 * reply with the RTT in ticks.  Works for any target: the gateway, a
 * host, or 127.0.0.1 (loopback). */
int net_icmp_ping(u32 ip, u64 *rtt_ticks) {
    if (!g_ip && (ip & 0xFF000000u) != 0x7F000000u) return -1;
    u8 r[40]; memset(r, 0, sizeof r);
    g_ping_id = (u16)(pit_ticks() ^ (ip >> 16) ^ (u16)(u64)g_mac);
    g_ping_seq++;
    g_ping_hit = false;
    r[0] = 8;                            /* echo request */
    r[4] = (u8)(g_ping_id >> 8); r[5] = (u8)(g_ping_id & 0xFF);
    r[6] = (u8)(g_ping_seq >> 8); r[7] = (u8)(g_ping_seq & 0xFF);
    for (int i = 8; i < 40; i++) r[i] = (u8)(i + 0x41);   /* payload */
    u16 chk = net_csum(r, 40);
    r[2] = (u8)(chk >> 8); r[3] = (u8)(chk & 0xFF);
    u64 t0 = pit_ticks();
    if (net_ip_send(g_ip, ip, 1, r, 40) != 0) return -1;
    while (pit_ticks() - t0 < 150) {
        net_pump();
        if (g_ping_hit && g_ping_reply_src == ip &&
            g_ping_reply_id == g_ping_id && g_ping_reply_seq == g_ping_seq) {
            if (rtt_ticks) *rtt_ticks = pit_ticks() - t0;
            return 0;
        }
        __asm__ volatile("pause");
    }
    return -1;
}

/* Choose the local port for the UDP socket (default 7777). */
int net_udp_bind(u16 port) {
    g_udp_src_port = port;
    return 0;
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
        /* install routes from the lease: link route + default via gw */
        if (g_mask) route_add(g_ip & g_mask, g_mask, 0, RT_LINK);
        if (g_gw)   route_add(0, 0, g_gw, RT_DEFAULT);
        kprintf("net: DHCP ACK - ip=%u.%u.%u.%u gw=%u.%u.%u.%u dns=%u.%u.%u.%u mask=%u.%u.%u.%u\n",
                (g_ip>>24)&255,(g_ip>>16)&255,(g_ip>>8)&255,g_ip&255,
                (g_gw>>24)&255,(g_gw>>16)&255,(g_gw>>8)&255,g_gw&255,
                (g_dns>>24)&255,(g_dns>>16)&255,(g_dns>>8)&255,g_dns&255,
                (g_mask>>24)&255,(g_mask>>16)&255,(g_mask>>8)&255,g_mask&255);
        route_dump();
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
    } else if (p[0] == 0) {              /* echo reply: hand to the ping client */
        g_ping_hit = true;
        g_ping_reply_src = src;
        g_ping_reply_id = (u16)((p[4] << 8) | p[5]);
        g_ping_reply_seq = (u16)((p[6] << 8) | p[7]);
    }
}

/* ICMP destination-unreachable (type 3): quote the offending IP header +
 * first 8 bytes of its payload - standard "real OS" behavior when a UDP
 * datagram arrives on a closed port. */
static void icmp_unreachable(u32 src, const u8 *iphdr, u16 iplen,
                             const u8 *seg, u16 seglen, u8 code) {
    u8 m[64]; memset(m, 0, sizeof m);
    m[0] = 3; m[1] = code;
    u16 qh = iplen > 20 ? 20 : iplen;
    if (qh > 20) qh = 20;
    memcpy(m + 8, iphdr, qh);
    if (seglen > 8) seglen = 8;
    memcpy(m + 8 + qh, seg, seglen);
    u16 tl = (u16)(8 + qh + seglen);
    u16 chk = net_csum(m, tl);
    m[2] = (u8)(chk >> 8); m[3] = (u8)(chk & 0xFF);
    ip_send(src, 1, m, tl);
}

static void process_udp(const u8 *p, u16 len, u32 src,
                        const u8 *iphdr, u16 iplen) {
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
        return;
    }
    /* closed port: tell the sender (like a real OS) */
    icmp_unreachable(src, iphdr, iplen, p, len, 3);   /* code 3 = port */
}

static void process_ip(const u8 *ip, u16 len) {
    if (len < 20) return;
    u8 ihl = (ip[0] & 0x0F) * 4;
    u16 total = (u16)((ip[2] << 8) | ip[3]);
    u8 proto = ip[9];
    u32 src = ntoh32(*(const u32 *)(ip + 12));
    u32 dst = ntoh32(*(const u32 *)(ip + 16));
    /* ours: our unicast, 127.0.0.1 (loopback), or broadcast */
    if (dst != g_ip && dst != 0xFFFFFFFFu && dst != 0x7F000001u && g_ip != 0)
        return;
    if (total < ihl || total > len) return;
    const u8 *payload = ip + ihl;
    u16 plen = total - ihl;
    /* inbound firewall check (dest port = our local port for tcp/udp) */
    if (plen >= 4 && (proto == 6 || proto == 17)) {
        u16 dp = (u16)((payload[2] << 8) | payload[3]);
        if (net_fw_check(proto, dst, dp)) { g_fw_dropped++; return; }
    }
    if (proto == 1) process_icmp(payload, plen, src);
    else if (proto == 17) process_udp(payload, plen, src, ip, total);
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
    else if (type == ETH_IPV6) net_ipv6_deliver(f + 14, len - 14);
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
    /* deliver queued loopback packets (never called with the TCP lock).
     * COPY-OUT DRAIN: the TCP handler running inside process_ip() queues
     * new packets (ACKs, SYN|ACKs, FINs) into this same ring.  Draining
     * in place let a re-entrant write land on the slot currently being
     * parsed when tail wrapped onto head - the segment's ports/flags got
     * clobbered mid-parse, the connection lookup failed (data silently
     * lost), and corrupted segments were mistaken for new SYNs (endless
     * SYN|ACK + retransmit storm).  Copy the packet out and free the
     * slot BEFORE processing so re-entrant writes can never collide. */
    while (g_lo_count > 0) {
        u8 tmp[1528];
        u16 plen = g_lo_len[g_lo_head];
        if (plen > sizeof tmp) plen = sizeof tmp;
        memcpy(tmp, g_lo_q[g_lo_head], plen);
        g_lo_head = (g_lo_head + 1) % LOOPBACK_QUEUE;
        g_lo_count--;
        process_ip(tmp, plen);
    }
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
    /* drive IPv6 SLAAC (RS until RA) */
    net_ipv6_poll();
}

void net_init(void) {
    nic_mac(g_mac);
    g_ip = g_mask = g_gw = g_dns = 0;
    g_dhcp_state = 0;
    g_udp_rx_len = 0;
    memset(g_arp, 0, sizeof g_arp);
    g_route_count = 0;
    memset(g_fw, 0, sizeof g_fw);
    g_fw_dropped = 0;
    g_lo_head = g_lo_tail = g_lo_count = 0;
    route_add(0x7F000000, 0xFF000000, 0, RT_LOCAL);   /* 127.0.0.0/8 lo */
    net_ipv6_init(g_mac);
    tcp_init();
    tls_init();
    kprintf("net: stack up (e1000), starting DHCP\n");
}
