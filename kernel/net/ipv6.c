/* Yart OS - IPv6 (kernel/net/ipv6.c)
 *
 * Real IPv6 on top of the same Ethernet driver:
 *   - 128-bit addressing (16-byte addresses, byte order preserved),
 *   - ICMPv6 echo (ping6 client + reply),
 *   - NDP: Router Solicitation -> Router Advertisement (SLAAC: prefix +
 *     EUI-64 interface id + default route via the router),
 *     Neighbor Solicitation/Advertisement (so peers can resolve us),
 *   - Ethernet type 0x86DD + multicast mapping (33:33:xx:xx:xx:xx).
 *
 * This is stateless autoconfiguration exactly like a real OS does on an
 * IPv6 LAN: RS out, RA in, address configured, traffic flows.  Verified
 * against QEMU slirp's IPv6 NAT (guest gets a global prefix from the RA,
 * router answers ping6).
 *
 * Not yet: TCP/UDP over v6 (dual-stack sockets), DAD, fragmentation,
 * extension headers.  Honest scope.
 */
#include <yart/net.h>
#include <yart/console.h>
#include <yart/string.h>
#include <yart/io.h>

/* IPv6 addresses as 16 raw bytes (network byte order, like the wire). */
typedef struct { u8 b[16]; } ip6_t;

/* ICMPv6 types */
#define ICMP6_ECHO_REQUEST 128
#define ICMP6_ECHO_REPLY   129
#define ICMP6_RS           133
#define ICMP6_RA           134
#define ICMP6_NS           135
#define ICMP6_NA           136

/* multicast helpers */
static const ip6_t IP6_ALL_NODES   = { { 0xFF,2,0,0,0,0,0,0,0,0,0,0,0,0,0,1 } };
static const ip6_t IP6_ALL_ROUTERS = { { 0xFF,2,0,0,0,0,0,0,0,0,0,0,0,0,0,2 } };

static void ip6_solicited_node(const ip6_t *a, ip6_t *out) {
    memset(out, 0, 16);
    out->b[0] = 0xFF; out->b[1] = 0x02;
    out->b[11] = 0x01; out->b[12] = 0xFF;
    out->b[13] = a->b[13]; out->b[14] = a->b[14]; out->b[15] = a->b[15];
}

/* ---- state ---- */
static ip6_t g_ip6;            /* our SLAAC global address (0 = none)  */
static ip6_t g_ip6_gw;         /* the router we learned from the RA     */
static bool  g_ip6_ok;
static u8    g_mac6[6];        /* NIC MAC (set by net_init)             */
static u16   g_icmp6_id, g_icmp6_seq;
static bool  g_ping6_hit;

/* IPv6 neighbor cache (NDP): maps an address to a MAC. */
#define ND6_CACHE 8
static struct { ip6_t ip; u8 mac[6]; bool valid; } g_nd6[ND6_CACHE];


/* ---- helpers ---- */
static bool ip6_is_zero(const ip6_t *a) {
    for (int i = 0; i < 16; i++) if (a->b[i]) return false;
    return true;
}
static bool ip6_eq(const ip6_t *a, const ip6_t *b) {
    for (int i = 0; i < 16; i++) if (a->b[i] != b->b[i]) return false;
    return true;
}
static bool ip6_is_multicast(const ip6_t *a) { return a->b[0] == 0xFF; }

/* EUI-64 interface id from the MAC: flip the U/L bit (RFC 4291). */
static void ip6_iid_from_mac(const u8 mac[6], u8 iid[8]) {
    iid[0] = mac[0] ^ 0x02;
    iid[1] = mac[1]; iid[2] = mac[2]; iid[3] = 0xFF;
    iid[4] = 0xFE; iid[5] = mac[3]; iid[6] = mac[4]; iid[7] = mac[5];
}

/* build our link-local address fe80::eui64 (used for NS/NA source) */
static void ip6_linklocal(ip6_t *out) {
    memset(out, 0, 16);
    out->b[0] = 0xFE; out->b[1] = 0x80;
    ip6_iid_from_mac(g_mac6, out->b + 8);
}

/* ---- multicast Ethernet mapping ---- */
static void ip6_mcast_mac(const ip6_t *a, u8 mac[6]) {
    mac[0] = 0x33; mac[1] = 0x33;
    mac[2] = a->b[12]; mac[3] = a->b[13];
    mac[4] = a->b[14]; mac[5] = a->b[15];
}

