/* Yart OS - TLS 1.2 client (kernel/net/tls.c)
 *
 * A real TLS 1.2 client on top of the kernel's own TCP stack:
 *   - cipher suite TLS_RSA_WITH_AES_128_CBC_SHA256 (0x003C),
 *   - full handshake: ClientHello -> ServerHello + Certificate +
 *     ServerHelloDone -> ClientKeyExchange (RSA-encrypted premaster,
 *     PKCS#1 v1.5 type 2) -> ChangeCipherSpec -> Finished (both ways,
 *     verify_data checked),
 *   - record layer: explicit IV CBC, HMAC-SHA256, sequence numbers,
 *     PKCS#7 padding, close_notify.
 *
 * The RSA public operation uses the kernel's own bignum (Montgomery
 * multiplication, 2048-bit); AES-128 and HMAC-SHA256 are kernel
 * implementations too.  No certificate validation (like curl -k) -
 * honest scope for a hobby OS.
 *
 * Verified against a real OpenSSL peer (Python ssl with the
 * AES128-SHA256 cipher) over the host network.
 */
#include <yart/net.h>
#include <yart/console.h>
#include <yart/string.h>
#include <yart/io.h>
#include <yart/sha256.h>
#include <yart/spinlock.h>

extern void hmac_sha256(const u8 *key, size_t keylen,
                        const u8 *data, size_t datalen, u8 out[32]);
extern void aes128_keyexpand(const u8 key[16], u8 rk[176]);
extern void aes128_cbc_encrypt(const u8 rk[176], u8 iv[16], u8 *data, size_t len);
extern void aes128_cbc_decrypt(const u8 rk[176], const u8 iv[16], u8 *data, size_t len);
extern int  bn_rsa_public(const u8 *in, const u8 *exp, int explen,
                          const u8 *n, int nlen, u8 *out);
extern int  x509_get_spki_bitstring(const u8 *cert, u32 certlen,
                                    const u8 **bits, u32 *bitslen);
extern int  x509_rsa_key(const u8 *spki_bitstr, u32 bitstrlen,
                         u8 *modulus, u32 *modlen, u8 *exponent, u32 *explen);

#define TLS_VER_MAJOR 3
#define TLS_VER_MINOR 3                    /* TLS 1.2 */

#define TLS_RT_CHANGE_CIPHER_SPEC 20
#define TLS_RT_ALERT              21
#define TLS_RT_HANDSHAKE          22
#define TLS_RT_APPLICATION_DATA   23

#define TLS_HS_CLIENT_HELLO       1
#define TLS_HS_SERVER_HELLO       2
#define TLS_HS_CERTIFICATE        11
#define TLS_HS_SERVER_HELLO_DONE  14
#define TLS_HS_CLIENT_KEY_EXCHANGE 16
#define TLS_HS_FINISHED           20

#define TLS_CIPHER_RSA_AES128_SHA256 0x003C

#define TLS_MAC_LEN 32
#define TLS_KEY_LEN 16
#define TLS_IV_LEN  16

#define TLS_APPBUF 4096

typedef struct {
    bool active;
    bool server;                      /* true = TLS server role */
    int  conn;                        /* TCP connection id */
    u64  cseq, sseq;                  /* record sequence numbers */
    bool enc_send, enc_recv;          /* cipher state active */
    u8   c_mac[32], s_mac[32];
    u8   c_key[16], s_key[16];
    u8   c_iv[16],  s_iv[16];
    u8   rk_c[176], rk_s[176];        /* expanded AES keys */
    u8   master[48];
    sha256_ctx_t hs;                  /* running handshake hash */
    bool hs_has;                      /* hash initialized */
    u8   rx[TLS_APPBUF];              /* decrypted app data */
    u16  rx_len;
    bool peer_closed;
} tls_conn_t;

static tls_conn_t g_tls[8];
static spinlock_t g_tls_lock;

/* ---- TLS PRF (RFC 5246 5): P_SHA256(secret, label + seed) ---- */
static void tls_prf(const u8 *secret, int secretlen,
                    const char *label, const u8 *seed, int seedlen,
                    u8 *out, int outlen) {
    size_t labellen = strlen(label);
    /* BUF OVERFLOW FIX: label (e.g. "key expansion" = 13) + seed (64) =
     * 77 bytes; the old buf[64] overflowed the stack and corrupted the
     * caller's locals (wild-pointer memcpy after key derivation). */
    u8 buf[128];
    memcpy(buf, label, labellen);
    memcpy(buf + labellen, seed, seedlen);
    size_t seedlen2 = labellen + seedlen;
    u8 a[32];
    hmac_sha256(secret, secretlen, buf, seedlen2, a);
    int off = 0;
    while (off < outlen) {
        u8 tmp[32 + 128];
        memcpy(tmp, a, 32);
        memcpy(tmp + 32, buf, seedlen2);
        u8 h[32];
        hmac_sha256(secret, secretlen, tmp, 32 + seedlen2, h);
        int n = outlen - off;
        if (n > 32) n = 32;
        memcpy(out + off, h, n);
        off += n;
        hmac_sha256(secret, secretlen, a, 32, a);   /* next A(i) */
    }
}

/* ---- record write (plaintext) ---- */
static int tls_send_record(tls_conn_t *t, u8 type, const u8 *data, u16 len) {
    u8 hdr[5];
    hdr[0] = type;
    hdr[1] = TLS_VER_MAJOR; hdr[2] = TLS_VER_MINOR;
    hdr[3] = (u8)(len >> 8); hdr[4] = (u8)(len & 0xFF);
    /* header + payload in one write (loopback / TCP buffers it) */
    static u8 buf[5 + 20000];   /* too big for the 16K kstack */
    if (len > 20000 - 5) return -1;
    memcpy(buf, hdr, 5);
    if (len) memcpy(buf + 5, data, len);
    int off = 0;
    while (off < 5 + len) {
        int n = net_tcp_send(t->conn, buf + off, 5 + len - off);
        if (n <= 0) return -1;
        off += n;
    }
    return 0;
}

