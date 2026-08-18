/* Yart OS - WPA2 EAPOL-Key 4-way handshake (supplicant side) + AES Key Wrap.
 *
 * AES Key Wrap (RFC 3394): decrypts the GTK sent inside EAPOL-Key Msg 3.
 *   - only the 128-bit KEK case is needed (WPA2's KEK is 128 bits).
 *
 * The 4-way handshake state machine (IEEE 802.11i):
 *   Msg1 (AP):  ANonce + replay counter            -> derive PTK
 *   Msg2 (STA): SNonce + MIC(KCK)                  -> AP derives PTK
 *   Msg3 (AP):  ANonce + MIC + wrapped GTK         -> verify, unwrap GTK
 *   Msg4 (STA): MIC                                -> done
 *
 * Self-test pins AES Key Wrap to RFC 3394 vector 4.1 and then runs a full
 * AP<->STA handshake round-trip (both sides derive the same PTK, all three
 * MICs verify, the GTK unwraps, and a tampered Msg3 is rejected).
 *
 * NOTE: the SNonce is currently generated deterministically; a real entropy
 * source (RDRAND / TSC) must be plugged into eapol_process_msg1 before this
 * runs against a live network.
 */
#include <yart/types.h>
#include <yart/string.h>
#include <yart/sha1.h>
#include <yart/wpa.h>
#include <yart/eapol.h>

extern void aes128_keyexpand(const u8 key[16], u8 rk[176]);
extern void aes128_encrypt_block(const u8 rk[176], const u8 in[16], u8 out[16]);
extern void aes128_decrypt_block(const u8 rk[176], const u8 in[16], u8 out[16]);

/* ---------- big-endian helpers ---------- */
static u16 be16(const u8 *p) { return (u16)(((u16)p[0] << 8) | p[1]); }
static void put16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static u64 be64(const u8 *p) {
    u64 v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
    return v;
}
static void put64(u8 *p, u64 v) {
    for (int i = 7; i >= 0; i--) { p[i] = (u8)v; v >>= 8; }
}

/* ---------- AES Key Wrap (RFC 3394, 128-bit KEK) ---------- */
void aes_key_wrap(const u8 kek[16], const u8 *pt, int n, u8 *ct) {
    u8 rk[176];
    aes128_keyexpand(kek, rk);
    u8 a[8], b[16], enc[16];
    memset(a, 0xa6, 8);                     /* default IV */
    memcpy(ct + 8, pt, 8 * (size_t)n);
    for (int j = 0; j < 6; j++) {
        for (int i = 1; i <= n; i++) {
            u64 t = (u64)n * j + i;
            memcpy(b, a, 8);
            memcpy(b + 8, ct + 8 * (size_t)i, 8);
            aes128_encrypt_block(rk, b, enc);
            memcpy(ct + 8 * (size_t)i, enc + 8, 8);
            for (int k = 0; k < 8; k++) a[k] = enc[k] ^ (u8)(t >> (56 - 8 * k));
        }
    }
    memcpy(ct, a, 8);
}

int aes_key_unwrap(const u8 kek[16], const u8 *ct, int n, u8 *pt) {
    u8 rk[176];
    aes128_keyexpand(kek, rk);
    u8 a[8], b[16], dec[16];
    memcpy(a, ct, 8);
    memcpy(pt, ct + 8, 8 * (size_t)n);
    for (int j = 5; j >= 0; j--) {
        for (int i = n; i >= 1; i--) {
            u64 t = (u64)n * j + i;
            for (int k = 0; k < 8; k++) b[k] = a[k] ^ (u8)(t >> (56 - 8 * k));
            memcpy(b + 8, pt + 8 * (size_t)(i - 1), 8);
            aes128_decrypt_block(rk, b, dec);
            memcpy(a, dec, 8);
            memcpy(pt + 8 * (size_t)(i - 1), dec + 8, 8);
        }
    }
    for (int i = 0; i < 8; i++) if (a[i] != 0xa6) return -1;   /* IV check */
    return 0;
}

