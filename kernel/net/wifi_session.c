/* YartOS Wi-Fi session orchestrator.
 *
 * Drives the 802.11 station state machine through an abstract frame
 * transport: scan (probe) -> auth -> assoc -> EAPOL 4-way handshake ->
 * CCMP data.  EAPOL runs over plaintext 802.11 data frames (LLC/SNAP with
 * EtherType 0x888E + an 802.1X header).
 *
 * The selftest runs a COMPLETE session against a simulated AP implemented in
 * this file with the same crypto primitives (but built independently of the
 * station code path), proving scan+join+handshake+data end-to-end.
 */
#include <yart/types.h>
#include <yart/string.h>
#include <yart/wifi_session.h>
#include <yart/wifi_data.h>
#include <yart/eapol.h>
#include <yart/sha1.h>
#include <yart/wpa.h>

/* ---- big-endian field helpers (802.1X / EAPOL-Key are BE) ---- */
static void put_be16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static void put_be64(u8 *p, u64 v) { for (int i = 7; i >= 0; i--) { p[i] = (u8)v; v >>= 8; } }

/* ---- plaintext 802.11 data frame codec (EAPOL rides these) ---- */
#define ETH_P_EAPOL 0x888E

static int data_build(u8 *out, size_t cap, u16 fc_extra,
                      const u8 a1[6], const u8 a2[6], const u8 a3[6],
                      u16 etype, const u8 *payload, size_t plen) {
    if (cap < 32 + plen) return -1;
    u16 fc = 0x0008 | fc_extra;              /* Data, subtype 0 */
    out[0] = (u8)fc; out[1] = (u8)(fc >> 8);
    out[2] = 0; out[3] = 0;
    memcpy(out + 4,  a1, 6);
    memcpy(out + 10, a2, 6);
    memcpy(out + 16, a3, 6);
    out[22] = 0; out[23] = 0;
    out[24] = 0xAA; out[25] = 0xAA; out[26] = 0x03;   /* LLC/SNAP */
    out[27] = 0x00; out[28] = 0x00; out[29] = 0x00;
    out[30] = (u8)(etype >> 8); out[31] = (u8)etype;
    memcpy(out + 32, payload, plen);
    return (int)(32 + plen);
}

/* Build a plaintext 802.11 data frame carrying an EAPOL-Key descriptor. */
static int eapol_build(u8 *out, size_t cap, const u8 bssid[6], const u8 sta[6],
                       bool to_ds, const u8 *kd, size_t kl) {
    if (cap < 36 + kl) return -1;
    u8 *p = out;
    int n;
    if (to_ds)
        n = data_build(p, cap, 0x0100, bssid, sta, bssid, ETH_P_EAPOL, NULL, 0);
    else
        n = data_build(p, cap, 0x0200, sta, bssid, bssid, ETH_P_EAPOL, NULL, 0);
    if (n < 0) return -1;
    /* 802.1X header: version 1, type 3 (EAPOL-Key), BE length */
    p[32] = 0x01; p[33] = 0x03;
    p[34] = (u8)(kl >> 8); p[35] = (u8)kl;
    memcpy(p + 36, kd, kl);
    return (int)(36 + kl);
}

/* Parse a plaintext 802.11 data frame carrying EAPOL; returns the key desc. */
static int eapol_parse(const u8 *f, size_t len, bool *to_ds,
                       const u8 **kd, size_t *kl) {
    if (len < 36) return -1;
    u16 fc = (u16)(f[0] | (f[1] << 8));
    if (((fc >> 2) & 3) != 2) return -1;         /* must be a data frame */
    if (to_ds) *to_ds = (fc & 0x0100) != 0;
    if (f[24] != 0xAA || f[25] != 0xAA || f[26] != 0x03) return -1;
    if (f[30] != 0x88 || f[31] != 0x8E) return -1;
    if (f[32] != 0x01 || f[33] != 0x03) return -1;   /* 802.1X EAPOL-Key */
    size_t k = ((size_t)f[34] << 8) | f[35];
    if (36 + k > len) return -1;
    *kd = f + 36;
    *kl = k;
    return 0;
}

/* ---- session ---- */
void wifi_session_init(wifi_session_t *s, const wifi_transport_t *tr,
                       void *ctx, const u8 sta_mac[6]) {
    memset(s, 0, sizeof *s);
    s->tr = tr;
    s->tr_ctx = ctx;
    wifi_sta_init(&s->sta, sta_mac);
}

