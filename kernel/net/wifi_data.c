/* Yart OS - 802.11 CCMP data-frame codec.
 *
 * Encrypts/decrypts non-QoS 802.11 data frames with AES-CCMP the same way
 * Linux mac80211 does:
 *   nonce = [ Priority=0, A2 (transmitter), PN0..PN5 ]
 *   AAD   = FC(masked) || A1 || A2 || A3 || SC(frag# only)
 *     FC mask: clear Retry(11)/PwrMgmt(12)/MoreData(13), set Protected(14)
 *   payload = LLC/SNAP (AA AA 03 00 00 00 + EtherType) + IP packet
 */
#include <yart/types.h>
#include <yart/string.h>
#include <yart/ccmp.h>
#include <yart/wifi_data.h>

int wifi_data_encrypt(u8 *out, size_t cap, const u8 tk[16], const u8 pn[6],
                      u16 seq, const u8 bssid[6], const u8 sta_mac[6],
                      bool to_ds, const u8 *eth, size_t eth_len) {
    if (eth_len < 14) return -1;
    size_t pt_len = eth_len - 14 + WIFI_SNAP_HDRLEN;
    size_t total = WIFI_HDRLEN + WIFI_CCMP_HDRLEN + pt_len + WIFI_CCMP_MICLEN;
    if (cap < total) return -1;

    u16 fc = 0x0008;                     /* Data subtype 0 */
    fc |= 0x4000;                        /* Protected frame */
    if (to_ds) fc |= 0x0100; else fc |= 0x0200;

    out[0] = (u8)fc; out[1] = (u8)(fc >> 8);
    out[2] = 0; out[3] = 0;              /* duration */
    if (to_ds) {                         /* STA->AP: A1=BSSID, A2=STA, A3=eth dst */
        memcpy(out + 4,  bssid,   6);
        memcpy(out + 10, sta_mac, 6);
        memcpy(out + 16, eth,     6);
    } else {                             /* AP->STA: A1=STA, A2=BSSID, A3=eth src */
        memcpy(out + 4,  sta_mac, 6);
        memcpy(out + 10, bssid,   6);
        memcpy(out + 16, eth + 6, 6);
    }
    out[22] = (u8)(seq & 0x0f);          /* fragment 0 + seq low 4 bits */
    out[23] = (u8)(seq >> 4);

    /* CCMP header: PN (LE) + ExtIV(KeyID=0, ExtIV=1) + reserved */
    memcpy(out + WIFI_HDRLEN, pn, 6);
    out[WIFI_HDRLEN + 6] = 0x20;
    out[WIFI_HDRLEN + 7] = 0x00;

    u8 nonce[13];
    nonce[0] = 0;                        /* non-QoS priority */
    memcpy(nonce + 1, out + 10, 6);      /* A2 */
    memcpy(nonce + 7, pn, 6);

    u8 aad[22];
    u16 mfc = (fc & ~0x3800u) | 0x4000u;
    aad[0] = (u8)mfc; aad[1] = (u8)(mfc >> 8);
    memcpy(aad + 2,  out + 4, 18);       /* A1 A2 A3 */
    aad[20] = out[22] & 0x0f;            /* fragment number only */
    aad[21] = 0;

    u8 *payload = out + WIFI_HDRLEN + WIFI_CCMP_HDRLEN;
    payload[0] = 0xAA; payload[1] = 0xAA; payload[2] = 0x03;   /* LLC */
    payload[3] = 0x00; payload[4] = 0x00; payload[5] = 0x00;   /* SNAP OUI */
    payload[6] = eth[12]; payload[7] = eth[13];                /* EtherType */
    memcpy(payload + 8, eth + 14, eth_len - 14);

    u8 mic[WIFI_CCMP_MICLEN];
    ccmp_encrypt(tk, nonce, aad, sizeof aad, payload, pt_len, payload, mic);
    memcpy(payload + pt_len, mic, WIFI_CCMP_MICLEN);
    return (int)total;
}