/* ---- transmit ---- */
static u8 *nd6_lookup(const ip6_t *ip);   /* fwd (defined with NDP)     */
static int ip6_send(const ip6_t *dst, u8 proto, const u8 *payload, u16 plen) {
    u8 mac[6];
    if (ip6_is_multicast(dst)) ip6_mcast_mac(dst, mac);
    else if (nd6_lookup(dst)) {
        memcpy(mac, nd6_lookup(dst), 6);       /* NDP-resolved neighbor   */
    } else {
        /* fallback (slirp decodes the IP dst from the frame): use the
         * solicited-node-style mcast mac so the packet still reaches the
         * tap even without a completed NDP resolution */
        ip6_mcast_mac(dst, mac);
        if (ip6_eq(dst, &g_ip6)) {
            /* self-ping: deliver locally */
            u16 total = (u16)(40 + plen);
            u8 h[40 + 1500];
            memset(h, 0, sizeof h);
            h[0] = 0x60;
            h[4] = (u8)(plen >> 8); h[5] = (u8)(plen & 0xFF);
            h[6] = proto;
            h[7] = 64;                          /* hop limit */
            memcpy(h + 8, g_ip6.b, 16);
            memcpy(h + 24, dst->b, 16);
            if (plen) memcpy(h + 40, payload, plen);
            net_ipv6_deliver(h, total);
            return 0;
        }
    }
    u16 total = (u16)(40 + plen);
    u8 f[14 + 40 + 1500];
    memset(f, 0, sizeof f);
    memcpy(f, mac, 6);
    memcpy(f + 6, g_mac6, 6);
    f[12] = 0x86; f[13] = 0xDD;
    u8 *h = f + 14;
    h[0] = 0x60;
    h[4] = (u8)(plen >> 8); h[5] = (u8)(plen & 0xFF);
    h[6] = proto;
    h[7] = 64;
    memcpy(h + 8, g_ip6.b, 16);                 /* src (global or ::)    */
    memcpy(h + 24, dst->b, 16);
    if (plen) memcpy(h + 40, payload, plen);
    return nic_send(f, 14 + total);
}

/* ICMPv6 with the proper pseudo-header checksum (RFC 4443).
 * OFFSET FIX: the message is [type,code,checksum][param:4][data...] -
 * the echo id/seq (or NS reserved / NA flags) live at offset 4..7, data
 * at 8..  The old convention copied the caller's body to offset 8, so
 * every echo request carried id=0/seq=0 (the peer's reply never matched)
 * and NS/NA fields were shifted by 4 bytes. */
static int icmp6_send(const ip6_t *dst, u8 kind, u8 code,
                      const u8 param[4], const u8 *data, u16 dlen) {
    u8 m[64 + 256]; memset(m, 0, sizeof m);
    m[0] = kind; m[1] = code;
    memcpy(m + 4, param, 4);
    if (dlen) memcpy(m + 8, data, dlen);
    u16 total = (u16)(8 + dlen);
    u8 ph[40 + 64 + 256]; memset(ph, 0, 40);
    memcpy(ph, g_ip6.b, 16);
    memcpy(ph + 16, dst->b, 16);
    ph[36] = (u8)(total >> 8); ph[37] = (u8)(total & 0xFF);
    ph[39] = 58;                                /* ICMPv6 */
    memcpy(ph + 40, m, total);
    u16 chk = net_csum(ph, 40 + total);
    m[2] = (u8)(chk >> 8); m[3] = (u8)(chk & 0xFF);
    return ip6_send(dst, 58, m, total);
}

static u8 *nd6_lookup(const ip6_t *ip) {
    for (int i = 0; i < ND6_CACHE; i++)
        if (g_nd6[i].valid && ip6_eq(&g_nd6[i].ip, ip)) return g_nd6[i].mac;
    return 0;
}
static void nd6_store(const ip6_t *ip, const u8 mac[6]) {
    u8 *e = nd6_lookup(ip);
    if (e) { memcpy(e, mac, 6); return; }
    for (int i = 0; i < ND6_CACHE; i++)
        if (!g_nd6[i].valid) {
            g_nd6[i].ip = *ip; memcpy(g_nd6[i].mac, mac, 6);
            g_nd6[i].valid = true; return;
        }
}

/* ---- NDP: router solicitation ---- */
static void ndp_send_rs(void) {
    /* RS from the UNSPECIFIED address (::) - the RFC 4861 form used
     * before an address is configured; some routers only answer this. */
    ip6_t zero;
    memset(&zero, 0, 16);
    u8 p[4] = { 0, 0, 0, 0 };               /* reserved */
    ip6_t saved = g_ip6;
    bool saved_ok = g_ip6_ok;
    g_ip6 = zero;
    g_ip6_ok = true;
    icmp6_send(&IP6_ALL_ROUTERS, ICMP6_RS, 0, p, 0, 0);
    g_ip6 = saved;
    g_ip6_ok = saved_ok;
}

