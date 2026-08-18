/* Yart OS - minimal IEEE 802.11 frame codecs (STA side).
 *
 * This is the management-plane foundation for the hand-rolled 802.11 core:
 * probe-request / auth / assoc-request builders and beacon / probe-response /
 * auth-response / assoc-response parsers.  The selftest hand-packs frames
 * byte-by-byte (independent of the builders) so the parser is not tested
 * circularly, and verifies the builders against hand-computed bytes.
 */
#include <yart/types.h>
#include <yart/string.h>
#include <yart/ieee80211.h>

static u16 le16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static void put_le16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }

static u16 mgmt_fc(u8 subtype) { return (u16)((subtype << 4) | (WIFI_FC_TYPE_MGMT << 2)); }

static void mgmt_hdr(u8 *f, u16 fc, const u8 da[6], const u8 sa[6], const u8 bssid[6]) {
    put_le16(f + 0, fc);
    put_le16(f + 2, 0);
    memcpy(f + 4, da, 6);
    memcpy(f + 10, sa, 6);
    memcpy(f + 16, bssid, 6);
    put_le16(f + 22, 0);
}

/* standard 802.11b/g basic rates (1, 2, 5.5, 11 Mb/s) with the basic-rate bit */
static const u8 basic_rates[4] = { 0x82, 0x84, 0x8b, 0x96 };

/* RSN IE advertising CCMP pairwise + PSK AKM (WPA2-PSK). */
static const u8 rsn_ie[22] = {
    0x30, 0x14, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04, 0x01, 0x00, 0x00,
    0x0f, 0xac, 0x04, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x02, 0x00, 0x00 };

int wifi_build_probe_req(const u8 bssid[6], const char *ssid, u8 *f, size_t cap) {
    size_t slen = ssid ? strlen(ssid) : 0;
    if (slen > 32) slen = 32;
    size_t len = 24 + 2 + slen + 2 + 4;     /* hdr + SSID IE + rates IE */
    if (cap < len) return -1;
    memset(f, 0, len);
    mgmt_hdr(f, mgmt_fc(WIFI_ST_PROBE_REQ), bssid, bssid, bssid);
    size_t p = 24;
    f[p++] = WIFI_EID_SSID; f[p++] = (u8)slen;              /* wildcard if slen==0 */
    memcpy(f + p, ssid, slen); p += slen;
    f[p++] = WIFI_EID_RATES; f[p++] = 4;
    memcpy(f + p, basic_rates, 4); p += 4;
    return (int)p;
}

int wifi_build_auth(const u8 bssid[6], const u8 sta[6], u16 seq, u8 *f, size_t cap) {
    size_t len = 24 + 6;                    /* algo + seq + status */
    if (cap < len) return -1;
    memset(f, 0, len);
    mgmt_hdr(f, mgmt_fc(WIFI_ST_AUTH), bssid, sta, bssid);
    put_le16(f + 24, WIFI_AUTH_ALG_OPEN);
    put_le16(f + 26, seq);
    put_le16(f + 28, WIFI_STATUS_SUCCESS);
    return (int)len;
}

int wifi_build_assoc_req(const u8 bssid[6], const u8 sta[6], const char *ssid,
                         u8 *f, size_t cap) {
    size_t slen = ssid ? strlen(ssid) : 0;
    if (slen > 32) slen = 32;
    size_t len = 24 + 4 + (2 + slen) + (2 + 4) + 22;   /* hdr+cap/listen + SSID + rates + RSN */
    if (cap < len) return -1;
    memset(f, 0, len);
    mgmt_hdr(f, mgmt_fc(WIFI_ST_ASSOC_REQ), bssid, sta, bssid);
    put_le16(f + 24, 0x0431);             /* ESS + short preamble + short slot */
    put_le16(f + 26, 10);                 /* listen interval */
    size_t p = 28;
    f[p++] = WIFI_EID_SSID; f[p++] = (u8)slen;
    memcpy(f + p, ssid, slen); p += slen;
    f[p++] = WIFI_EID_RATES; f[p++] = 4;
    memcpy(f + p, basic_rates, 4); p += 4;
    memcpy(f + p, rsn_ie, 22); p += 22;
    return (int)p;
}