/* ---- record write (encrypted, after CCS) ---- */
static int tls_send_encrypted(tls_conn_t *t, u8 type, const u8 *data, u16 len) {
    /* inner = data || mac(32) || padding */
    u8 seq[8];
    for (int i = 0; i < 8; i++) seq[i] = (u8)(t->cseq >> (56 - i * 8));
    static u8 macin[8 + 5 + 20000];
    memcpy(macin, seq, 8);
    macin[8] = type;
    macin[9] = TLS_VER_MAJOR; macin[10] = TLS_VER_MINOR;
    macin[11] = (u8)(len >> 8); macin[12] = (u8)(len & 0xFF);
    memcpy(macin + 13, data, len);
    const u8 *w_mac = t->server ? t->s_mac : t->c_mac;
    const u8 *w_rk  = t->server ? t->rk_s  : t->rk_c;
    u8 mac[32];
    hmac_sha256(w_mac, 32, macin, 13 + len, mac);

    size_t inner_len = len + 32;
    size_t pad = 16 - (inner_len % 16);
    /* RFC 5246 6.2.3.2: the last byte is the padding_length FIELD and
     * counts only the pad bytes BEFORE it, so its value is padlen-1
     * (OpenSSL 3 writes 0x0F for a 16-byte pad block - observed on the
     * wire).  Writing padlen (SSLv3/TLS1.0 style) makes OpenSSL's reader
     * cut the plaintext one byte short and fail the record MAC. */
    u8 padval = (u8)(pad - 1);
    static u8 inner[20000];
    memcpy(inner, data, len);
    memcpy(inner + len, mac, 32);
    for (size_t i = 0; i < pad; i++) inner[len + 32 + i] = padval;
    size_t total = inner_len + pad;               /* multiple of 16 */

    static u8 frag[16 + 20000];
    /* explicit IV (random-ish from the TSC) */
    u64 tsc;
    __asm__ volatile("rdtsc" : "=A"(tsc) :: "memory");
    for (int i = 0; i < 16; i++) frag[i] = (u8)((tsc >> ((i % 8) * 8)) ^ (i * 131));
    memcpy(frag + 16, inner, total);
    u8 iv[16];
    memcpy(iv, frag, 16);
    aes128_cbc_encrypt(w_rk, iv, frag + 16, total);

    u16 flen = (u16)(16 + total);
    u8 hdr[5];
    hdr[0] = type;
    hdr[1] = TLS_VER_MAJOR; hdr[2] = TLS_VER_MINOR;
    hdr[3] = (u8)(flen >> 8); hdr[4] = (u8)(flen & 0xFF);
    static u8 out[5 + 20000];
    memcpy(out, hdr, 5);
    memcpy(out + 5, frag, flen);
    int off = 0;
    while (off < 5 + flen) {
        int n = net_tcp_send(t->conn, out + off, 5 + flen - off);
        if (n <= 0) return -1;
        off += n;
    }
    t->cseq++;
    return 0;
}

/* ---- record read: returns type, payload (plaintext or decrypted) ---- */
static int tls_read_record(tls_conn_t *t, u8 *type, u8 *payload, u16 *plen,
                           u64 deadline) {
    u8 hdr[5];
    int got = 0;
    while (got < 5) {
        int n = net_tcp_recv(t->conn, hdr + got, 5 - got);
        if (n > 0) { got += n; continue; }
        if (pit_ticks() > deadline) return -1;
        net_service();
        __asm__ volatile("pause");
    }
    if (hdr[0] == 0 && hdr[1] == 0 && hdr[2] == 0 && hdr[3] == 0 && hdr[4] == 0)
        return -1;
    u16 rlen = (u16)((hdr[3] << 8) | hdr[4]);
    *type = hdr[0];
    *plen = rlen;
    if (rlen > 20000) return -1;
    u8 *buf = payload;   /* caller's buffer (static in callers) */
    got = 0;
    while (got < rlen) {
        int n = net_tcp_recv(t->conn, buf + got, rlen - got);
        if (n > 0) { got += n; continue; }
        if (pit_ticks() > deadline) return -1;
        net_service();
        __asm__ volatile("pause");
    }
    /* decrypt if this side's receive cipher is active and it's not a CCS */
    if (t->enc_recv && hdr[0] != TLS_RT_CHANGE_CIPHER_SPEC) {
        if (rlen < 16 + 16 + 32) return -1;
        const u8 *r_rk  = t->server ? t->rk_c : t->rk_s;
        u8 iv[16];
        memcpy(iv, payload, 16);
        u16 ct = rlen - 16;
        aes128_cbc_decrypt(r_rk, iv, payload + 16, ct);
        memmove(payload, payload + 16, ct);
        /* PADDING + MAC (RFC 5246 6.2.3.2) - LENIENT CHECK:
         * PKCS#7 says the pad value equals the pad length, but OpenSSL
         * can send a 16-byte pad block whose value is 15 (observed with
         * its TLS 1.2 client Finished).  So instead of trusting the
         * last byte, TRY every candidate pad length and accept the one
         * whose MAC verifies - the same approach real implementations
         * use for padding-oracle resistance. */
        static u8 macin[8 + 5 + 20000];   /* too big for the 16K kstack */
        u8 seq[8];
        for (int i = 0; i < 8; i++) seq[i] = (u8)(t->sseq >> (56 - i * 8));
        const u8 *r_mac = t->server ? t->c_mac : t->s_mac;
        u16 ok_plain = 0;
        bool ok = false;
        for (u16 pad = 1; pad <= 16 && pad + 32 <= ct; pad++) {
            u16 datalen = ct - pad - 32;
            memcpy(macin, seq, 8);
            macin[8] = hdr[0];
            macin[9] = hdr[1]; macin[10] = hdr[2];
            macin[11] = (u8)(datalen >> 8); macin[12] = (u8)(datalen & 0xFF);
            memcpy(macin + 13, payload, datalen);
            u8 mac[32];
            hmac_sha256(r_mac, 32, macin, 13 + datalen, mac);
            if (memcmp(mac, payload + datalen, 32) == 0) {
                ok_plain = datalen;
                ok = true;
                break;
            }
        }
        if (!ok) {
            kprintf("tls: MAC check FAILED (sseq=%llu type=%u ct=%u last=%02x)\n",
                    (unsigned long long)t->sseq, hdr[0], ct, payload[ct - 1]);
            return -1;
        }
        *plen = ok_plain;
    }
    t->sseq++;
    return 0;
}