/* ---------- EAPOL-Key frame layout ---------- */
#define E_DESC    0
#define E_KEYINFO 1
#define E_KEYLEN  3
#define E_REPLAY  5
#define E_NONCE   13
#define E_IV      45
#define E_RSC     61
#define E_ID      69
#define E_MIC     77
#define E_DLEN    93
#define E_DATA    95

#define WPA_KI_VERSION  0x0002
#define WPA_KI_TYPE     0x0008
#define WPA_KI_INSTALL  0x0040
#define WPA_KI_ACK      0x0080
#define WPA_KI_MIC      0x0100
#define WPA_KI_SECURE   0x0200
#define WPA_KI_ENCR     0x1000

static int ct_memcmp(const u8 *a, const u8 *b, int n) {
    u8 d = 0;
    for (int i = 0; i < n; i++) d |= a[i] ^ b[i];
    return d;
}

/* MIC over the frame with the Key MIC field zeroed (shared by both sides). */
static void mic_frame(const u8 kck[16], const u8 *frame, size_t len, u8 mic[16]) {
    u8 tmp[256];
    memcpy(tmp, frame, len);
    memset(tmp + E_MIC, 0, 16);
    wpa_eapol_mic(kck, tmp, len, mic);
}

/* Identify which message of the 4-way handshake this is (1..4, 0 = unknown). */
int eapol_key_msg_type(const u8 *f, size_t len) {
    if (len < 95) return 0;
    if (f[E_DESC] != 254 && f[E_DESC] != 2) return 0;
    u16 ki = be16(f + E_KEYINFO);
    bool ack = (ki & WPA_KI_ACK) != 0;
    bool mic = (ki & WPA_KI_MIC) != 0;
    if (ack && !mic) return 1;
    if (!ack && mic) return (ki & WPA_KI_SECURE) ? 4 : 2;
    if (ack && mic)  return 3;
    return 0;
}

/* ---------- supplicant ---------- */
void eapol_init(eapol_supplicant_t *s, const u8 pmk[32],
                const u8 aa[6], const u8 spa[6]) {
    memcpy(s->pmk, pmk, 32);
    memcpy(s->aa, aa, 6);
    memcpy(s->spa, spa, 6);
    s->replay_counter = 0;
    s->have_ptk = s->have_gtk = false;
    s->gtk_len = 0;
    s->gtk_key_id = 0;
    memset(s->anonce, 0, 32);
    memset(s->snonce, 0, 32);
    memset(s->ptk, 0, 48);
}

int eapol_process_msg1(eapol_supplicant_t *s, const u8 *f, size_t len) {
    if (len < 95) return -1;
    if (f[E_DESC] != 254 && f[E_DESC] != 2) return -1;
    u16 ki = be16(f + E_KEYINFO);
    if (!(ki & WPA_KI_ACK)) return -1;          /* Msg1 carries ACK */
    if (ki & WPA_KI_MIC) return -1;             /* Msg1 has no MIC */
    memcpy(s->anonce, f + E_NONCE, 32);
    s->replay_counter = be64(f + E_REPLAY);
    for (int i = 0; i < 32; i++) s->snonce[i] = (u8)(0x22 + i);  /* TODO: entropy */
    wpa_ptk_derive(s->pmk, s->aa, s->spa, s->anonce, s->snonce, s->ptk);
    s->have_ptk = true;
    return 0;
}

int eapol_build_msg2(eapol_supplicant_t *s, u8 *f, size_t cap) {
    static const u8 rsn_ie[22] = {
        0x30,0x14,0x01,0x00,0x00,0x0f,0xac,0x04,0x01,0x00,0x00,
        0x0f,0xac,0x04,0x01,0x00,0x00,0x0f,0xac,0x02,0x00,0x00 };
    int len = 95 + 22;
    if (!s->have_ptk || cap < (size_t)len) return -1;
    memset(f, 0, 95);
    f[E_DESC] = 254;
    put16(f + E_KEYINFO, WPA_KI_VERSION | WPA_KI_TYPE | WPA_KI_MIC);
    put16(f + E_KEYLEN, 16);
    put64(f + E_REPLAY, s->replay_counter);
    memcpy(f + E_NONCE, s->snonce, 32);
    put16(f + E_DLEN, 22);
    memcpy(f + E_DATA, rsn_ie, 22);
    u8 mic[16];
    mic_frame(s->ptk, f, len, mic);
    memcpy(f + E_MIC, mic, 16);
    return len;
}