/* ---- parsers ---- */

static int parse_ies(const u8 *p, size_t len, wifi_bss_t *b) {
    size_t i = 0;
    while (i + 2 <= len) {
        u8 id = p[i], elen = p[i + 1];
        if (i + 2 + elen > len) return -1;
        const u8 *d = p + i + 2;
        if (id == WIFI_EID_SSID) {
            int n = elen > 32 ? 32 : elen;
            memcpy(b->ssid, d, n);
            b->ssid[n] = 0;
            b->ssid_len = n;
        } else if (id == WIFI_EID_DS && elen >= 1) {
            b->channel = d[0];
        } else if (id == WIFI_EID_RATES) {
            int n = elen > 8 ? 8 : elen;
            memcpy(b->rates, d, n);
            b->n_rates = n;
        } else if (id == WIFI_EID_RSN) {
            b->has_rsn = true;
            /* pairwise cipher = IE data[8..11] (00 0f ac 04 = CCMP) */
            if (elen >= 12 && d[8] == 0x00 && d[9] == 0x0f && d[10] == 0xac && d[11] == 0x04)
                b->rsn_cipher = 4;
        }
        i += 2 + elen;
    }
    return 0;
}

static int parse_mgmt_body(const u8 *f, size_t len, size_t fixed, wifi_bss_t *b) {
    const wifi_mgmt_hdr_t *h = (const wifi_mgmt_hdr_t *)f;
    if (len < 24 + fixed) return -1;
    memcpy(b->bssid, h->addr3, 6);
    b->ssid_len = 0; b->ssid[0] = 0;
    b->channel = 0; b->n_rates = 0; b->has_rsn = false; b->rsn_cipher = 0;
    return parse_ies(f + 24 + fixed, len - 24 - fixed, b);
}

int wifi_parse_beacon(const u8 *f, size_t len, wifi_bss_t *b) {
    if (len < 24 || le16(f) != mgmt_fc(WIFI_ST_BEACON)) return -1;
    return parse_mgmt_body(f, len, 12, b);      /* timestamp + interval + capability */
}

int wifi_parse_probe_resp(const u8 *f, size_t len, wifi_bss_t *b) {
    if (len < 24 || le16(f) != mgmt_fc(WIFI_ST_PROBE_RESP)) return -1;
    return parse_mgmt_body(f, len, 12, b);
}

int wifi_parse_auth_resp(const u8 *f, size_t len, u16 *status) {
    if (len < 30 || le16(f) != mgmt_fc(WIFI_ST_AUTH)) return -1;
    if (status) *status = le16(f + 28);
    return 0;
}

int wifi_parse_assoc_resp(const u8 *f, size_t len, u16 *status, u16 *aid) {
    if (len < 30 || le16(f) != mgmt_fc(WIFI_ST_ASSOC_RESP)) return -1;
    if (status) *status = le16(f + 26);
    if (aid) *aid = le16(f + 28);
    return 0;
}