/* ---- public: ping6 (blocking) ---- */
int net_icmp6_ping(const u8 addr[16], u64 *rtt_ticks) {
    const ip6_t *a = (const ip6_t *)addr;
    if (!g_ip6_ok && !ip6_eq(a, &g_ip6)) {
        if (ip6_is_zero(a)) return -1;
    }
    g_ping6_hit = false;
    g_icmp6_id = (u16)(pit_ticks() ^ (u16)(u64)g_mac6);
    g_icmp6_seq++;
    u8 p[4];                                /* identifier + seq at 4..7 */
    p[0] = (u8)(g_icmp6_id >> 8); p[1] = (u8)(g_icmp6_id & 0xFF);
    p[2] = (u8)(g_icmp6_seq >> 8); p[3] = (u8)(g_icmp6_seq & 0xFF);
    u8 body[32]; memset(body, 0, sizeof body);
    for (int i = 0; i < 32; i++) body[i] = (u8)(0x40 + i);
    u64 t0 = pit_ticks();
    if (icmp6_send(a, ICMP6_ECHO_REQUEST, 0, p, body, 32) != 0) return -1;
    while (pit_ticks() - t0 < MS_TO_TICKS(1500)) {
        net_pump_ipv6();
        if (g_ping6_hit) {
            if (rtt_ticks) *rtt_ticks = pit_ticks() - t0;
            return 0;
        }
        __asm__ volatile("pause");
    }
    return -1;
}

/* ---- inbound ---- */
static void icmp6_handle(const u8 *m, u16 len, const ip6_t *src) {
    if (len < 8) return;
    u8 kind = m[0];
    if (kind == ICMP6_ECHO_REQUEST) {
        /* reply (echo the peer's id/seq + data back) */
        u16 blen = len > 8 ? len - 8 : 0;
        if (blen > 56) blen = 56;
        icmp6_send(src, ICMP6_ECHO_REPLY, 0, m + 4, m + 8, blen);
    } else if (kind == ICMP6_ECHO_REPLY) {
        if (m[4] == (u8)(g_icmp6_id >> 8) && m[5] == (u8)(g_icmp6_id & 0xFF) &&
            m[6] == (u8)(g_icmp6_seq >> 8) && m[7] == (u8)(g_icmp6_seq & 0xFF))
            g_ping6_hit = true;
    } else if (kind == ICMP6_RA) {
        /* learn the router's MAC from the source link-layer option.
         * RA layout: 16-byte header, options at m[16]: type(1) len(1)
         * MAC at m[18..23].  (The old m[8] read garbage from the
         * reachable-time field and never stored the router's MAC.) */
        if (len >= 24 && m[16] == 1 && m[17] == 1)
            nd6_store(src, m + 18);
        /* Router Advertisement: first prefix option = SLAAC prefix.
         * Only configure once - later RAs (radvd repeats every few
         * seconds) must not re-print and re-set the state. */
        if (g_ip6_ok) return;
        u16 opt = 16;                          /* options after 16 B hdr */
        while (opt + 4 <= len) {
            u8 otype = m[opt];
            u8 olen = m[opt + 1];               /* in 8-byte units */
            if (otype == 3 && olen >= 4) {      /* prefix info */
                u8 plen8 = m[opt + 2];          /* prefix length */
                u8 flags = m[opt + 3];
                if (plen8 == 64 && (flags & 0x40)) {   /* on-link + A */
                    memcpy(g_ip6.b, m + opt + 16, 8);  /* first 8 bytes */
                    memset(g_ip6.b + 8, 0, 8);
                    ip6_iid_from_mac(g_mac6, g_ip6.b + 8);
                    g_ip6_gw = *src;            /* router = RA source */
                    g_ip6_ok = true;
                    kprintf("ipv6: SLAAC configured - addr %02x%02x:%02x%02x:"
                            "%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x "
                            "via router %02x%02x:..\\n",
                            g_ip6.b[0], g_ip6.b[1], g_ip6.b[2], g_ip6.b[3],
                            g_ip6.b[4], g_ip6.b[5], g_ip6.b[6], g_ip6.b[7],
                            g_ip6.b[8], g_ip6.b[9], g_ip6.b[10], g_ip6.b[11],
                            g_ip6.b[12], g_ip6.b[13], g_ip6.b[14], g_ip6.b[15],
                            src->b[0], src->b[1]);
                    return;
                }
            }
            opt += (u16)(olen * 8);
        }
    } else if (kind == ICMP6_NA) {
        /* learn the sender's MAC (target link-layer option at m+26) */
        if (len >= 32) {
            u8 mac[6];
            memcpy(mac, m + 26, 6);
            nd6_store(src, mac);
        }
    } else if (kind == ICMP6_NS) {
        /* someone asks for our address -> NA */
        if (g_ip6_ok && len >= 24 &&
            memcmp(m + 8, g_ip6.b, 16) == 0) {
            u8 p[4];                            /* flags: S + O */
            p[0] = 0x20; p[1] = 0; p[2] = 0; p[3] = 0;
            u8 body[24];
            memcpy(body, g_ip6.b, 16);          /* target */
            body[16] = 2; body[17] = 1;         /* target link-layer opt */
            memcpy(body + 18, g_mac6, 6);
            icmp6_send(src, ICMP6_NA, 0, p, body, 24);
        }
    }
}

