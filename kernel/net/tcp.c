/* Yart OS - TCP (kernel/net/tcp.c)
 *
 * A minimal but REAL TCP implementation on top of the existing
 * Ethernet/ARP/IPv4/UDP/DHCP stack: full 3-way handshake, sequence/ack
 * tracking, receive buffering, go-back-N retransmission with timeout,
 * graceful close (FIN/ACK), and a listen/accept server path.  Verified
 * against real peers: the boot test connects to a host echo server
 * (10.0.2.2 via QEMU slirp) and the kernel's own HTTP server answers a
 * real curl from the host.
 *
 * Design notes:
 *  - one transmit buffer per connection (2048 B); ACKs slide snd_una;
 *    the whole unacked buffer is retransmitted on a single timer.
 *  - one receive buffer per connection (4096 B); the advertised window
 *    is fixed to the buffer size; out-of-order segments are dropped and
 *    the peer retransmits - correct, if not fancy.
 *  - driven by net_service()/tcp_poll() (polling, like the rest of the
 *    stack) plus synchronous syscall wrappers.
 */
#include <yart/net.h>
#include <yart/console.h>
#include <yart/string.h>
#include <yart/io.h>
#include <yart/spinlock.h>

#define TCP_MAX_CONN    8
#define TCP_SNDBUF      2048
#define TCP_RXBUF       4096
#define TCP_MAX_DATA    1400          /* fits one Ethernet frame          */

#define TCP_RETRANS_TICKS 25          /* 250 ms @ 100 Hz                  */
#define TCP_MAX_RETRIES   8
#define TCP_CONNECT_TIMEOUT 800       /* 8 s                              */
#define TCP_CLOSE_TIMEOUT   250       /* 2.5 s                            */

/* TCP flags */
#define TF_FIN 0x01
#define TF_SYN 0x02
#define TF_RST 0x04
#define TF_PSH 0x08
#define TF_ACK 0x10

typedef enum {
    TCP_CLOSED = 0, TCP_LISTEN, TCP_SYN_SENT, TCP_ESTABLISHED,
    TCP_FIN_WAIT1, TCP_FIN_WAIT2, TCP_LAST_ACK, TCP_CLOSING
} tcp_state_t;

typedef struct {
    bool used;
    tcp_state_t st;
    u16 lport, rport;
    u32 rip;                        /* remote ip, host order             */
    u32 snd_una;                    /* first unacknowledged sequence      */
    u32 rcv_nxt;                    /* next expected inbound sequence     */
    u32 iss;                        /* initial send sequence              */
    u32 fin_seq;                    /* sequence of our FIN (if sent)      */
    u8  snd[TCP_SNDBUF];            /* unacked transmit data              */
    u16 snd_len;
    u8  rx[TCP_RXBUF];              /* received data not yet read         */
    u16 rx_len;
    u64 last_send;                  /* pit_ticks of last TX               */
    int retries;
} tcp_conn_t;

static tcp_conn_t g_tcp[TCP_MAX_CONN];
static spinlock_t g_tcp_lock;
static u16 g_eph_port = 30000;

extern u16 net_csum(const u8 *data, int len);
extern int  net_ip_send(u32 src, u32 dst, u8 proto, const u8 *payload, u16 plen);
extern u32  net_own_ip(void);
extern int  net_arp_resolve(u32 ip);

static u32 tcp_seq(void) {
    u64 t;
    __asm__ volatile("rdtsc" : "=A"(t) :: "memory");
    return (u32)(t ^ (pit_ticks() << 20) ^ 0x9E3779B9u);
}

/* Build + send one TCP segment.  Takes the lock-free path (caller holds
 * the lock or is single-threaded in the RX path). */
static bool tcp_is_loopback(const tcp_conn_t *c) {
    return (c->rip & 0xFF000000u) == 0x7F000000u;
}