int eapol_process_msg3(eapol_supplicant_t *s, const u8 *f, size_t len) {
    if (len < 95 || !s->have_ptk) return -1;
    if (f[E_DESC] != 254 && f[E_DESC] != 2) return -1;
    u16 ki = be16(f + E_KEYINFO);
    if (!(ki & WPA_KI_MIC) || !(ki & WPA_KI_ACK)) return -1;
    u8 expect[16], got[16];
    memcpy(expect, f + E_MIC, 16);
    mic_frame(s->ptk, f, len, got);
    if (ct_memcmp(expect, got, 16)) return -1;   /* MIC mismatch */
    if (memcmp(f + E_NONCE, s->anonce, 32)) return -1;   /* ANonce must match */
    s->replay_counter = be64(f + E_REPLAY);
    if (ki & WPA_KI_ENCR) {
        u16 dl = be16(f + E_DLEN);
        if (dl != 24 || len < (size_t)95 + dl) return -1;
        if (aes_key_unwrap(s->ptk + 16, f + E_DATA, dl / 8 - 1, s->gtk)) return -1;
        s->gtk_len = 16;
        s->gtk_key_id = (u8)((ki >> 4) & 0x3);
    }
    s->have_gtk = true;
    return 0;
}

int eapol_build_msg4(eapol_supplicant_t *s, u8 *f, size_t cap) {
    int len = 95;
    if (!s->have_ptk || cap < (size_t)len) return -1;
    memset(f, 0, 95);
    f[E_DESC] = 254;
    put16(f + E_KEYINFO, WPA_KI_VERSION | WPA_KI_TYPE | WPA_KI_MIC | WPA_KI_SECURE);
    put16(f + E_KEYLEN, 0);
    put64(f + E_REPLAY, s->replay_counter);
    u8 mic[16];
    mic_frame(s->ptk, f, len, mic);
    memcpy(f + E_MIC, mic, 16);
    return len;
}

/* ---------- self-test: key wrap vector + full handshake round-trip ---------- */
typedef struct {
    u8 pmk[32], aa[6], spa[6];
    u8 anonce[32], snonce[32], ptk[48];
    u64 rctr;
    u8 gtk[16];
} ap_side_t;

static int ap_m2(ap_side_t *a, const u8 *f, size_t len) {
    if (len < 95) return -1;
    memcpy(a->snonce, f + E_NONCE, 32);
    wpa_ptk_derive(a->pmk, a->aa, a->spa, a->anonce, a->snonce, a->ptk);
    u8 expect[16], got[16];
    memcpy(expect, f + E_MIC, 16);
    mic_frame(a->ptk, f, len, got);
    return ct_memcmp(expect, got, 16) ? -1 : 0;
}

static int ap_m3(ap_side_t *a, u8 *f) {
    int len = 95 + 24;
    memset(f, 0, 95);
    f[E_DESC] = 254;
    put16(f + E_KEYINFO, WPA_KI_VERSION | WPA_KI_TYPE | WPA_KI_ACK | WPA_KI_MIC |
                         WPA_KI_INSTALL | WPA_KI_ENCR | WPA_KI_SECURE);
    put16(f + E_KEYLEN, 16);
    a->rctr++;
    put64(f + E_REPLAY, a->rctr);
    memcpy(f + E_NONCE, a->anonce, 32);
    put16(f + E_DLEN, 24);
    aes_key_wrap(a->ptk + 16, a->gtk, 2, f + E_DATA);
    u8 mic[16];
    mic_frame(a->ptk, f, len, mic);
    memcpy(f + E_MIC, mic, 16);
    return len;
}

static int ap_m4(ap_side_t *a, const u8 *f, size_t len) {
    if (len < 95) return -1;
    u8 expect[16], got[16];
    memcpy(expect, f + E_MIC, 16);
    mic_frame(a->ptk, f, len, got);
    return ct_memcmp(expect, got, 16) ? -1 : 0;
}