int wifi_data_decrypt(const u8 tk[16], u8 *frame, size_t len,
                      u8 *eth_dst, u8 *eth_src, u16 *ethertype) {
    if (len < WIFI_HDRLEN + WIFI_CCMP_HDRLEN + WIFI_CCMP_MICLEN) return -1;
    u16 fc = (u16)(frame[0] | (frame[1] << 8));

    u8 pn[6];
    memcpy(pn, frame + WIFI_HDRLEN, 6);
    u8 nonce[13];
    nonce[0] = 0;
    memcpy(nonce + 1, frame + 10, 6);    /* A2 */
    memcpy(nonce + 7, pn, 6);

    u16 mfc = (fc & ~0x3800u) | 0x4000u;
    u8 aad[22];
    aad[0] = (u8)mfc; aad[1] = (u8)(mfc >> 8);
    memcpy(aad + 2, frame + 4, 18);
    aad[20] = frame[22] & 0x0f;
    aad[21] = 0;

    size_t ct_len = len - WIFI_HDRLEN - WIFI_CCMP_HDRLEN - WIFI_CCMP_MICLEN;
    u8 *ct = frame + WIFI_HDRLEN + WIFI_CCMP_HDRLEN;
    const u8 *mic = ct + ct_len;
    if (ccmp_decrypt(tk, nonce, aad, sizeof aad, ct, ct_len, mic, ct)) return -1;

    if (ct_len < 8 || ct[0] != 0xAA || ct[1] != 0xAA || ct[2] != 0x03) return -1;
    if (ethertype) *ethertype = (u16)(((u16)ct[6] << 8) | ct[7]);
    if (fc & 0x0100) {                   /* ToDS */
        if (eth_dst) memcpy(eth_dst, frame + 16, 6);
        if (eth_src) memcpy(eth_src, frame + 10, 6);
    } else {                             /* FromDS */
        if (eth_dst) memcpy(eth_dst, frame + 4, 6);
        if (eth_src) memcpy(eth_src, frame + 16, 6);
    }
    return 0;
}

int wifi_data_selftest(void) {
    static const u8 tk[16] = {
        0xaf,0x47,0x36,0x1e,0xb1,0x66,0x34,0x5c,0x21,0x26,0x82,0xee,0x42,0xa8,0xe9,0x96 };
    static const u8 bssid[6] = {0x02,0x11,0x22,0x33,0x44,0x55};
    static const u8 sta[6]   = {0x02,0x66,0x77,0x88,0x99,0xaa};
    static const u8 pn[6]    = {0x01,0x00,0x00,0x00,0x00,0x00};

    /* Ethernet frame: dst/srcless synthetic + EtherType 0x0800 + payload */
    u8 eth[14 + 20];
    memset(eth, 0, sizeof eth);
    eth[0] = 0xc0; eth[1] = 0x01; eth[2] = 0x02; eth[3] = 0x03; eth[4] = 0x04; eth[5] = 0x05;
    eth[6] = 0xa0; eth[7] = 0x01; eth[8] = 0x02; eth[9] = 0x03; eth[10] = 0x04; eth[11] = 0x05;
    eth[12] = 0x08; eth[13] = 0x00;      /* IPv4 */
    for (int i = 0; i < 20; i++) eth[14 + i] = (u8)(0x40 + i);

    u8 f[512];
    int n;

    /* AP -> STA */
    n = wifi_data_encrypt(f, sizeof f, tk, pn, 1, bssid, sta, false, eth, sizeof eth);
    if (n != WIFI_HDRLEN + WIFI_CCMP_HDRLEN + (sizeof eth - 14 + 8) + 8) return 1;
    if (!(f[1] & 0x40)) return 2;                    /* protected bit set */
    u8 dst[6], src[6]; u16 etype = 0;
    if (wifi_data_decrypt(tk, f, n, dst, src, &etype)) return 3;
    if (memcmp(dst, sta, 6) || memcmp(src, eth + 6, 6)) return 4;
    if (etype != 0x0800) return 5;
    if (memcmp(f + WIFI_HDRLEN + WIFI_CCMP_HDRLEN + 8, eth + 14, 20)) return 6;

    /* STA -> AP */
    n = wifi_data_encrypt(f, sizeof f, tk, pn, 2, bssid, sta, true, eth, sizeof eth);
    if (n < 0) return 7;
    if (wifi_data_decrypt(tk, f, n, dst, src, &etype)) return 8;
    if (memcmp(dst, eth, 6) || memcmp(src, sta, 6)) return 9;

    /* tampered ciphertext must fail the MIC */
    n = wifi_data_encrypt(f, sizeof f, tk, pn, 3, bssid, sta, false, eth, sizeof eth);
    if (n < 0) return 10;
    f[WIFI_HDRLEN + WIFI_CCMP_HDRLEN + 5] ^= 0xff;
    if (wifi_data_decrypt(tk, f, n, dst, src, &etype) == 0) return 11;

    return 0;
}