/* ---- selftest ---- */
int ieee80211_selftest(void) {
    static const u8 bssid[6] = {0x02,0x11,0x22,0x33,0x44,0x55};
    static const u8 sta[6]   = {0x02,0x66,0x77,0x88,0x99,0xaa};
    u8 f[256];
    int n;

    /* 1. probe request: hand-computed bytes */
    n = wifi_build_probe_req(bssid, "YartOS", f, sizeof f);
    if (n != 24 + 2 + 6 + 2 + 4) return 1;
    if (le16(f) != 0x0040) return 2;                  /* mgmt, subtype probe req */
    if (memcmp(f + 4, bssid, 6) || memcmp(f + 16, bssid, 6)) return 3;
    if (f[24] != 0x00 || f[25] != 6 || memcmp(f + 26, "YartOS", 6)) return 4;
    if (f[32] != 0x01 || f[33] != 4) return 5;

    /* 2. probe request: wildcard scan (empty SSID) */
    n = wifi_build_probe_req(bssid, NULL, f, sizeof f);
    if (n != 24 + 2 + 0 + 2 + 4) return 6;
    if (f[25] != 0) return 7;

    /* 3. parse a hand-packed probe response (independent of the builders) */
    {
        static const u8 resp[] = {
            0x50, 0x00, 0x00, 0x00,                 /* fc=0x0050, duration */
            0x02,0x66,0x77,0x88,0x99,0xaa,          /* addr1 = sta */
            0x02,0x11,0x22,0x33,0x44,0x55,          /* addr2 = bssid */
            0x02,0x11,0x22,0x33,0x44,0x55,          /* addr3 = bssid */
            0x00, 0x00,                            /* seq */
            0,0,0,0,0,0,0,0,                       /* timestamp */
            0x64, 0x00,                            /* beacon interval 100 */
            0x31, 0x04,                            /* capability 0x0431 */
            0x00, 0x06, 'Y','a','r','t','O','S',    /* SSID IE */
            0x01, 0x04, 0x82,0x84,0x8b,0x96,        /* rates IE */
            0x03, 0x01, 0x06,                       /* DS = channel 6 */
            0x30, 0x14, 0x01,0x00, 0x00,0x0f,0xac,0x04, 0x01,0x00,
            0x00,0x0f,0xac,0x04, 0x01,0x00, 0x00,0x0f,0xac,0x02, 0x00,0x00 };
        wifi_bss_t b;
        if (wifi_parse_probe_resp(resp, sizeof resp, &b)) return 8;
        if (memcmp(b.bssid, bssid, 6)) return 9;
        if (strcmp(b.ssid, "YartOS") || b.ssid_len != 6) return 10;
        if (b.channel != 6) return 11;
        if (b.n_rates != 4 || b.rates[0] != 0x82 || b.rates[3] != 0x96) return 12;
        if (!b.has_rsn || b.rsn_cipher != 4) return 13;
    }

    /* 4. auth exchange */
    n = wifi_build_auth(bssid, sta, 1, f, sizeof f);
    if (n != 30 || le16(f) != 0x00b0) return 14;
    if (le16(f + 24) != 0 || le16(f + 26) != 1) return 15;
    {
        u8 r[30];
        memcpy(r, f, 30);
        memcpy(r + 4, sta, 6);      /* auth resp: addr1 = sta */
        put_le16(r + 28, 0);        /* status success */
        u16 st = 0xffff;
        if (wifi_parse_auth_resp(r, 30, &st) || st != 0) return 16;
    }

    /* 5. assoc exchange */
    n = wifi_build_assoc_req(bssid, sta, "YartOS", f, sizeof f);
    if (n != 24 + 4 + 8 + 6 + 22) return 17;
    if (le16(f) != 0x0000) return 18;                 /* assoc req */
    if (le16(f + 24) != 0x0431) return 19;
    {
        u8 r[32];
        memset(r, 0, sizeof r);
        memcpy(r, f, 24);
        put_le16(r + 0, 0x0010);      /* assoc resp */
        memcpy(r + 4, sta, 6);
        memcpy(r + 16, bssid, 6);
        put_le16(r + 24, 0x0431);     /* capability */
        put_le16(r + 26, 0);          /* status success */
        put_le16(r + 28, 0xc001);     /* AID 1 | 0xc000 */
        u16 st = 0xffff, aid = 0;
        if (wifi_parse_assoc_resp(r, 30, &st, &aid) || st != 0 || aid != 0xc001) return 20;
    }

    return 0;
}
