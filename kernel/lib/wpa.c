/* Yart OS - WPA2 key-handshake primitives (IEEE 802.11i).
 *
 * The 4-way handshake turns the PMK (from PBKDF2, see sha1.c) into the PTK
 * whose sub-keys are:
 *   KCK (0..15)  - Key Confirmation Key: EAPOL-Key MICs
 *   KEK (16..31) - Key Encryption Key: encrypts the GTK in Message 3
 *   TK  (32..47) - Temporal Key: feeds AES-CCMP (see ccmp.c)
 *
 * PRF (8.5.1.1): R = HMAC-SHA1(K, label || 0x00 || data || counter)
 * for counter = 0,1,2,...  (label is taken WITH its NUL terminator.)
 */
#include <yart/types.h>
#include <yart/string.h>
#include <yart/sha1.h>
#include <yart/wpa.h>

void wpa_prf(const u8 *key, size_t key_len, const char *label,
             const u8 *data, size_t data_len, u8 *out, size_t out_len) {
    u8 counter = 0;
    size_t label_len = strlen(label) + 1;   /* includes the NUL separator */
    size_t pos = 0;
    u8 msg[128];
    while (pos < out_len) {
        size_t m = 0;
        memcpy(msg + m, label, label_len); m += label_len;
        memcpy(msg + m, data, data_len);   m += data_len;
        msg[m++] = counter++;
        u8 h[20];
        hmac_sha1(key, key_len, msg, m, h);
        size_t take = out_len - pos;
        if (take > 20) take = 20;
        memcpy(out + pos, h, take);
        pos += take;
    }
}

void wpa_ptk_derive(const u8 pmk[32], const u8 aa[6], const u8 spa[6],
                    const u8 anonce[32], const u8 snonce[32], u8 ptk[48]) {
    u8 data[76];
    int off = 0;
    const u8 *m1 = memcmp(aa, spa, 6) < 0 ? aa : spa;
    const u8 *m2 = memcmp(aa, spa, 6) < 0 ? spa : aa;
    memcpy(data + off, m1, 6); off += 6;
    memcpy(data + off, m2, 6); off += 6;
    const u8 *n1 = memcmp(anonce, snonce, 32) < 0 ? anonce : snonce;
    const u8 *n2 = memcmp(anonce, snonce, 32) < 0 ? snonce : anonce;
    memcpy(data + off, n1, 32); off += 32;
    memcpy(data + off, n2, 32); off += 32;
    wpa_prf(pmk, 32, "Pairwise key expansion", data, off, ptk, 48);
}

void wpa_eapol_mic(const u8 kck[16], const u8 *frame, size_t len, u8 mic[16]) {
    u8 full[20];
    hmac_sha1(kck, 16, frame, len, full);
    memcpy(mic, full, 16);
}

int wpa_selftest(void) {
    u8 out[64];

    /* ---- IEEE 802.11i PRF vector 0: key=0x0b*20, "prefix", "Hi There" ---- */
    {
        static const u8 key[20] = {
            0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,
            0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b };
        static const u8 want[64] = {
            0xbc,0xd4,0xc6,0x50,0xb3,0x0b,0x96,0x84,0x95,0x18,0x29,0xe0,0xd7,0x5f,0x9d,0x54,
            0xb8,0x62,0x17,0x5e,0xd9,0xf0,0x06,0x06,0xe1,0x7d,0x8d,0xa3,0x54,0x02,0xff,0xee,
            0x75,0xdf,0x78,0xc3,0xd3,0x1e,0x0f,0x88,0x9f,0x01,0x21,0x20,0xc0,0x86,0x2b,0xeb,
            0x67,0x75,0x3e,0x74,0x39,0xae,0x24,0x2e,0xdb,0x83,0x73,0x69,0x83,0x56,0xcf,0x5a };
        wpa_prf(key, 20, "prefix", (const u8 *)"Hi There", 8, out, 64);
        if (memcmp(out, want, 64)) return 1;
    }

    /* ---- IEEE 802.11i PRF vector 1: key="Jefe", "prefix", 28-byte data ---- */
    {
        static const u8 want[64] = {
            0x51,0xf4,0xde,0x5b,0x33,0xf2,0x49,0xad,0xf8,0x1a,0xeb,0x71,0x3a,0x3c,0x20,0xf4,
            0xfe,0x63,0x14,0x46,0xfa,0xbd,0xfa,0x58,0x24,0x47,0x59,0xae,0x58,0xef,0x90,0x09,
            0xa9,0x9a,0xbf,0x4e,0xac,0x2c,0xa5,0xfa,0x87,0xe6,0x92,0xc4,0x40,0xeb,0x40,0x02,
            0x3e,0x7b,0xab,0xb2,0x06,0xd6,0x1d,0xe7,0xb9,0x2f,0x41,0x52,0x90,0x92,0xb8,0xfc };
        wpa_prf((const u8 *)"Jefe", 4, "prefix",
                (const u8 *)"what do ya want for nothing?", 28, out, 64);
        if (memcmp(out, want, 64)) return 2;
    }

    /* ---- PTK derivation: canonical PMK ("password"/"IEEE") + synthetic
     * MACs/nonces; expected PTK computed with an independent reference. ---- */
    {
        static const u8 pmk[32] = {
            0xf4,0x2c,0x6f,0xc5,0x2d,0xf0,0xeb,0xef,0x9e,0xbb,0x4b,0x90,0xb3,0x8a,0x5f,0x90,
            0x2e,0x83,0xfe,0x1b,0x13,0x5a,0x70,0xe2,0x3a,0xed,0x76,0x2e,0x97,0x10,0xa1,0x2e };
        static const u8 aa[6]  = {0x00,0x10,0x18,0xde,0x0b,0xb1};
        static const u8 spa[6] = {0x00,0x10,0x18,0xde,0x0b,0xb2};
        u8 anonce[32], snonce[32], ptk[48], ptk2[48];
        memset(anonce, 0x01, 32);
        memset(snonce, 0x02, 32);
        static const u8 want[48] = {
            0x72,0x2f,0x78,0xda,0x2c,0x1a,0x92,0x3d,0xa5,0x5e,0xd6,0xfe,0x38,0x1b,0x31,0xca,
            0x03,0x5d,0xa3,0xdc,0x03,0xc0,0x97,0xc7,0x9c,0x9b,0x0b,0xd7,0xc1,0xed,0x03,0x75,
            0xaf,0x47,0x36,0x1e,0xb1,0x66,0x34,0x5c,0x21,0x26,0x82,0xee,0x42,0xa8,0xe9,0x96 };
        wpa_ptk_derive(pmk, aa, spa, anonce, snonce, ptk);
        if (memcmp(ptk, want, 48)) return 3;
        /* min/max invariance: swapped MACs and nonces must give the SAME PTK */
        wpa_ptk_derive(pmk, spa, aa, snonce, anonce, ptk2);
        if (memcmp(ptk, ptk2, 48)) return 4;
    }

    /* ---- EAPOL-Key MIC: HMAC-SHA1(KCK, frame) truncated; tamper-detection ---- */
    {
        u8 kck[16], mic1[16], mic2[16];
        u8 frame[32];
        memset(kck, 0x33, 16);
        for (int i = 0; i < 32; i++) frame[i] = (u8)(i * 7 + 1);
        wpa_eapol_mic(kck, frame, 32, mic1);
        frame[5] ^= 0xff;                         /* tamper one byte */
        wpa_eapol_mic(kck, frame, 32, mic2);
        if (memcmp(mic1, mic2, 16) == 0) return 5;
    }

    return 0;
}