static int sess_send(wifi_session_t *s, const u8 *f, u32 len) {
    return s->tr->tx(s->tr_ctx, f, len);
}

static int sess_recv_until(wifi_session_t *s, u8 *f, u32 cap, u32 *len,
                           u32 timeout_ms) {
    u32 waited = 0;
    while (waited < timeout_ms) {
        int r = s->tr->rx(s->tr_ctx, f, cap, len);
        if (r == 1) return 1;
        if (r < 0) return -1;
        s->tr->delay_ms(s->tr_ctx, 10);
        waited += 10;
    }
    return 0;
}

int wifi_session_scan(wifi_session_t *s, wifi_bss_t *results, int max,
                      u32 timeout_ms) {
    u8 f[512];
    static const u8 bc[6] = {0xff,0xff,0xff,0xff,0xff,0xff};
    int n = wifi_build_probe_req(bc, NULL, f, sizeof f);
    if (n < 0) return -1;
    memcpy(f + 10, s->sta.sta_mac, 6);   /* addr2 = our MAC */
    if (sess_send(s, f, (u32)n)) return -1;

    int found = 0;
    u32 waited = 0;
    while (found < max && waited < timeout_ms) {
        u32 fl;
        if (sess_recv_until(s, f, sizeof f, &fl, 200) == 1) {
            wifi_bss_t b;
            if (!wifi_parse_probe_resp(f, fl, &b))
                results[found++] = b;
        }
        waited += 200;
    }
    return found;
}

int wifi_session_join(wifi_session_t *s, const wifi_bss_t *bss,
                      const char *passphrase) {
    u8 f[2048]; u32 fl = 0;
    int n;

    wifi_sta_set_passphrase(&s->sta, passphrase, bss->ssid);
    wifi_sta_start_join(&s->sta, bss);

    /* auth */
    n = wifi_sta_build_auth(&s->sta, f, sizeof f);
    if (n < 0 || sess_send(s, f, (u32)n)) return -1;
    if (sess_recv_until(s, f, sizeof f, &fl, 2000) != 1) return -1;
    if (wifi_sta_on_auth_resp(&s->sta, f, fl)) return -1;

    /* assoc */
    n = wifi_sta_build_assoc(&s->sta, f, sizeof f);
    if (n < 0 || sess_send(s, f, (u32)n)) return -1;
    if (sess_recv_until(s, f, sizeof f, &fl, 2000) != 1) return -1;
    if (wifi_sta_on_assoc_resp(&s->sta, f, fl)) return -1;

    /* 4-way handshake */
    for (int round = 0; round < 4; round++) {
        if (sess_recv_until(s, f, sizeof f, &fl, 2000) != 1) return -1;
        const u8 *kd; size_t kl; bool to_ds;
        if (eapol_parse(f, fl, &to_ds, &kd, &kl)) return -1;
        u8 reply[512];
        int r = wifi_sta_eapol_in(&s->sta, kd, kl, reply, sizeof reply);
        if (r < 0) return -1;
        if (r > 0) {
            int wl = eapol_build(f, sizeof f, s->sta.bssid, s->sta.sta_mac,
                                 true, reply, (size_t)r);
            if (wl < 0 || sess_send(s, f, (u32)wl)) return -1;
        }
        if (wifi_sta_connected(&s->sta)) break;
    }
    if (!wifi_sta_connected(&s->sta)) return -1;
    return 0;
}

int wifi_session_send_data(wifi_session_t *s, const u8 *eth, size_t eth_len) {
    u8 f[2048];
    int n = wifi_data_encrypt(f, sizeof f, s->sta.tk, s->pn_tx, s->seq_tx++,
                              s->sta.bssid, s->sta.sta_mac, true, eth, eth_len);
    if (n < 0) return -1;
    return sess_send(s, f, (u32)n);
}

int wifi_session_recv_data(wifi_session_t *s, u8 *eth_dst, u8 *eth_src,
                           u16 *ethertype, u8 *payload, size_t cap,
                           size_t *plen) {
    u8 f[2048]; u32 fl;
    if (s->tr->rx(s->tr_ctx, f, sizeof f, &fl) != 1) return 0;
    u16 fc = (u16)(f[0] | (f[1] << 8));
    if (((fc >> 2) & 3) != 2) return 0;          /* not a data frame */
    if (wifi_data_decrypt(s->sta.tk, f, fl, eth_dst, eth_src, ethertype)) return -1;
    size_t pl = fl - (WIFI_HDRLEN + WIFI_CCMP_HDRLEN + WIFI_SNAP_HDRLEN + WIFI_CCMP_MICLEN);
    if (pl > cap) return -1;
    memcpy(payload, f + WIFI_HDRLEN + WIFI_CCMP_HDRLEN + WIFI_SNAP_HDRLEN, pl);
    if (plen) *plen = pl;
    return 1;
}