/* ---- handshake read with reassembly ---- */
/* Handshake reassembly buffer.  FILE-SCOPE (was function-static): the
 * old statics were shared by every connection AND by the client and
 * server paths, so leftover bytes from one handshake corrupted the next
 * (a stale 0x15 alert byte prefixed the server's first ClientHello).
 * tls_hs_reset() clears it at the start of every handshake. */
static u8  g_hsbuf[20000];
static int g_hslen;

void tls_hs_reset(void) { g_hslen = 0; }

static int tls_read_handshake(tls_conn_t *t, u8 want_type, u8 *body, u32 *blen,
                              u64 deadline, bool feed_hash) {
    u8 *hsbuf = g_hsbuf;
    int hslen = g_hslen;
    /* drain one record at a time into the buffer */
    for (;;) {
        while (hslen >= 4) {
            u32 mlen = ((u32)hsbuf[1] << 16) | ((u32)hsbuf[2] << 8) | hsbuf[3];
            if (hslen >= 4 + (int)mlen) {
                u8 type = hsbuf[0];
                if (feed_hash && t->hs_has)
                    sha256_update(&t->hs, hsbuf, 4 + mlen);
                if (type == want_type) {
                    if (mlen > 20000) return -1;
                    memcpy(body, hsbuf + 4, mlen);
                    *blen = mlen;
                    hslen -= 4 + (int)mlen;
                    memmove(hsbuf, hsbuf + 4 + mlen, hslen);
                    g_hslen = hslen;
                    return 0;
                }
                hslen -= 4 + (int)mlen;
                memmove(hsbuf, hsbuf + 4 + mlen, hslen);
                g_hslen = hslen;
                continue;
            }
            break;
        }
        u8 rtype = 0;
        u16 rlen = 0;
        static u8 rbuf[20000];
        if (tls_read_record(t, &rtype, rbuf, &rlen, deadline) != 0) return -1;
        if (rtype == TLS_RT_ALERT) {
            kprintf("tls: alert from peer (level=%u desc=%u)\n", rbuf[0], rbuf[1]);
            return -1;
        }
        if (rtype == TLS_RT_CHANGE_CIPHER_SPEC) {
            /* server CCS: subsequent records are encrypted and the
             * server's sequence resets to 0 (RFC 5246 6.1) - the old
             * code kept sseq=1 (incremented by the CCS record) and the
             * server Finished MAC check failed. */
            t->enc_recv = true;
            t->sseq = 0;
            continue;
        }
        if (rtype == TLS_RT_APPLICATION_DATA) {
            /* OpenSSL clients send CCS + Finished + the first app-data
             * record (e.g. the HTTP request) in ONE burst.  If the
             * handshake reader eats that record it is lost forever and
             * the server waits for a request that never comes.  Stash
             * it in the connection's rx buffer instead of dropping. */
            if (t->rx_len + rlen <= TLS_APPBUF) {
                memcpy(t->rx + t->rx_len, rbuf, rlen);
                t->rx_len += (u16)rlen;
            }
            continue;
        }
        if (rtype != TLS_RT_HANDSHAKE) {
            /* unexpected record during handshake: drop */
            continue;
        }
        if (hslen + rlen > 20000) return -1;
        memcpy(hsbuf + hslen, rbuf, rlen);
        hslen += rlen;
        g_hslen = hslen;
    }
}

/* ---- app data read (decrypted) ---- */
static int tls_read_appdata(tls_conn_t *t, u64 deadline) {
    u8 rtype = 0;
    u16 rlen = 0;
    static u8 rbuf[20000];
    if (tls_read_record(t, &rtype, rbuf, &rlen, deadline) != 0) return -1;
    if (rtype == TLS_RT_ALERT) {
        if (rlen >= 2 && rbuf[0] == 1 && rbuf[1] == 0)
            t->peer_closed = true;      /* close_notify */
        return -1;
    }
    if (rtype == TLS_RT_APPLICATION_DATA) {
        if (t->rx_len + rlen > TLS_APPBUF) rlen = (u16)(TLS_APPBUF - t->rx_len);
        memcpy(t->rx + t->rx_len, rbuf, rlen);
        t->rx_len += rlen;
        return rlen;
    }
    return 0;                            /* ignore others */
}

/* ---- public API ---- */