int eapol_selftest(void) {
    u8 f[256];
    int flen;

    /* ---- RFC 3394 4.1: 128-bit KEK wraps 128-bit key data ---- */
    {
        static const u8 kek[16] = {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
                                   0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f};
        static const u8 pt[16] = {0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
                                  0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff};
        static const u8 want[24] = {0x1f,0xa6,0x8b,0x0a,0x81,0x12,0xb4,0x47,
                                    0xae,0xf3,0x4b,0xd8,0xfb,0x5a,0x7b,0x82,
                                    0x9d,0x3e,0x86,0x23,0x71,0xd2,0xcf,0xe5};
        u8 ct[24], back[16];
        aes_key_wrap(kek, pt, 2, ct);
        if (memcmp(ct, want, 24)) return 1;
        if (aes_key_unwrap(kek, ct, 2, back) || memcmp(back, pt, 16)) return 2;
    }

    /* ---- full 4-way handshake round-trip ---- */
    {
        static const u8 pmk[32] = {
            0xf4,0x2c,0x6f,0xc5,0x2d,0xf0,0xeb,0xef,0x9e,0xbb,0x4b,0x90,0xb3,0x8a,0x5f,0x90,
            0x2e,0x83,0xfe,0x1b,0x13,0x5a,0x70,0xe2,0x3a,0xed,0x76,0x2e,0x97,0x10,0xa1,0x2e };
        static const u8 aa[6]  = {0x00,0x10,0x18,0xde,0x0b,0xb1};
        static const u8 spa[6] = {0x00,0x10,0x18,0xde,0x0b,0xb2};

        ap_side_t ap;
        memset(&ap, 0, sizeof ap);
        memcpy(ap.pmk, pmk, 32);
        memcpy(ap.aa, aa, 6);
        memcpy(ap.spa, spa, 6);
        for (int i = 0; i < 32; i++) ap.anonce[i] = (u8)(0x11 + i);
        for (int i = 0; i < 16; i++) ap.gtk[i] = (u8)(0x44 + i);
        ap.rctr = 1;

        eapol_supplicant_t sta;
        eapol_init(&sta, pmk, aa, spa);

        /* Msg1 */
        memset(f, 0, 95);
        f[E_DESC] = 254;
        put16(f + E_KEYINFO, WPA_KI_VERSION | WPA_KI_TYPE | WPA_KI_ACK);
        put16(f + E_KEYLEN, 16);
        put64(f + E_REPLAY, ap.rctr);
        memcpy(f + E_NONCE, ap.anonce, 32);
        if (eapol_process_msg1(&sta, f, 95)) return 3;

        /* Msg2 */
        flen = eapol_build_msg2(&sta, f, sizeof f);
        if (flen < 0 || ap_m2(&ap, f, flen)) return 4;
        if (memcmp(ap.ptk, sta.ptk, 48)) return 5;   /* both sides same PTK */

        /* Msg3 */
        flen = ap_m3(&ap, f);
        if (eapol_process_msg3(&sta, f, flen)) return 6;
        if (memcmp(sta.gtk, ap.gtk, 16)) return 7;   /* GTK unwrapped correctly */

        /* Msg4 */
        flen = eapol_build_msg4(&sta, f, sizeof f);
        if (flen < 0 || ap_m4(&ap, f, flen)) return 8;

        /* tampered Msg3 must be rejected */
        {
            u8 t[256];
            int tl = ap_m3(&ap, t);
            t[E_DATA] ^= 0x80;                       /* corrupt wrapped GTK */
            eapol_supplicant_t s2;
            eapol_init(&s2, pmk, aa, spa);
            memset(f, 0, 95);
            f[E_DESC] = 254;
            put16(f + E_KEYINFO, WPA_KI_VERSION | WPA_KI_TYPE | WPA_KI_ACK);
            put16(f + E_KEYLEN, 16);
            put64(f + E_REPLAY, ap.rctr - 1);
            memcpy(f + E_NONCE, ap.anonce, 32);
            if (eapol_process_msg1(&s2, f, 95)) return 9;
            if (eapol_process_msg3(&s2, t, tl) == 0) return 10;  /* must fail */
        }
    }

    return 0;
}
