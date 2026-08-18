/* Yart OS - WPA2 station connection state machine (supplicant side).
 *
 * Drives: auth -> assoc -> EAPOL 4-way handshake -> CONNECTED.  The selftest
 * simulates a full AP on the other side (probe response, auth/assoc responses,
 * EAPOL Msg1/Msg3, then a CCMP-protected data frame) and asserts the whole
 * path agrees — the same "simulated AP" technique used to prove the EAPOL
 * handshake.  Radio bring-up is the driver's job; this layer is radio-free.
 */
#include <yart/types.h>
#include <yart/string.h>
#include <yart/sha1.h>
#include <yart/wpa.h>
#include <yart/ccmp.h>
#include <yart/ieee80211.h>
#include <yart/wifi_data.h>
#include <yart/wifi_sta.h>

void wifi_sta_init(wifi_sta_t *s, const u8 sta_mac[6]) {
    memset(s, 0, sizeof *s);
    memcpy(s->sta_mac, sta_mac, 6);
    s->state = WIFI_STA_DISCONNECTED;
}

void wifi_sta_set_pmk(wifi_sta_t *s, const u8 pmk[32]) {
    memcpy(s->eapol.pmk, pmk, 32);          /* stored; EAPOL is bound at join time */
    s->pmk_set = true;
}

void wifi_sta_set_passphrase(wifi_sta_t *s, const char *passphrase, const char *ssid) {
    pbkdf2_hmac_sha1(passphrase, strlen(passphrase),
                     (const u8 *)ssid, strlen(ssid), 4096, s->eapol.pmk, 32);
    s->pmk_set = true;
}

void wifi_sta_start_join(wifi_sta_t *s, const wifi_bss_t *bss) {
    memcpy(s->bssid, bss->bssid, 6);
    memcpy(s->ssid, bss->ssid, sizeof s->ssid - 1);
    s->ssid[32] = 0;
    s->channel = bss->channel;
    s->seq = 0;
    s->aid = 0;
    s->state = WIFI_STA_AUTHENTICATING;
    /* bind the EAPOL context to this BSS now that bssid is known */
    if (s->pmk_set)
        eapol_init(&s->eapol, s->eapol.pmk, s->bssid, s->sta_mac);
}

int wifi_sta_build_auth(wifi_sta_t *s, u8 *f, size_t cap) {
    if (s->state != WIFI_STA_AUTHENTICATING) return -1;
    int n = wifi_build_auth(s->bssid, s->sta_mac, ++s->seq, f, cap);
    if (n > 0) s->seq++;
    return n;
}

int wifi_sta_on_auth_resp(wifi_sta_t *s, const u8 *f, size_t len) {
    u16 status;
    if (wifi_parse_auth_resp(f, len, &status)) return -1;
    if (status != WIFI_STATUS_SUCCESS) { s->state = WIFI_STA_DISCONNECTED; return -1; }
    if (s->state == WIFI_STA_AUTHENTICATING) s->state = WIFI_STA_ASSOCIATING;
    return 0;
}

int wifi_sta_build_assoc(wifi_sta_t *s, u8 *f, size_t cap) {
    if (s->state != WIFI_STA_ASSOCIATING) return -1;
    return wifi_build_assoc_req(s->bssid, s->sta_mac, s->ssid, f, cap);
}

int wifi_sta_on_assoc_resp(wifi_sta_t *s, const u8 *f, size_t len) {
    u16 status, aid;
    if (wifi_parse_assoc_resp(f, len, &status, &aid)) return -1;
    if (status != WIFI_STATUS_SUCCESS) { s->state = WIFI_STA_DISCONNECTED; return -1; }
    s->aid = aid;
    if (s->state == WIFI_STA_ASSOCIATING) s->state = WIFI_STA_ASSOCIATED;
    return 0;
}

int wifi_sta_eapol_in(wifi_sta_t *s, const u8 *f, size_t len, u8 *reply, size_t cap) {
    if (!s->pmk_set) return -1;
    int t = eapol_key_msg_type(f, len);
    if (t == 1) {                       /* Msg1: derive PTK, answer Msg2 */
        if (eapol_process_msg1(&s->eapol, f, len)) return -1;
        return eapol_build_msg2(&s->eapol, reply, cap);
    }
    if (t == 3) {                       /* Msg3: verify, unwrap GTK, answer Msg4 */
        if (eapol_process_msg3(&s->eapol, f, len)) return -1;
        memcpy(s->tk, s->eapol.ptk + 32, 16);
        s->tk_installed = true;
        s->state = WIFI_STA_CONNECTED;
        return eapol_build_msg4(&s->eapol, reply, cap);
    }
    return 0;
}