static int tcp_send_seg(tcp_conn_t *c, u8 flags, u32 seq, u32 ack,
                        const u8 *data, u16 dlen) {
    if (!net_own_ip() && !tcp_is_loopback(c)) return -1;
    u8 seg[TCP_MAX_DATA + 40];
    u16 tlen = (u16)(20 + dlen);
    seg[0] = (u8)(c->lport >> 8);  seg[1] = (u8)(c->lport & 0xFF);
    seg[2] = (u8)(c->rport >> 8);  seg[3] = (u8)(c->rport & 0xFF);
    u32 s = hton32(seq); memcpy(seg + 4, &s, 4);
    u32 a = hton32(ack); memcpy(seg + 8, &a, 4);
    seg[12] = 0x50;                                /* data offset 5 */
    seg[13] = flags;
    seg[14] = (u8)(TCP_RXBUF >> 8);                /* window 4096    */
    seg[15] = (u8)(TCP_RXBUF & 0xFF);
    seg[16] = 0; seg[17] = 0;                      /* checksum below */
    seg[18] = 0; seg[19] = 0;                      /* urgent ptr     */
    if (dlen) memcpy(seg + 20, data, dlen);
    /* checksum over the pseudo header + segment */
    u8 ph[12 + TCP_MAX_DATA + 40];
    memset(ph, 0, 12);
    u32 sip = hton32(tcp_is_loopback(c) ? 0x7F000001u : net_own_ip());
    u32 dip = hton32(c->rip);
    memcpy(ph, &sip, 4);
    memcpy(ph + 4, &dip, 4);
    ph[9] = 6;
    u16 tl = hton16(tlen);
    memcpy(ph + 10, &tl, 2);
    memcpy(ph + 12, seg, tlen);
    u16 chk = net_csum(ph, 12 + tlen);
    seg[16] = (u8)(chk >> 8);
    seg[17] = (u8)(chk & 0xFF);
    /* LEAF TRANSMIT: the caller must have resolved the peer's MAC first
     * (net_arp_resolve drives inbound processing, which must NEVER run
     * while we hold g_tcp_lock - net_service/tcp_poll would self-deadlock
     * on the non-reentrant lock).  -1 here = no ARP entry yet; callers
     * treat it as "try again on the next retransmit". */
    return net_ip_send(tcp_is_loopback(c) ? 0x7F000001u : net_own_ip(),
                       c->rip, 6, seg, tlen);
}

/* Per-connection retransmission / timeout logic (caller holds lock). */
static void tcp_poll_conn(tcp_conn_t *c) {
    u64 now = pit_ticks();
    if (c->st != TCP_SYN_SENT && c->st != TCP_FIN_WAIT1 &&
        c->st != TCP_LAST_ACK && c->snd_len == 0)
        return;
    if (now - c->last_send < TCP_RETRANS_TICKS) return;
    c->last_send = now;
    if (++c->retries > TCP_MAX_RETRIES) {
        kprintf("tcp: conn on :%u giving up after %d retries - closing\n",
                (unsigned)c->lport, TCP_MAX_RETRIES);
        tcp_send_seg(c, TF_RST | TF_ACK, c->snd_una + c->snd_len, c->rcv_nxt,
                     NULL, 0);
        memset(c, 0, sizeof *c);
        return;
    }
    if (c->st == TCP_SYN_SENT) {
        tcp_send_seg(c, TF_SYN, c->iss, 0, NULL, 0);
    } else if (c->snd_len > 0) {
        tcp_send_seg(c, TF_ACK | TF_PSH, c->snd_una, c->rcv_nxt,
                     c->snd, c->snd_len);
    } else {                                       /* FIN retransmit */
        tcp_send_seg(c, TF_FIN | TF_ACK, c->fin_seq, c->rcv_nxt, NULL, 0);
    }
}

void tcp_poll(void) {
    u64 fl = irq_save();
    spin_lock(&g_tcp_lock);
    for (int i = 0; i < TCP_MAX_CONN; i++)
        if (g_tcp[i].used && g_tcp[i].st != TCP_LISTEN)
            tcp_poll_conn(&g_tcp[i]);
    spin_unlock(&g_tcp_lock);
    irq_restore(fl);
}

/* ---- inbound ---- */