bool wifi_session_connected(const wifi_session_t *s) {
    return wifi_sta_connected(&s->sta);
}

/* ===================== selftest: simulated AP =====================
 * The AP is a stateful frame-in/frame-out transform built from the SAME
 * crypto primitives but independently of the station code path. */
typedef struct {
    u8   bssid[6];
    u8   sta_mac[6];
    u8   pmk[32], anonce[32], snonce[32], ptk[48], gtk[16];
    u64  rctr;
    bool hsk_done;
    u8   pn[6];
    u16  data_seq;
    u8   out[2048]; u32 out_len; bool out_pending;
    bool m1_pending;
    u8   last_rx[2048]; u32 last_rx_len; bool got_data;
} ap_t;

static int ap_verify_mic(const u8 *kck, const u8 *f, size_t len) {
    u8 tmp[256], expect[16], got[16];
    memcpy(tmp, f, len);
    memset(tmp + EAPOL_MIC, 0, 16);
    memcpy(expect, f + EAPOL_MIC, 16);
    wpa_eapol_mic(kck, tmp, len, got);
    u8 d = 0;
    for (int i = 0; i < 16; i++) d |= expect[i] ^ got[i];
    return d ? -1 : 0;
}

static void ap_probe_resp(ap_t *ap, const u8 *f) {
    (void)f;
    u8 r[128]; u32 o = 0;
    r[o++]=0x50; r[o++]=0x00; r[o++]=0; r[o++]=0;
    memcpy(r+o, ap->sta_mac, 6); o+=6;
    memcpy(r+o, ap->bssid, 6); o+=6;
    memcpy(r+o, ap->bssid, 6); o+=6;
    r[o++]=0; r[o++]=0;
    for (int i=0;i<8;i++) r[o++]=0;         /* timestamp */
    r[o++]=0x64; r[o++]=0x00;               /* beacon interval 100 */
    r[o++]=0x31; r[o++]=0x04;               /* capability 0x0431 */
    r[o++]=0x00; r[o++]=7; memcpy(r+o, "YartNet", 7); o+=7;
    r[o++]=0x01; r[o++]=4; r[o++]=0x82; r[o++]=0x84; r[o++]=0x8b; r[o++]=0x96;
    r[o++]=0x03; r[o++]=1; r[o++]=6;        /* DS channel 6 */
    r[o++]=0x30; r[o++]=0x14;
    static const u8 rsn[20] = {0x01,0x00,0x00,0x0f,0xac,0x04,0x01,0x00,0x00,0x0f,0xac,0x04,0x01,0x00,0x00,0x0f,0xac,0x02,0x00,0x00};
    memcpy(r+o, rsn, 20); o+=20;
    memcpy(ap->out, r, o); ap->out_len = o; ap->out_pending = true;
}

static void ap_auth_resp(ap_t *ap, const u8 *f) {
    memcpy(ap->sta_mac, f + 10, 6);
    u8 r[30];
    memcpy(r, f, 30);
    r[0] = 0xb0; r[1] = 0x00;               /* auth resp */
    memcpy(r+4, f+10, 6);                   /* addr1 = sta */
    memcpy(r+10, ap->bssid, 6);
    memcpy(r+16, ap->bssid, 6);
    r[28] = 0; r[29] = 0;                   /* status success */
    memcpy(ap->out, r, 30); ap->out_len = 30; ap->out_pending = true;
}

static void ap_assoc_resp(ap_t *ap, const u8 *f) {
    u8 r[30];
    memcpy(r, f, 24);
    r[0] = 0x10; r[1] = 0x00;               /* assoc resp */
    memcpy(r+4, f+10, 6);                   /* addr1 = sta */
    memcpy(r+10, ap->bssid, 6);
    memcpy(r+16, ap->bssid, 6);
    r[22] = 0; r[23] = 0;
    r[24]=0x31; r[25]=0x04;                 /* capability */
    r[26]=0; r[27]=0;                       /* status success */
    r[28]=0x01; r[29]=0xc0;                 /* AID 1 */
    memcpy(ap->out, r, 30); ap->out_len = 30; ap->out_pending = true;
    ap->m1_pending = true;
}