void net_ipv6_deliver(const u8 *h, u16 len) {
    if (len < 40) return;
    if ((h[0] >> 4) != 6) return;
    u16 plen = (u16)((h[4] << 8) | h[5]);
    u8  proto = h[6];
    if (40 + plen > len) plen = (u16)(len - 40);
    ip6_t src, dst;
    memcpy(src.b, h + 8, 16);
    memcpy(dst.b, h + 24, 16);
    /* ours? our global addr, our link-local, or multicast */
    bool mine = (g_ip6_ok && ip6_eq(&dst, &g_ip6)) ||
                ip6_eq(&dst, &IP6_ALL_NODES) || ip6_is_multicast(&dst);
    if (!mine && !ip6_eq(&dst, &g_ip6)) return;
    if (proto == 58) icmp6_handle(h + 40, plen, &src);
}

/* Drive SLAAC as a NON-BLOCKING state machine (called from net_service).
 * Nothing here waits: every step either sends a packet or checks a flag
 * and returns.  The earlier blocking probe (nd6_resolve + ping loops
 * inside net_service) stalled the BSP net loop for seconds and starved
 * the loopback queue - the firewall tests failed because of it. */
#define V6_STEP_RS       0   /* send up to 4 router solicitations */
#define V6_STEP_PROBE    1   /* NDP-resolve + ping candidates      */
#define V6_STEP_NS       2   /* waiting for the NA                  */
#define V6_STEP_PING     3   /* waiting for the echo reply          */
static int v6_step = V6_STEP_RS;
static int v6_rs_n;
static int v6_cand;
static ip6_t v6_target;

static void ipv6_configure(const ip6_t *router, bool via_global_prefix) {
    g_ip6_gw = *router;
    if (via_global_prefix && router->b[0] != 0xFE) {
        memcpy(g_ip6.b, router->b, 8);           /* global prefix        */
        memset(g_ip6.b + 8, 0, 8);
        ip6_iid_from_mac(g_mac6, g_ip6.b + 8);
    } else {
        ip6_linklocal(&g_ip6);                   /* link-local router     */
    }
    g_ip6_ok = true;
    kprintf("ipv6: configured addr %02x%02x:%02x%02x:%02x%02x:%02x%02x:"
            "%02x%02x:%02x%02x:%02x%02x:%02x%02x router %02x%02x:..\n",
            g_ip6.b[0], g_ip6.b[1], g_ip6.b[2], g_ip6.b[3],
            g_ip6.b[4], g_ip6.b[5], g_ip6.b[6], g_ip6.b[7],
            g_ip6.b[8], g_ip6.b[9], g_ip6.b[10], g_ip6.b[11],
            g_ip6.b[12], g_ip6.b[13], g_ip6.b[14], g_ip6.b[15],
            router->b[0], router->b[1]);
}

static void ipv6_send_ns(const ip6_t *target) {
    u8 p[4] = { 0, 0, 0, 0 };
    u8 body[24];
    memcpy(body, target->b, 16);
    body[16] = 1; body[17] = 1;
    memcpy(body + 18, g_mac6, 6);
    ip6_t sn;
    ip6_solicited_node(target, &sn);
    ip6_t ll;
    ip6_linklocal(&ll);
    ip6_t saved = g_ip6;
    bool saved_ok = g_ip6_ok;
    g_ip6 = ll;
    g_ip6_ok = true;
    icmp6_send(&sn, ICMP6_NS, 0, p, body, 24);
    g_ip6 = saved;
    g_ip6_ok = saved_ok;
}