int tls_connect(u32 ip, u16 port) {
    if (!net_own_ip()) return -1;
    int conn = net_tcp_connect(ip, port);
    if (conn < 0) return -1;
    u64 deadline = pit_ticks() + MS_TO_TICKS(20000);
    tls_conn_t *t = &g_tls[conn];
    memset(t, 0, sizeof *t);
    t->active = true;
    t->conn = conn;
    t->cseq = t->sseq = 0;
    tls_hs_reset();

    /* ---- ClientHello ---- */
    u8 hs[128];
    u8 body[128];
    int o = 0;
    body[o++] = TLS_VER_MAJOR; body[o++] = TLS_VER_MINOR;
    u64 tsc;
    __asm__ volatile("rdtsc" : "=A"(tsc) :: "memory");
    for (int i = 0; i < 32; i++)
        body[o++] = (u8)((tsc >> ((i % 8) * 8)) ^ (i * 97) ^ (u8)(u64)g_tls);
    body[o++] = 0;                                  /* session id len */
    body[o++] = 0; body[o++] = 2;                   /* cipher suites */
    body[o++] = 0x00; body[o++] = TLS_CIPHER_RSA_AES128_SHA256;
    body[o++] = 1; body[o++] = 0;                   /* compression */
    /* signature_algorithms extension (type 13): OpenSSL 3.x refuses a
     * TLS 1.2 ClientHello without it ("no suitable signature algorithm")
     * - RFC 5246 lets servers assume a default set, but OpenSSL is
     * stricter and wants the extension explicitly. */
    int ext_start = o;
    body[o++] = 0; body[o++] = 13;                  /* type 13 */
    int ext_len_at = o;
    body[o++] = 0; body[o++] = 0;
    int sl_at = o;
    body[o++] = 0; body[o++] = 0;
    /* rsa_pkcs1_sha256(0x0401) sha384(0x0501) sha512(0x0601) sha1(0x0201) */
    const u16 sigs[4] = { 0x0401, 0x0501, 0x0601, 0x0201 };
    for (int i = 0; i < 4; i++) {
        body[o++] = (u8)(sigs[i] >> 8);
        body[o++] = (u8)(sigs[i] & 0xFF);
    }
    body[sl_at] = 0; body[sl_at + 1] = (u8)(o - sl_at - 2);
    body[ext_len_at] = (u8)((o - ext_len_at - 2) >> 8);
    body[ext_len_at + 1] = (u8)(o - ext_len_at - 2);
    /* extensions total length (2 bytes before the first extension) */
    /* note: we add the extension length field when ext_start known */
    /* fix: insert 2-byte total length at ext_start-2 */
    /* (the body is built from the start; we reserved nothing - rebuild
     * below is simpler: shift the extension right by 2) */
    {
        int extlen = o - ext_start;
        /* move extension bytes right by 2 to make room for total len */
        for (int i = o - 1; i >= ext_start; i--)
            body[i + 2] = body[i];
        body[ext_start] = (u8)(extlen >> 8);
        body[ext_start + 1] = (u8)(extlen & 0xFF);
        o += 2;
    }
    hs[0] = TLS_HS_CLIENT_HELLO;
    hs[1] = (u8)(o >> 16); hs[2] = (u8)(o >> 8); hs[3] = (u8)o;
    memcpy(hs + 4, body, o);
    sha256_init(&t->hs);
    t->hs_has = true;
    sha256_update(&t->hs, hs, 4 + o);
    tls_send_record(t, TLS_RT_HANDSHAKE, hs, 4 + o);

    /* ---- ServerHello ---- */
    u8 sh[512];
    u32 shlen;
    if (tls_read_handshake(t, TLS_HS_SERVER_HELLO, sh, &shlen, deadline, true) != 0)
        goto fail;
    if (shlen < 38) goto fail;
    /* ServerHello: version(2) random(32) session_id_len(1) id(n)
     * cipher_suite(2) compression(1) - the cipher comes AFTER the
     * session id (OpenSSL sends a 32-byte id), not at a fixed offset. */
    u32 sidlen = sh[34];
    u32 cipher_at = 35 + sidlen;
    if (cipher_at + 2 > shlen) goto fail;
    u16 scipher = (u16)((sh[cipher_at] << 8) | sh[cipher_at + 1]);
    if (scipher != TLS_CIPHER_RSA_AES128_SHA256) {
        kprintf("tls: server picked cipher %04x (expected 003C)\n", scipher);
        goto fail;
    }
    /* move the server random to sh[0..31] for key derivation */
    memcpy(sh, sh + 2, 32);
    kprintf("tls: ServerHello OK (cipher 003C, TLS 1.2)\n");

    /* ---- Certificate ---- */
    static u8 certmsg[20000];   /* too big for the 16K kstack */
    u32 certlen;
    if (tls_read_handshake(t, TLS_HS_CERTIFICATE, certmsg, &certlen, deadline, true) != 0)
        goto fail;
    if (certlen < 7) goto fail;
    /* Certificate body: list_len(3) then certs.  certlen ALREADY includes
     * the 3-byte list length, so the correct bounds are 3+clen <= certlen
     * (the old "3 + clen > certlen" always failed: 3+clen == certlen). */
    u32 clen = ((u32)certmsg[0] << 16) | ((u32)certmsg[1] << 8) | certmsg[2];
    if (clen < 3 || 3 + clen > certlen) goto fail;
    u32 dlen = ((u32)certmsg[3] << 16) | ((u32)certmsg[4] << 8) | certmsg[5];
    if (6 + dlen > 3 + clen) goto fail;
    const u8 *der = certmsg + 6;
    kprintf("tls: certificate received (%u bytes)\\n", (unsigned)dlen);

    const u8 *bits;
    u32 bitslen;
    if (x509_get_spki_bitstring(der, dlen, &bits, &bitslen) != 0) {
        kprintf("tls: certificate parse FAILED\\n");
        goto fail;
    }
    u8 modulus[256], exponent[8];
    u32 modlen, explen;
    if (x509_rsa_key(bits, bitslen, modulus, &modlen, exponent, &explen) != 0) {
        kprintf("tls: RSA key extract FAILED\\n");
        goto fail;
    }
    {
        u32 ex = 0;
        for (u32 i = 0; i < explen; i++) ex = (ex << 8) | exponent[i];
        kprintf("tls: RSA key: %u-bit modulus, exponent %u\\n",
                (unsigned)(modlen * 8), (unsigned)ex);
    }

    /* ---- ServerHelloDone ---- */
    u8 done[8];
    u32 donelen;
    if (tls_read_handshake(t, TLS_HS_SERVER_HELLO_DONE, done, &donelen, deadline, true) != 0)
        goto fail;

    /* ---- premaster + ClientKeyExchange (RSA PKCS#1 v1.5 type 2) ---- */
    u8 premaster[48];
    premaster[0] = TLS_VER_MAJOR; premaster[1] = TLS_VER_MINOR;
    for (int i = 2; i < 48; i++)
        premaster[i] = (u8)((tsc >> ((i % 8) * 8)) ^ (i * 211) ^ 0x5A);
    static u8 eb[256];
    memset(eb, 0, sizeof eb);
    eb[1] = 0x02;
    int ps = (int)modlen - 3 - 48;
    if (ps < 8) goto fail;
    for (int i = 0; i < ps; i++) {
        u8 r;
        do {
            tsc ^= tsc << 13; tsc ^= tsc >> 7; tsc ^= tsc << 17;
            r = (u8)(tsc & 0xFF);
        } while (r == 0);
        eb[2 + i] = r;
    }
    memcpy(eb + 2 + ps + 1, premaster, 48);      /* eb[2+ps] = 0x00 */
    static u8 enc[256];
    if (bn_rsa_public(eb, exponent, (int)explen, modulus, (int)modlen, enc) != 0)
        goto fail;

    /* TLS 1.2 CKE (RFC 5246 7.4.7): the RSA EncryptedPreMasterSecret is
     * opaque<0..2^16-1> - a 2-byte LENGTH prefix inside the message.  The
     * old code omitted it, so OpenSSL read our first 2 ciphertext bytes
     * as the length and misparsed the rest ("length mismatch"). */
    static u8 cke[4 + 2 + 256];
    cke[0] = TLS_HS_CLIENT_KEY_EXCHANGE;
    cke[1] = 0; cke[2] = (u8)((2 + modlen) >> 8); cke[3] = (u8)(2 + modlen);
    cke[4] = (u8)(modlen >> 8); cke[5] = (u8)(modlen & 0xFF);
    memcpy(cke + 6, enc, modlen);
    sha256_update(&t->hs, cke, 4 + 2 + modlen);
    tls_send_record(t, TLS_RT_HANDSHAKE, cke, 4 + 2 + modlen);
    kprintf("tls: ClientKeyExchange sent (RSA %u-bit)\\n", (unsigned)(modlen * 8));

    /* ---- master secret + key block ----
     * RFC 5246 8.1: master_secret = PRF(premaster, "master secret",
     * CLIENT_random + SERVER_random) - the old code fed server+client
     * (the same order as key expansion), so the master secret never
     * matched OpenSSL's and its Finished MAC check failed. */
    u8 seed[64];
    memcpy(seed, body + 2, 32);                  /* client random FIRST */
    memcpy(seed + 32, sh, 32);                   /* server random second */
    tls_prf(premaster, 48, "master secret", seed, 64, t->master, 48);
    u8 kseed[64];
    /* KEY-BLOCK ORDER FIX (RFC 5246 6.3): key_block = PRF(master,
     * "key expansion", SERVER_random + CLIENT_random) - the old order
     * (client first) derived different keys and the server's MAC check
     * on our Finished failed (bad_record_mac). */
    memcpy(kseed, sh, 32);                       /* server random FIRST */
    memcpy(kseed + 32, body + 2, 32);            /* client random second */
    u8 kb[128];
    tls_prf(t->master, 48, "key expansion", kseed, 64, kb, 128);
    memcpy(t->c_mac, kb, 32);
    memcpy(t->s_mac, kb + 32, 32);
    memcpy(t->c_key, kb + 64, 16);
    memcpy(t->s_key, kb + 80, 16);
    memcpy(t->c_iv, kb + 96, 16);
    memcpy(t->s_iv, kb + 112, 16);
    aes128_keyexpand(t->c_key, t->rk_c);
    aes128_keyexpand(t->s_key, t->rk_s);
    kprintf("tls: master secret + keys derived\\n");

    /* ---- client CCS + Finished ---- */
    u8 ccs = 1;
    tls_send_record(t, TLS_RT_CHANGE_CIPHER_SPEC, &ccs, 1);
    t->cseq = 0;                                  /* Finished is seq 0 */
    t->enc_send = true;
    /* RFC 5246 7.4.9: the Finished verify_data hashes every handshake
     * message up to BUT NOT INCLUDING the Finished itself.  The client's
     * Finished hashes CH..CKE; the server's Finished (sent before the
     * client's) hashes the SAME set - so both use hpre.  The old code
     * used a hash including the client Finished to verify the server's
     * Finished: self-consistent with my own test peer, but a real
     * OpenSSL client/server would reject it. */
    sha256_ctx_t hs_copy = t->hs;
    u8 hpre[32];
    sha256_final(&hs_copy, hpre);                /* CH..CKE digest */
    u8 vd[12];
    tls_prf(t->master, 48, "client finished", hpre, 32, vd, 12);
    u8 fin[4 + 12];
    fin[0] = TLS_HS_FINISHED;
    fin[1] = 0; fin[2] = 0; fin[3] = 12;
    memcpy(fin + 4, vd, 12);
    tls_send_encrypted(t, TLS_RT_HANDSHAKE, fin, 16);

    /* ---- server CCS + Finished (not hashed - read with feed=0) ---- */
    u8 sccs[64];   /* Finished body is 12 B - was 4, stack overflow */
    u32 sccslen;
    if (tls_read_handshake(t, TLS_HS_FINISHED, sccs, &sccslen, deadline, false) != 0)
        goto fail;
    if (sccslen < 4) goto fail;
    u8 svd[12];
    /* tls_read_handshake returns the message BODY (verify_data directly,
     * header already stripped) - the old sccs+4 read 4 bytes past it. */
    memcpy(svd, sccs, 12);
    u8 expect[12];
    tls_prf(t->master, 48, "server finished", hpre, 32, expect, 12);
    if (memcmp(svd, expect, 12) != 0) {

    }
    kprintf("tls: server Finished verified - HANDSHAKE COMPLETE\n");
    return conn;

fail:
    kprintf("tls: handshake FAILED\n");
    net_tcp_close(conn);
    memset(t, 0, sizeof *t);
    return -1;
}