static void ap_queue_m1(ap_t *ap) {
    u8 kd[95];
    memset(kd, 0, sizeof kd);
    kd[EAPOL_DESC] = 2;
    put_be16(kd + EAPOL_KEYINFO, WPA_KI_VERSION | WPA_KI_TYPE | WPA_KI_ACK);
    put_be16(kd + EAPOL_KEYLEN, 16);
    put_be64(kd + EAPOL_REPLAY, ap->rctr);
    memcpy(kd + EAPOL_NONCE, ap->anonce, 32);
    int n = eapol_build(ap->out, sizeof ap->out, ap->bssid, ap->sta_mac, false, kd, sizeof kd);
    if (n > 0) { ap->out_len = (u32)n; ap->out_pending = true; }
}

static int ap_send_m3(ap_t *ap) {
    u8 kd[95 + 24];
    memset(kd, 0, sizeof kd);
    kd[EAPOL_DESC] = 2;
    put_be16(kd + EAPOL_KEYINFO, WPA_KI_VERSION | WPA_KI_TYPE | WPA_KI_ACK |
                                 WPA_KI_MIC | WPA_KI_INSTALL | WPA_KI_ENCR | WPA_KI_SECURE);
    put_be16(kd + EAPOL_KEYLEN, 16);
    ap->rctr++;
    put_be64(kd + EAPOL_REPLAY, ap->rctr);
    memcpy(kd + EAPOL_NONCE, ap->anonce, 32);
    put_be16(kd + EAPOL_DLEN, 24);
    aes_key_wrap(ap->ptk + 16, ap->gtk, 2, kd + EAPOL_DATA);
    u8 tmp[256], mic[16];
    memcpy(tmp, kd, sizeof kd);
    memset(tmp + EAPOL_MIC, 0, 16);
    wpa_eapol_mic(ap->ptk, tmp, sizeof kd, mic);
    memcpy(kd + EAPOL_MIC, mic, 16);
    int n = eapol_build(ap->out, sizeof ap->out, ap->bssid, ap->sta_mac, false, kd, sizeof kd);
    if (n <= 0) return -1;
    ap->out_len = (u32)n; ap->out_pending = true;
    return 0;
}

static int ap_eapol(ap_t *ap, const u8 *kd, size_t kl) {
    int t = eapol_key_msg_type(kd, kl);
    if (t == 2) {                            /* Msg2 */
        memcpy(ap->snonce, kd + EAPOL_NONCE, 32);
        wpa_ptk_derive(ap->pmk, ap->bssid, ap->sta_mac, ap->anonce, ap->snonce, ap->ptk);
        if (ap_verify_mic(ap->ptk, kd, kl)) return -1;
        return ap_send_m3(ap);
    }
    if (t == 4) {                            /* Msg4 */
        if (ap_verify_mic(ap->ptk, kd, kl)) return -1;
        ap->hsk_done = true;
    }
    return 0;
}

static int ap_data(ap_t *ap, const u8 *f, u32 len) {
    u8 buf[2048];
    if (len > sizeof buf) return -1;
    memcpy(buf, f, len);
    u8 dst[6], src[6]; u16 et;
    if (wifi_data_decrypt(ap->ptk + 32, buf, len, dst, src, &et)) return -1;
    (void)dst; (void)src;
    size_t pl = len - (WIFI_HDRLEN + WIFI_CCMP_HDRLEN + WIFI_SNAP_HDRLEN + WIFI_CCMP_MICLEN);
    if (pl > sizeof ap->last_rx) return -1;
    memcpy(ap->last_rx, buf + WIFI_HDRLEN + WIFI_CCMP_HDRLEN + WIFI_SNAP_HDRLEN, pl);
    ap->last_rx_len = (u32)pl;
    ap->got_data = true;
    return 0;
}

static void ap_send_data(ap_t *ap, const u8 *eth, size_t eth_len) {
    int n = wifi_data_encrypt(ap->out, sizeof ap->out, ap->ptk + 32, ap->pn,
                              ap->data_seq++, ap->bssid, ap->sta_mac, false,
                              eth, eth_len);
    if (n > 0) { ap->out_len = (u32)n; ap->out_pending = true; }
}