void net_tcp_deliver(const u8 *seg, u16 len, u32 src) {
    if (len < 20) return;
    u64 fl = irq_save();
    spin_lock(&g_tcp_lock);
    /* PORT-ORDER FIX: the TCP header is [src_port][dst_port].  Reading
     * the destination from bytes 0-1 made every inbound segment look like
     * it targeted the SENDER's port - slirp's SYN for our :8080 arrived
     * as 'dport=47156' (its ephemeral port), matched no listener, and got
     * an RST.  src = bytes 0-1, dst = bytes 2-3. */
    u16 sport = (u16)((seg[0] << 8) | seg[1]);
    u16 dport = (u16)((seg[2] << 8) | seg[3]);
    u32 seq = ntoh32(*(const u32 *)(seg + 4));
    u32 ack = ntoh32(*(const u32 *)(seg + 8));
    u8  flags = seg[13];
    u16 hlen = (u16)((seg[12] >> 4) * 4);
    if (hlen < 20 || hlen > len) { spin_unlock(&g_tcp_lock); irq_restore(fl); return; }
    const u8 *data = seg + hlen;
    u16 dlen = len - hlen;

    /* SHADOWING FIX: an established child and its listener share lport,
     * and the listener sits in a lower slot - a single pass always found
     * the LISTENER and silently dropped the child's data/FIN segments.
     * Look for an exact (rport+rip) child first, then a listener. */
    tcp_conn_t *c = NULL;
    for (int i = 0; i < TCP_MAX_CONN; i++) {
        tcp_conn_t *o = &g_tcp[i];
        if (!o->used || o->lport != dport || o->st == TCP_LISTEN) continue;
        if (o->rport == sport && o->rip == src) { c = o; break; }
    }
    if (!c) {
        for (int i = 0; i < TCP_MAX_CONN; i++) {
            tcp_conn_t *o = &g_tcp[i];
            if (o->used && o->lport == dport && o->st == TCP_LISTEN) { c = o; break; }
        }
    }

    /* No connection: answer RST so the peer fails fast (skip if it IS a
     * RST, and skip for a bare SYN on a dead listener). */
    if (!c) {
        if (!(flags & TF_RST)) {
            tcp_conn_t tmp;
            memset(&tmp, 0, sizeof tmp);
            tmp.lport = dport; tmp.rport = sport; tmp.rip = src;
            u32 rseq = (flags & TF_ACK) ? ack : 0;
            u32 rack = seq + dlen + ((flags & TF_SYN) ? 1 : 0);
            tcp_send_seg(&tmp, TF_RST | TF_ACK, rseq, rack, NULL, 0);
        }
        spin_unlock(&g_tcp_lock); irq_restore(fl);
        return;
    }

    if (c->st == TCP_LISTEN) {
        if ((flags & TF_SYN) && !(flags & TF_RST)) {
            /* find a child slot for the new connection */
            tcp_conn_t *ch = NULL;
            for (int i = 0; i < TCP_MAX_CONN; i++)
                if (!g_tcp[i].used) { ch = &g_tcp[i]; break; }
            if (ch) {
                memset(ch, 0, sizeof *ch);
                ch->used = true;
                ch->st = TCP_ESTABLISHED;      /* SYN|ACK sent below       */
                ch->lport = dport;
                ch->rport = sport;
                ch->rip = src;
                ch->rcv_nxt = seq + 1;         /* SYN consumes a seq       */
                ch->iss = tcp_seq();
                ch->snd_una = ch->iss + 1;   /* first data byte = iss+1   */
                /* peer MAC may be uncached - resolve OUTSIDE the lock
                 * (net_arp_resolve drives inbound RX; net_pump recursion
                 * into deliver is fine because we release the lock first;
                 * a nested SYN may re-use this slot, hence the recheck) */
                spin_unlock(&g_tcp_lock);
                int aok = (net_arp_resolve(src) == 0);
                spin_lock(&g_tcp_lock);
                if (ch->used && ch->st == TCP_ESTABLISHED) {
                    if (aok) {
                        ch->last_send = pit_ticks();
                        tcp_send_seg(ch, TF_SYN | TF_ACK, ch->iss, ch->rcv_nxt,
                                     NULL, 0);
                        kprintf("tcp: SYN on :%u from %u.%u.%u.%u:%u -> connection %d\n",
                                (unsigned)dport, (src >> 24) & 255,
                                (src >> 16) & 255, (src >> 8) & 255, src & 255,
                                (unsigned)sport, (unsigned)(ch - g_tcp));
                    } else {
                        memset(ch, 0, sizeof *ch);   /* drop the attempt */
                    }
                }
            }
        }
        spin_unlock(&g_tcp_lock); irq_restore(fl);
        return;
    }

    if (flags & TF_RST) {
        kprintf("tcp: RST on conn :%u - closed by peer\n", (unsigned)c->lport);
        memset(c, 0, sizeof *c);
        spin_unlock(&g_tcp_lock); irq_restore(fl);
        return;
    }

    if (c->st == TCP_SYN_SENT) {
        /* SYN|ACK completes the handshake */
        if ((flags & TF_SYN) && (flags & TF_ACK) && ack == c->snd_una + 1) {
            c->st = TCP_ESTABLISHED;
            c->rcv_nxt = seq + 1;
            c->retries = 0;
            /* the SYN consumed one sequence number: the first DATA byte
             * is iss+1.  Sending data at seq=iss made the peer's TCP trim
             * our first payload byte as a SYN-overlap retransmission (the
             * host echo server received 17 of our 18 bytes). */
            tcp_send_seg(c, TF_ACK, c->iss + 1, c->rcv_nxt, NULL, 0);
            c->snd_una = c->iss + 1;
            kprintf("tcp: established with %u.%u.%u.%u:%u (conn %d)\n",
                    (src >> 24) & 255, (src >> 16) & 255, (src >> 8) & 255,
                    src & 255, (unsigned)sport, (unsigned)(c - g_tcp));
        }
        spin_unlock(&g_tcp_lock); irq_restore(fl);
        return;
    }

    /* ACK processing: slide snd_una over acknowledged bytes (+ FIN). */
    if (flags & TF_ACK) {
        u32 acked = ack - c->snd_una;
        if (acked >= 1 && acked <= c->snd_len) {
            memmove(c->snd, c->snd + acked, c->snd_len - acked);
            c->snd_len -= (u16)acked;
            c->snd_una = ack;
            c->retries = 0;
        } else if (acked == (u32)c->snd_len + 1) {   /* data + our FIN acked */
            c->snd_len = 0;
            c->snd_una = ack;
            c->retries = 0;
        } else if (acked > (u32)c->snd_len + 1) {    /* defensive */
            c->snd_len = 0;
            c->snd_una = ack;
            c->retries = 0;
        }
        if (c->st == TCP_FIN_WAIT1 && acked >= (u32)c->snd_len + 1) {
            /* our FIN acked (acked above already folded in) */
            c->st = TCP_FIN_WAIT2;
        }
        if (c->st == TCP_LAST_ACK && acked >= 1) {
            /* DATA-AFTER-CLOSE FIX: the peer's FIN completes the close,
             * but if data is still buffered (it arrived just before the
             * FIN), freeing the conn HERE destroys it - the userland recv
             * then hits a dead socket and silently loses the payload.
             * Stay in TCP_CLOSING until recv drains the buffer; recv
             * frees the conn when it empties (EOF semantics). */
            if (c->rx_len > 0)
                c->st = TCP_CLOSING;
            else {
                memset(c, 0, sizeof *c);        /* fully closed */
                spin_unlock(&g_tcp_lock); irq_restore(fl);
                return;
            }
        }
    }

    /* inbound data */
    if (dlen > 0 && c->st == TCP_ESTABLISHED) {
        if (seq == c->rcv_nxt && dlen <= TCP_RXBUF - c->rx_len) {
            memcpy(c->rx + c->rx_len, data, dlen);
            c->rx_len += dlen;
            c->rcv_nxt += dlen;
            tcp_send_seg(c, TF_ACK, c->snd_una + c->snd_len, c->rcv_nxt,
                         NULL, 0);
        } else if (seq != c->rcv_nxt) {
            /* out of order: re-ACK what we have so the peer retransmits */
            tcp_send_seg(c, TF_ACK, c->snd_una + c->snd_len, c->rcv_nxt,
                         NULL, 0);
        }
        /* buffer full: drop silently - peer's retransmit timer will
         * recover (window advertised == buffer size) */
    }

    /* peer FIN */
    if (flags & TF_FIN) {
        if (c->st == TCP_ESTABLISHED || c->st == TCP_FIN_WAIT1 ||
            c->st == TCP_FIN_WAIT2) {
            if (seq == c->rcv_nxt) c->rcv_nxt++;
            tcp_send_seg(c, TF_ACK, c->snd_una + c->snd_len, c->rcv_nxt,
                         NULL, 0);
            if (c->st == TCP_ESTABLISHED) {
                c->st = TCP_LAST_ACK;
                c->fin_seq = c->snd_una + c->snd_len;
                c->last_send = pit_ticks();
                c->retries = 0;
                tcp_send_seg(c, TF_FIN | TF_ACK, c->fin_seq, c->rcv_nxt,
                             NULL, 0);
            } else if (c->st == TCP_FIN_WAIT2) {
                memset(c, 0, sizeof *c);        /* both FINs done */
            }
        }
    }
    spin_unlock(&g_tcp_lock);
    irq_restore(fl);
}