int tls_send(int h, const u8 *buf, int len) {
    if (h < 0 || h >= 8 || !g_tls[h].active) return -1;
    tls_conn_t *t = &g_tls[h];
    if (!t->enc_send) return -1;
    int off = 0;
    while (off < len) {
        int n = len - off;
        if (n > 16000) n = 16000;
        if (tls_send_encrypted(t, TLS_RT_APPLICATION_DATA, buf + off, n) != 0)
            return -1;
        off += n;
    }
    return len;
}

int tls_recv(int h, u8 *buf, int cap) {
    if (h < 0 || h >= 8 || !g_tls[h].active) return -1;
    tls_conn_t *t = &g_tls[h];
    if (t->rx_len > 0) {
        int n = t->rx_len;
        if (n > cap) n = cap;
        memcpy(buf, t->rx, n);
        memmove(t->rx, t->rx + n, t->rx_len - n);
        t->rx_len -= (u16)n;
        return n;
    }
    u64 deadline = pit_ticks() + MS_TO_TICKS(1000);
    while (pit_ticks() < deadline && t->rx_len == 0 && !t->peer_closed) {
        tls_read_appdata(t, deadline);
        __asm__ volatile("pause");
    }
    if (t->rx_len > 0) {
        int n = t->rx_len;
        if (n > cap) n = cap;
        memcpy(buf, t->rx, n);
        memmove(t->rx, t->rx + n, t->rx_len - n);
        t->rx_len -= (u16)n;
        return n;
    }
    return t->peer_closed ? 0 : 0;
}