static int ap_handle(ap_t *ap, const u8 *f, u32 len) {
    if (len < 24) return 0;
    u16 fc = (u16)(f[0] | (f[1] << 8));
    u8 type = (u8)((fc >> 2) & 3), subtype = (u8)((fc >> 4) & 0xf);
    if (type == 0) {                         /* management */
        if (subtype == WIFI_ST_PROBE_REQ) ap_probe_resp(ap, f);
        else if (subtype == WIFI_ST_AUTH)   ap_auth_resp(ap, f);
        else if (subtype == WIFI_ST_ASSOC_REQ) ap_assoc_resp(ap, f);
        return 0;
    }
    if (type == 2) {                         /* data */
        const u8 *kd; size_t kl; bool to_ds;
        if (!eapol_parse(f, len, &to_ds, &kd, &kl)) return ap_eapol(ap, kd, kl);
        return ap_data(ap, f, len);
    }
    return 0;
}

static int ap_tx(void *ctx, const u8 *f, u32 len) { return ap_handle((ap_t *)ctx, f, len); }
static int ap_rx(void *ctx, u8 *f, u32 cap, u32 *len) {
    ap_t *ap = (ap_t *)ctx;
    if (!ap->out_pending) {
        if (ap->m1_pending) { ap->m1_pending = false; ap_queue_m1(ap); }
        else return 0;
    }
    u32 n = ap->out_len < cap ? ap->out_len : cap;
    memcpy(f, ap->out, n);
    *len = n;
    ap->out_pending = false;
    return 1;
}
static void ap_delay(void *ctx, u32 ms) { (void)ctx; (void)ms; }

int wifi_session_selftest(void) {
    static const u8 sta_mac[6] = {0x02,0x66,0x77,0x88,0x99,0xaa};
    ap_t ap;
    memset(&ap, 0, sizeof ap);
    memcpy(ap.bssid, "\x02\x11\x22\x33\x44\x55", 6);
    pbkdf2_hmac_sha1("yartpass", 8, (const u8 *)"YartNet", 7, 4096, ap.pmk, 32);
    for (int i = 0; i < 32; i++) ap.anonce[i] = (u8)(0x11 + i);
    for (int i = 0; i < 16; i++) ap.gtk[i]   = (u8)(0x44 + i);
    ap.rctr = 1;
    ap.pn[0] = 1;
    ap.data_seq = 0x10;

    static const wifi_transport_t tr = {
        .tx = ap_tx, .rx = ap_rx, .delay_ms = ap_delay
    };
    wifi_session_t s;
    wifi_session_init(&s, &tr, &ap, sta_mac);

    /* 1. scan finds the AP with WPA2 */
    wifi_bss_t results[4];
    if (wifi_session_scan(&s, results, 4, 1500) != 1) return 1;
    if (strcmp(results[0].ssid, "YartNet") || !results[0].has_rsn ||
        results[0].rsn_cipher != 4) return 2;

    /* 2. join: auth -> assoc -> 4-way handshake */
    if (wifi_session_join(&s, &results[0], "yartpass")) return 3;
    if (!wifi_session_connected(&s)) return 4;
    if (!ap.hsk_done) return 5;

    /* 3. AP -> STA CCMP data */
    u8 eth[34];
    memset(eth, 0, sizeof eth);
    eth[0]=0xc0; eth[1]=0x01; eth[2]=0x02; eth[3]=0x03; eth[4]=0x04; eth[5]=0x05;
    eth[6]=0xa0; eth[7]=0x01; eth[8]=0x02; eth[9]=0x03; eth[10]=0x04; eth[11]=0x05;
    eth[12]=0x08; eth[13]=0x00;
    for (int i = 0; i < 20; i++) eth[14+i] = (u8)(0x40 + i);

    ap_send_data(&ap, eth, sizeof eth);
    u8 dst[6], src[6]; u16 et = 0; u8 payload[64]; size_t pl = 0;
    if (wifi_session_recv_data(&s, dst, src, &et, payload, sizeof payload, &pl) != 1) return 6;
    if (memcmp(dst, sta_mac, 6) || et != 0x0800 || pl != 20) return 7;
    if (memcmp(payload, eth + 14, 20)) return 8;

    /* 4. STA -> AP CCMP data */
    if (wifi_session_send_data(&s, eth, sizeof eth)) return 9;
    if (!ap.got_data || ap.last_rx_len != 20) return 10;
    if (memcmp(ap.last_rx, eth + 14, 20)) return 11;

    return 0;
}