/* ---- userland-facing API ---- */

int net_tcp_connect(u32 ip, u16 port) {
    if (!net_own_ip() && (ip & 0xFF000000u) != 0x7F000000u) return -1;
    /* resolve the peer's MAC OUTSIDE the lock (drives inbound RX) */
    if (net_arp_resolve(ip) != 0) {
        kprintf("tcp: ARP resolution for %u.%u.%u.%u failed\n",
                (ip >> 24) & 255, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
        return -1;
    }
    u64 fl = irq_save();
    spin_lock(&g_tcp_lock);
    int slot = -1;
    for (int i = 0; i < TCP_MAX_CONN; i++)
        if (!g_tcp[i].used) { slot = i; break; }
    if (slot < 0) { spin_unlock(&g_tcp_lock); irq_restore(fl); return -1; }
    tcp_conn_t *c = &g_tcp[slot];
    memset(c, 0, sizeof *c);
    c->used = true;
    c->st = TCP_SYN_SENT;
    c->rip = ip;
    c->rport = port;
    c->lport = g_eph_port++;
    c->iss = tcp_seq();
    c->snd_una = c->iss;
    c->last_send = pit_ticks();
    tcp_send_seg(c, TF_SYN, c->iss, 0, NULL, 0);
    spin_unlock(&g_tcp_lock);
    irq_restore(fl);
    kprintf("tcp: connecting to %u.%u.%u.%u:%u (conn %d)...\n",
            (ip >> 24) & 255, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255,
            (unsigned)port, slot);

    /* synchronous handshake */
    u64 t0 = pit_ticks();
    while (pit_ticks() - t0 < TCP_CONNECT_TIMEOUT) {
        net_service();                 /* deliver SYN|ACK + drive retx */
        u64 f2 = irq_save();
        spin_lock(&g_tcp_lock);
        bool done = !g_tcp[slot].used || g_tcp[slot].st == TCP_ESTABLISHED;
        spin_unlock(&g_tcp_lock);
        irq_restore(f2);
        if (done) break;
        __asm__ volatile("pause");
    }
    fl = irq_save();
    spin_lock(&g_tcp_lock);
    if (g_tcp[slot].used && g_tcp[slot].st == TCP_ESTABLISHED) {
        spin_unlock(&g_tcp_lock);
        irq_restore(fl);
        return slot;
    }
    if (g_tcp[slot].used) memset(&g_tcp[slot], 0, sizeof g_tcp[slot]);
    spin_unlock(&g_tcp_lock);
    irq_restore(fl);
    kprintf("tcp: connect to %u.%u.%u.%u:%u FAILED (timeout)\n",
            (ip >> 24) & 255, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255,
            (unsigned)port);
    return -1;
}

int net_tcp_send(int id, const u8 *data, int len) {
    if (len <= 0 || len > TCP_MAX_DATA) return -1;
    u64 fl = irq_save();
    spin_lock(&g_tcp_lock);
    tcp_conn_t *c = &g_tcp[id];
    if (!c->used || c->st != TCP_ESTABLISHED) {
        spin_unlock(&g_tcp_lock); irq_restore(fl); return -1;
    }
    if (c->snd_len + len > TCP_SNDBUF) {
        spin_unlock(&g_tcp_lock); irq_restore(fl); return -1; /* busy */
    }
    memcpy(c->snd + c->snd_len, data, (size_t)len);
    c->snd_len += (u16)len;
    tcp_send_seg(c, TF_ACK | TF_PSH, c->snd_una, c->rcv_nxt,
                 c->snd, c->snd_len);
    c->last_send = pit_ticks();
    c->retries = 0;
    spin_unlock(&g_tcp_lock);
    irq_restore(fl);
    return len;
}

int net_tcp_recv(int id, u8 *buf, int cap) {
    u64 fl = irq_save();
    spin_lock(&g_tcp_lock);
    tcp_conn_t *c = &g_tcp[id];
    if (!c->used) {                         /* EOF: 0, not -1            */
        spin_unlock(&g_tcp_lock); irq_restore(fl); return 0;
    }
    if (c->st == TCP_LISTEN || c->st == TCP_SYN_SENT) {
        spin_unlock(&g_tcp_lock); irq_restore(fl); return -1;
    }
    int n = c->rx_len;
    if (n > cap) n = cap;
    if (n > 0) {
        memcpy(buf, c->rx, (size_t)n);
        memmove(c->rx, c->rx + n, (size_t)(c->rx_len - n));
        c->rx_len -= (u16)n;
    }
    /* closing conn whose buffer is now empty: the close is complete */
    if (c->rx_len == 0 && c->st == TCP_CLOSING)
        memset(c, 0, sizeof *c);
    spin_unlock(&g_tcp_lock);
    irq_restore(fl);
    return n;
}

int net_tcp_close(int id) {
    u64 fl = irq_save();
    spin_lock(&g_tcp_lock);
    tcp_conn_t *c = &g_tcp[id];
    if (!c->used) { spin_unlock(&g_tcp_lock); irq_restore(fl); return -1; }
    if (c->st == TCP_LISTEN) {
        memset(c, 0, sizeof *c);
        spin_unlock(&g_tcp_lock); irq_restore(fl); return 0;
    }
    if (c->st == TCP_CLOSING) {            /* already closing: drop it */
        memset(c, 0, sizeof *c);
        spin_unlock(&g_tcp_lock); irq_restore(fl); return 0;
    }
    if (c->st == TCP_ESTABLISHED || c->st == TCP_FIN_WAIT1) {
        c->st = TCP_FIN_WAIT1;
        c->fin_seq = c->snd_una + c->snd_len;
        c->last_send = pit_ticks();
        c->retries = 0;
        tcp_send_seg(c, TF_FIN | TF_ACK, c->fin_seq, c->rcv_nxt, NULL, 0);
        kprintf("tcp: closing conn %d (FIN sent)\n", id);
    } else {
        memset(c, 0, sizeof *c);
        spin_unlock(&g_tcp_lock); irq_restore(fl); return 0;
    }
    spin_unlock(&g_tcp_lock);
    irq_restore(fl);

    u64 t0 = pit_ticks();
    while (pit_ticks() - t0 < TCP_CLOSE_TIMEOUT) {
        net_service();
        u64 f2 = irq_save();
        spin_lock(&g_tcp_lock);
        bool gone = !g_tcp[id].used;
        bool peer_done = g_tcp[id].used && g_tcp[id].st == TCP_FIN_WAIT2;
        spin_unlock(&g_tcp_lock);
        irq_restore(f2);
        if (gone || peer_done) break;
        __asm__ volatile("pause");
    }
    fl = irq_save();
    spin_lock(&g_tcp_lock);
    if (g_tcp[id].used) memset(&g_tcp[id], 0, sizeof g_tcp[id]);
    spin_unlock(&g_tcp_lock);
    irq_restore(fl);
    return 0;
}

int net_tcp_listen(u16 port) {
    u64 fl = irq_save();
    spin_lock(&g_tcp_lock);
    for (int i = 0; i < TCP_MAX_CONN; i++)
        if (g_tcp[i].used && g_tcp[i].lport == port) {
            spin_unlock(&g_tcp_lock); irq_restore(fl); return -1;
        }
    for (int i = 0; i < TCP_MAX_CONN; i++) {
        if (!g_tcp[i].used) {
            memset(&g_tcp[i], 0, sizeof g_tcp[i]);
            g_tcp[i].used = true;
            g_tcp[i].st = TCP_LISTEN;
            g_tcp[i].lport = port;
            kprintf("tcp: listening on :%u (listener id %d)\n", (unsigned)port, i);
            spin_unlock(&g_tcp_lock); irq_restore(fl);
            return i;                 /* return the LISTENER ID - callers
                                         must not assume slot 0 (a leftover
                                         listener or conn can occupy it) */
        }
    }
    spin_unlock(&g_tcp_lock);
    irq_restore(fl);
    return -1;
}

/* Non-blocking: returns an ESTABLISHED child conn id, or -2 = not yet. */
int net_tcp_accept(int listener_id) {
    u64 fl = irq_save();
    spin_lock(&g_tcp_lock);
    tcp_conn_t *l = &g_tcp[listener_id];
    if (!l->used || l->st != TCP_LISTEN) {
        spin_unlock(&g_tcp_lock); irq_restore(fl); return -1;
    }
    for (int i = 0; i < TCP_MAX_CONN; i++) {
        tcp_conn_t *o = &g_tcp[i];
        if (o->used && o->st == TCP_ESTABLISHED && o->lport == l->lport &&
            i != listener_id) {
            spin_unlock(&g_tcp_lock); irq_restore(fl);
            return i;
        }
    }
    spin_unlock(&g_tcp_lock);
    irq_restore(fl);
    return -2;
}

void tcp_init(void) {
    memset(g_tcp, 0, sizeof g_tcp);
    spin_init(&g_tcp_lock);
    kprintf("tcp: TCP ready (8 conns, %d B rx / %d B tx per conn)\n",
            TCP_RXBUF, TCP_SNDBUF);
}