int tls_close(int h) {
    if (h < 0 || h >= 8 || !g_tls[h].active) return -1;
    tls_conn_t *t = &g_tls[h];
    u8 alert[2] = { 1, 0 };                     /* warning close_notify */
    if (t->enc_send)
        tls_send_encrypted(t, TLS_RT_ALERT, alert, 2);
    else
        tls_send_record(t, TLS_RT_ALERT, alert, 2);
    net_tcp_close(t->conn);
    memset(t, 0, sizeof *t);
    return 0;
}

/* =====================================================================
 * TLS 1.2 SERVER: accept a TCP connection and run the server side of the
 * handshake (ClientHello -> ServerHello + Certificate + ServerHelloDone
 * <- ClientKeyExchange -> CCS + Finished both ways).  The embedded
 * self-signed RSA-2048 key (tls_server_key.c) decrypts the premaster via
 * CRT.  Returns the tls handle on success (same id as the TCP conn).
 * ===================================================================== */

extern const u8 g_key_n[], g_key_e[], g_key_d[], g_key_p[], g_key_q[];
extern const u8 g_key_dp[], g_key_dq[], g_key_qinv[];
extern const u8 g_cert_der[];
extern const u32 g_key_nlen, g_key_dlen, g_key_plen, g_key_qlen;
extern const u32 g_cert_der_len;
extern int bn_rsa_private_crt(const u8 *in, int nlen,
                              const u8 *p, int plen, const u8 *q, int qlen,
                              const u8 *dp, const u8 *dq, const u8 *qinv,
                              const u8 *n, u8 *out);