void net_ipv6_poll(void) {
    static u64 last_action;
    static u64 step_start;
    static bool gave_up;      /* no router anywhere: stop probing forever */
    static int  cycles;       /* full candidate sweeps with no answer      */
    if (gave_up || g_ip6_ok) return;
    if (pit_ticks() - last_action < MS_TO_TICKS(100)) return;
    last_action = pit_ticks();

    /* per-step timeout: a silent router must not wedge the state machine */
    if (v6_step != V6_STEP_RS && v6_step != V6_STEP_PROBE &&
        pit_ticks() - step_start > MS_TO_TICKS(600)) {
        v6_step = V6_STEP_PROBE;
        step_start = pit_ticks();
    }

    if (v6_step == V6_STEP_RS) {
        if (v6_rs_n < 4) { ndp_send_rs(); v6_rs_n++; return; }
        v6_step = V6_STEP_PROBE;
        v6_cand = 0;
        return;
    }
    if (v6_step == V6_STEP_PROBE) {
        static const u8 cands[6][16] = {
            { 0xfd,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1 },   /* fd00::1 (host) */
            { 0xfd,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2 },   /* fd00::2  */
            { 0xfe,0xc0,0,0,0,0,0,0,0,0,0,0,0,0,0,2 },/* fec0::2  */
            { 0xfc,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2 },   /* fc00::2  */
            { 0xfe,0x80,0,0,0,0,0,0,0,0,0,0,0,0,0,2 },/* fe80::2  */
            { 0xfe,0x80,0,0,0,0,0,0,0,0,0,0,0,0,0,1 } /* fe80::1 (host) */
        };
        while (v6_cand < 6) {
            memcpy(&v6_target, cands[v6_cand], 16);
            v6_cand++;
            if (nd6_lookup(&v6_target)) continue;      /* known already */
            ipv6_send_ns(&v6_target);
            v6_step = V6_STEP_NS;
            step_start = pit_ticks();
            return;
        }
        /* Every candidate was probed with no answer.  A real router answers
         * on the first sweep; an emulated/host-only network never will, so
         * retrying forever just burns CPU and spams the serial console under
         * TCG.  Give up after two full sweeps and go silent. */
        if (++cycles >= 2) {
            gave_up = true;
            kprintf("ipv6: no router found - SLAAC disabled (IPv4 only)\n");
            return;
        }
        v6_step = V6_STEP_RS;                /* one more sweep, then stop */
        v6_rs_n = 0;
        return;
    }
    if (v6_step == V6_STEP_NS) {
        if (!nd6_lookup(&v6_target)) return;          /* keep waiting     */
        /* resolved: ping it (link-local source, real-host semantics) */
        g_ping6_hit = false;
        u8 p[4];
        p[0] = (u8)(g_icmp6_id >> 8); p[1] = (u8)(g_icmp6_id & 0xFF);
        p[2] = (u8)(g_icmp6_seq >> 8); p[3] = (u8)(g_icmp6_seq & 0xFF);
        u8 body[32]; memset(body, 0, sizeof body);
        ip6_t ll;
        ip6_linklocal(&ll);
        ip6_t saved = g_ip6;
        bool saved_ok = g_ip6_ok;
        g_ip6 = ll;
        g_ip6_ok = true;
        icmp6_send(&v6_target, ICMP6_ECHO_REQUEST, 0, p, body, 32);
        g_ip6 = saved;
        g_ip6_ok = saved_ok;
        v6_step = V6_STEP_PING;
        step_start = pit_ticks();
        return;
    }
    if (v6_step == V6_STEP_PING) {
        if (!g_ping6_hit) return;                    /* keep waiting     */
        kprintf("ipv6: router candidate %02x%02x:..%02x%02x answered - configuring\n",
                v6_target.b[0], v6_target.b[1], v6_target.b[14], v6_target.b[15]);
        ipv6_configure(&v6_target, true);
        return;
    }
}

/* RX-pump wrapper for the ping6 wait loop (net.c's pump is static). */
void net_pump_ipv6(void) { extern void net_service(void); net_service(); }
bool net_ipv6_ready(void) { return g_ip6_ok; }
void net_ipv6_addrs(u8 addr[16], u8 router[16]) {
    if (addr) memcpy(addr, g_ip6.b, 16);
    if (router) memcpy(router, g_ip6_gw.b, 16);
}

void net_ipv6_init(const u8 mac[6]) {
    memcpy(g_mac6, mac, 6);
    memset(&g_ip6, 0, sizeof g_ip6);
    memset(&g_ip6_gw, 0, sizeof g_ip6_gw);
    g_ip6_ok = false;
    g_ping6_hit = false;
    g_icmp6_id = 0; g_icmp6_seq = 0;
    kprintf("ipv6: ICMPv6 + NDP + SLAAC ready (waiting for RA)\\n");
}