bool wifi_sta_connected(const wifi_sta_t *s) { return s->state == WIFI_STA_CONNECTED; }

/* ---------- selftest: full AP<->STA session ---------- */
typedef struct {
    u8 pmk[32], aa[6], spa[6];
    u8 anonce[32], snonce[32], ptk[48];
    u64 rctr;
    u8 gtk[16];
    u8 pn[6];
    u16 data_seq;
} ap_t;

static void st_put16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static void st_put64(u8 *p, u64 v) { for (int i = 7; i >= 0; i--) { p[i] = (u8)v; v >>= 8; } }
static void st_put_le16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }   /* 802.11 fields are LE */

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

int wifi_sta_selftest(void) {
    static const u8 pmk[32] = {
        0xf4,0x2c,0x6f,0xc5,0x2d,0xf0,0xeb,0xef,0x9e,0xbb,0x4b,0x90,0xb3,0x8a,0x5f,0x90,
        0x2e,0x83,0xfe,0x1b,0x13,0x5a,0x70,0xe2,0x3a,0xed,0x76,0x2e,0x97,0x10,0xa1,0x2e };
    static const u8 bssid[6] = {0x02,0x11,0x22,0x33,0x44,0x55};
    static const u8 sta[6]   = {0x02,0x66,0x77,0x88,0x99,0xaa};
    u8 f[512];
    int n;

    /* --- AP context --- */
    ap_t ap;
    memset(&ap, 0, sizeof ap);
    memcpy(ap.pmk, pmk, 32);
    memcpy(ap.aa, bssid, 6);
    memcpy(ap.spa, sta, 6);
    for (int i = 0; i < 32; i++) ap.anonce[i] = (u8)(0x11 + i);
    for (int i = 0; i < 16; i++) ap.gtk[i] = (u8)(0x44 + i);
    ap.rctr = 1;
    ap.pn[0] = 1;
    ap.data_seq = 0x10;

    /* --- STA context --- */
    wifi_sta_t s;
    wifi_sta_init(&s, sta);
    wifi_sta_set_pmk(&s, pmk);

    /* 1. probe response (hand-packed), STA joins */
    {
        static const u8 resp[] = {
            0x50,0x00, 0x00,0x00,
            0x02,0x66,0x77,0x88,0x99,0xaa, 0x02,0x11,0x22,0x33,0x44,0x55,
            0x02,0x11,0x22,0x33,0x44,0x55, 0x00,0x00,
            0,0,0,0,0,0,0,0, 0x64,0x00, 0x31,0x04,
            0x00,0x07,'Y','a','r','t','N','e','t',
            0x01,0x04,0x82,0x84,0x8b,0x96,
            0x03,0x01,0x06,
            0x30,0x14,0x01,0x00,0x00,0x0f,0xac,0x04,0x01,0x00,0x00,0x0f,0xac,0x04,0x01,0x00,0x00,0x0f,0xac,0x02,0x00,0x00 };
        wifi_bss_t b;
        if (wifi_parse_probe_resp(resp, sizeof resp, &b)) return 1;
        if (strcmp(b.ssid, "YartNet") || !b.has_rsn || b.rsn_cipher != 4) return 2;
        wifi_sta_start_join(&s, &b);
        if (s.state != WIFI_STA_AUTHENTICATING) return 3;
    }

    /* 2. auth exchange */
    n = wifi_sta_build_auth(&s, f, sizeof f);
    if (n < 0) return 4;
    {
        u8 r[30];
        memcpy(r, f, 30);
        memcpy(r + 4, sta, 6);
        st_put16(r + 28, 0);
        if (wifi_sta_on_auth_resp(&s, r, 30)) return 5;
        if (s.state != WIFI_STA_ASSOCIATING) return 6;
    }

    /* 3. assoc exchange */
    n = wifi_sta_build_assoc(&s, f, sizeof f);
    if (n < 0) return 7;
    {
        u8 r[32];
        memset(r, 0, sizeof r);
        memcpy(r, f, 24);
        st_put_le16(r + 0, 0x0010);          /* 802.11 FC is little-endian */
        memcpy(r + 4, sta, 6);
        memcpy(r + 16, bssid, 6);
        st_put_le16(r + 24, 0x0431);
        st_put_le16(r + 26, 0);
        st_put_le16(r + 28, 0xc001);
        if (wifi_sta_on_assoc_resp(&s, r, 30)) return 8;
        if (s.state != WIFI_STA_ASSOCIATED || s.aid != 0xc001) return 9;
    }

    /* 4. 4-way handshake */
    {
        /* Msg1 */
        memset(f, 0, 95);
        f[EAPOL_DESC] = 2;
        st_put16(f + EAPOL_KEYINFO, WPA_KI_VERSION | WPA_KI_TYPE | WPA_KI_ACK);
        st_put16(f + EAPOL_KEYLEN, 16);
        st_put64(f + EAPOL_REPLAY, ap.rctr);
        memcpy(f + EAPOL_NONCE, ap.anonce, 32);
        n = wifi_sta_eapol_in(&s, f, 95, f, sizeof f);
        if (n <= 0) return 10;
        memcpy(ap.snonce, f + EAPOL_NONCE, 32);
        wpa_ptk_derive(ap.pmk, ap.aa, ap.spa, ap.anonce, ap.snonce, ap.ptk);
        if (ap_verify_mic(ap.ptk, f, n)) return 11;

        /* Msg3 */
        {
            int len = 95 + 24;
            memset(f, 0, 95);
            f[EAPOL_DESC] = 2;
            st_put16(f + EAPOL_KEYINFO, WPA_KI_VERSION | WPA_KI_TYPE | WPA_KI_ACK |
                                      WPA_KI_MIC | WPA_KI_INSTALL | WPA_KI_ENCR | WPA_KI_SECURE);
            st_put16(f + EAPOL_KEYLEN, 16);
            ap.rctr++;
            st_put64(f + EAPOL_REPLAY, ap.rctr);
            memcpy(f + EAPOL_NONCE, ap.anonce, 32);
            st_put16(f + EAPOL_DLEN, 24);
            aes_key_wrap(ap.ptk + 16, ap.gtk, 2, f + EAPOL_DATA);
            u8 tmp[256], mic[16];
            memcpy(tmp, f, len);
            memset(tmp + EAPOL_MIC, 0, 16);
            wpa_eapol_mic(ap.ptk, tmp, len, mic);
            memcpy(f + EAPOL_MIC, mic, 16);
            n = wifi_sta_eapol_in(&s, f, len, f, sizeof f);
            if (n <= 0) return 12;
            if (!wifi_sta_connected(&s) || !s.tk_installed) return 13;
            if (ap_verify_mic(ap.ptk, f, n)) return 14;
            if (memcmp(s.eapol.gtk, ap.gtk, 16)) return 15;
        }
    }

    /* 5. protected data frame AP -> STA */
    {
        u8 eth[14 + 20];
        memset(eth, 0, sizeof eth);
        eth[0]=0xc0;eth[1]=0x01;eth[2]=0x02;eth[3]=0x03;eth[4]=0x04;eth[5]=0x05;
        eth[6]=0xa0;eth[7]=0x01;eth[8]=0x02;eth[9]=0x03;eth[10]=0x04;eth[11]=0x05;
        eth[12]=0x08;eth[13]=0x00;
        for (int i = 0; i < 20; i++) eth[14 + i] = (u8)(0x40 + i);

        n = wifi_data_encrypt(f, sizeof f, ap.ptk + 32, ap.pn, ap.data_seq,
                              bssid, sta, false, eth, sizeof eth);
        if (n < 0) return 16;
        u8 dst[6], src[6]; u16 etype = 0;
        if (wifi_data_decrypt(s.tk, f, n, dst, src, &etype)) return 17;
        if (memcmp(dst, sta, 6) || memcmp(src, eth + 6, 6) || etype != 0x0800) return 18;
        if (memcmp(f + WIFI_HDRLEN + WIFI_CCMP_HDRLEN + 8, eth + 14, 20)) return 19;
    }

    return 0;
}