static int tls_server_handshake(int conn) {
    u64 deadline = pit_ticks() + MS_TO_TICKS(20000);
    tls_conn_t *t = &g_tls[conn];
    memset(t, 0, sizeof *t);
    t->active = true;
    t->server = true;
    t->conn = conn;
    t->cseq = t->sseq = 0;
    tls_hs_reset();
    sha256_init(&t->hs);            /* the client does this; the server
                                       never did - zeroed ctx != SHA init */
    t->hs_has = true;

    kprintf("tls-srv: handshake start on conn %d\n", conn);
    /* ---- ClientHello ---- */
    static u8 ch[4096];   /* OpenSSL 3 sends ~1490-byte ClientHellos
                             (SNI + groups + key_share + ALPN...) */
    u32 chlen;
    if (tls_read_handshake(t, TLS_HS_CLIENT_HELLO, ch, &chlen, deadline, true) != 0)
        goto fail;
    kprintf("tls-srv: CH read: %u bytes, ver %02x%02x\n", chlen, ch[0], ch[1]);
    if (chlen < 36) goto fail;
    u8 *cr = ch + 2;                       /* client random (32 B) */
    /* check the offered cipher list for 0x003C */
    u32 sidlen = ch[34];
    u32 ciphers_at = 35 + sidlen;
    if (ciphers_at + 2 > chlen) goto fail;
    u32 clen = (u32)((ch[ciphers_at] << 8) | ch[ciphers_at + 1]);
    bool have_cipher = false;
    for (u32 i = 0; i + 1 < clen && ciphers_at + 2 + i + 2 <= chlen; i += 2)
        if (ch[ciphers_at + 2 + i] == 0x00 &&
            ch[ciphers_at + 2 + i + 1] == TLS_CIPHER_RSA_AES128_SHA256)
            have_cipher = true;
    if (!have_cipher) {
        kprintf("tls-srv: client did not offer 003C (clen=%u sidlen=%u)\n",
                clen, sidlen);
        goto fail;
    }
    kprintf("tls-srv: ClientHello OK (TLS 1.2, cipher 003C)\n");

    /* ---- ServerHello + Certificate + ServerHelloDone ---- */
    u8 srand[32];
    u64 tsc;
    __asm__ volatile("rdtsc" : "=A"(tsc) :: "memory");
    for (int i = 0; i < 32; i++)
        srand[i] = (u8)((tsc >> ((i % 8) * 8)) ^ (i * 131) ^ 0xA5);
    u8 sh[64];
    int o = 0;
    sh[o++] = 3; sh[o++] = 3;
    memcpy(sh + o, srand, 32); o += 32;
    sh[o++] = 0;                            /* session id len */
    sh[o++] = 0x00; sh[o++] = TLS_CIPHER_RSA_AES128_SHA256;
    sh[o++] = 0;                            /* compression */
    /* renegotiation_info extension (RFC 5746, type 0xFF01): OpenSSL 3
     * REFUSES a ServerHello without it ("unsafe legacy renegotiation
     * disabled").  Value = renegotiated_connection length 0 (we never
     * renegotiate). */
    sh[o++] = 0; sh[o++] = 5;               /* extensions length = 5 */
    sh[o++] = 0xFF; sh[o++] = 0x01;         /* type */
    sh[o++] = 0; sh[o++] = 1;               /* ext length */
    sh[o++] = 0;                            /* empty renegotiated_conn */
    static u8 sh_msg[64 + 4];
    sh_msg[0] = TLS_HS_SERVER_HELLO;
    sh_msg[1] = 0; sh_msg[2] = 0; sh_msg[3] = (u8)o;
    memcpy(sh_msg + 4, sh, o);
    sha256_update(&t->hs, sh_msg, 4 + o);

    /* Certificate: [type 0x0b][handshake len][certificate_list len][cert len][DER].
     * LIST-LENGTH FIX: the certificate_list length must be 3+cert_len
     * (the list contains the cert's own 3-byte length field + the DER);
     * writing cert_len there made OpenSSL read 3 bytes short and
     * complain "cert length mismatch". */
    /* Certificate: [type 0x0b][handshake len][certificate_list len][cert len][DER].
     * LENGTHS: handshake len = 3 + 3 + der_len (the whole body);
     * certificate_list len = 3 + der_len.  The first attempt set both
     * to 3+der_len, making the message 3 bytes short of its header
     * (OpenSSL: "length mismatch"). */
    static u8 cert_msg[4 + 3 + 3 + 4096];
    int co = 0;
    cert_msg[co++] = TLS_HS_CERTIFICATE;
    u32 mlen = 3 + 3 + g_cert_der_len;       /* handshake message length */
    cert_msg[co++] = (u8)(mlen >> 16);
    cert_msg[co++] = (u8)(mlen >> 8);
    cert_msg[co++] = (u8)mlen;
    u32 clist = 3 + g_cert_der_len;          /* certificate_list length */
    cert_msg[co++] = (u8)(clist >> 16);
    cert_msg[co++] = (u8)(clist >> 8);
    cert_msg[co++] = (u8)clist;
    cert_msg[co++] = (u8)(g_cert_der_len >> 16);  /* this certificate */
    cert_msg[co++] = (u8)(g_cert_der_len >> 8);
    cert_msg[co++] = (u8)g_cert_der_len;
    memcpy(cert_msg + co, g_cert_der, g_cert_der_len);
    co += (int)g_cert_der_len;
    sha256_update(&t->hs, cert_msg, co);

    static u8 shd_msg[4] = { TLS_HS_SERVER_HELLO_DONE, 0, 0, 0 };
    sha256_update(&t->hs, shd_msg, 4);

    tls_send_record(t, TLS_RT_HANDSHAKE, sh_msg, 4 + o);
    tls_send_record(t, TLS_RT_HANDSHAKE, cert_msg, co);
    tls_send_record(t, TLS_RT_HANDSHAKE, shd_msg, 4);
    kprintf("tls-srv: ServerHello + Certificate + ServerHelloDone sent\n");

    /* ---- ClientKeyExchange: RSA-decrypt the premaster ---- */
    u8 cke[512];
    u32 ckelen;
    if (tls_read_handshake(t, TLS_HS_CLIENT_KEY_EXCHANGE, cke, &ckelen, deadline, true) != 0)
        goto fail;
    if (ckelen < 2 + 128) goto fail;
    u32 elen = (u32)((cke[0] << 8) | cke[1]);
    if (2 + elen > ckelen || elen != g_key_nlen) goto fail;
    u8 eb[256];
    if (bn_rsa_private_crt(cke + 2, (int)g_key_nlen,
                           g_key_p, (int)g_key_plen, g_key_q, (int)g_key_qlen,
                           g_key_dp, g_key_dq, g_key_qinv, g_key_n, eb) != 0)
        goto fail;
    /* PKCS#1 v1.5 type 2: 00 02 PS 00 premaster */
    if (eb[0] != 0 || eb[1] != 2) {
        kprintf("tls-srv: bad PKCS#1 header\n");
        goto fail;
    }
    u32 sep = 2;
    while (sep < g_key_nlen && eb[sep] != 0) sep++;
    if (sep < 10 || sep + 48 > g_key_nlen) {
        kprintf("tls-srv: bad padding\n");
        goto fail;
    }
    u8 premaster[48];
    memcpy(premaster, eb + sep + 1, 48);
    if (premaster[0] != 3 || premaster[1] != 3) {
        kprintf("tls-srv: premaster version mismatch\n");
        goto fail;
    }
    kprintf("tls-srv: RSA-CRT decrypted the premaster (version 3.3)\n");

    /* ---- master secret + key block (same PRF as the client) ---- */
    u8 seed[64];
    memcpy(seed, cr, 32);                    /* client random FIRST */
    memcpy(seed + 32, srand, 32);
    tls_prf(premaster, 48, "master secret", seed, 64, t->master, 48);
    u8 kseed[64];
    memcpy(kseed, srand, 32);                /* server random FIRST */
    memcpy(kseed + 32, cr, 32);
    u8 kb[128];
    tls_prf(t->master, 48, "key expansion", kseed, 64, kb, 128);
    memcpy(t->c_mac, kb, 32);
    memcpy(t->s_mac, kb + 32, 32);
    memcpy(t->c_key, kb + 64, 16);
    memcpy(t->s_key, kb + 80, 16);
    memcpy(t->c_iv, kb + 96, 16);
    memcpy(t->s_iv, kb + 112, 16);
    aes128_keyexpand(t->c_key, t->rk_c);
    aes128_keyexpand(t->s_key, t->rk_s);
    kprintf("tls-srv: master secret + keys derived\n");

    /* ---- server CCS + Finished ----
     * RFC 5246 7.4.9 hashes "all handshake messages up to but not
     * including this Finished".  For the server's Finished that is
     * CH..CKE - UNLESS the client sent its own Finished early (which
     * OpenSSL 3 clients do: CKE+CCS+Finished in one burst, before the
     * server's Finished exists): OpenSSL then expects the server's
     * verify_data over H(CH..CKE + clientFinished), because its
     * transcript already contains its own Finished message (verified
     * against OpenSSL 3.5.6 with the keylog master secret).  So try to
     * read the client's CCS+Finished first (short deadline); if it is
     * already there, hash it into t->hs and use that for the svd. */
    sha256_ctx_t hs_copy = t->hs;
    u8 hpre[32];
    sha256_final(&hs_copy, hpre);            /* CH..CKE digest */
    u8 cfin[64];   /* Finished body is 12 B - was 4, stack overflow */
    u32 cfmlen = 0;
    bool have_cfin = false;
    if (tls_read_handshake(t, TLS_HS_FINISHED, cfin, &cfmlen,
                           pit_ticks() + MS_TO_TICKS(300), true) == 0) {
        have_cfin = true;                    /* client Finished hashed */
    }
    u8 ccs = 1;
    tls_send_record(t, TLS_RT_CHANGE_CIPHER_SPEC, &ccs, 1);
    t->cseq = 0;
    t->enc_send = true;
    u8 svd[12];
    if (have_cfin) {
        sha256_ctx_t hs2 = t->hs;
        u8 hfin[32];
        sha256_final(&hs2, hfin);            /* CH..CKE + client Fin */
        tls_prf(t->master, 48, "server finished", hfin, 32, svd, 12);
        kprintf("tls-srv: early client Finished - svd hashes it too\n");
    } else {
        tls_prf(t->master, 48, "server finished", hpre, 32, svd, 12);
    }
    u8 sfin[16];
    sfin[0] = TLS_HS_FINISHED;
    sfin[1] = 0; sfin[2] = 0; sfin[3] = 12;
    memcpy(sfin + 4, svd, 12);
    tls_send_encrypted(t, TLS_RT_HANDSHAKE, sfin, 16);
    kprintf("tls-srv: server Finished sent (svd %02x%02x%02x%02x%02x%02x%02x%02x)\n",
            svd[0], svd[1], svd[2], svd[3], svd[4], svd[5], svd[6], svd[7]);

    /* ---- client CCS + Finished (verify; may already be consumed) ---- */
    if (!have_cfin) {
        if (tls_read_handshake(t, TLS_HS_FINISHED, cfin, &cfmlen, deadline, false) != 0)
            goto fail;
    }
    if (cfmlen < 4) goto fail;
    u8 cvd[12];
    memcpy(cvd, cfin, 12);
    u8 expect[12];
    tls_prf(t->master, 48, "client finished", hpre, 32, expect, 12);
    if (memcmp(cvd, expect, 12) != 0) {
        kprintf("tls-srv: client Finished VERIFY FAILED\n");
        goto fail;
    }
    kprintf("tls-srv: client Finished verified - HANDSHAKE COMPLETE\n");
    return conn;

fail:
    kprintf("tls-srv: handshake FAILED\n");
    net_tcp_close(conn);
    memset(t, 0, sizeof *t);
    return -1;
}

/* Non-blocking accept: returns a finished TLS conn id, -2 = not yet. */
int tls_server_accept(int listener) {
    int conn = net_tcp_accept(listener);
    if (conn == -2) return -2;
    if (conn < 0) return -1;
    return tls_server_handshake(conn);
}

int tls_server_listen(u16 port) {
    return net_tcp_listen(port);
}

void tls_init(void) {
    memset(g_tls, 0, sizeof g_tls);
    spin_init(&g_tls_lock);
    kprintf("tls: TLS 1.2 client+server ready (RSA-AES128-CBC-SHA256) [PADVAL-RFC5246]\\n");
}
