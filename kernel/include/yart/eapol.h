#pragma once
#include <yart/types.h>

/* AES Key Wrap (RFC 3394) — used to decrypt the GTK in EAPOL-Key Msg 3.
 * 128-bit KEK only (that is all WPA2's KEK is).  `n` = number of 64-bit
 * blocks in the plaintext; ciphertext is (n+1)*8 bytes. */
void aes_key_wrap(const u8 kek[16], const u8 *pt, int n, u8 *ct);
int  aes_key_unwrap(const u8 kek[16], const u8 *ct, int n, u8 *pt); /* 0 = ok, -1 = bad IV */

/* Supplicant-side WPA2 4-way handshake state machine (IEEE 802.11i).
 * Usage: eapol_init() with PMK + MACs, then feed Msg1, build Msg2, feed
 * Msg3, build Msg4.  The PTK/GTK land in the struct for the data path. */
typedef struct {
    u8  pmk[32];
    u8  aa[6], spa[6];          /* authenticator / supplicant MACs */
    u8  anonce[32], snonce[32];
    u8  ptk[48];                /* KCK 0..15, KEK 16..31, TK 32..47 */
    u64 replay_counter;
    bool have_ptk;
    bool have_gtk;
    u8  gtk[32];
    int gtk_len;
    u8  gtk_key_id;
} eapol_supplicant_t;

void eapol_init(eapol_supplicant_t *s, const u8 pmk[32],
                const u8 aa[6], const u8 spa[6]);

int eapol_process_msg1(eapol_supplicant_t *s, const u8 *frame, size_t len);
int eapol_build_msg2(eapol_supplicant_t *s, u8 *frame, size_t cap);
int eapol_process_msg3(eapol_supplicant_t *s, const u8 *frame, size_t len);
int eapol_build_msg4(eapol_supplicant_t *s, u8 *frame, size_t cap);

/* EAPOL-Key descriptor layout (offset 0 = Descriptor Type byte; the 802.1X
 * header is stripped by the caller). */
#define EAPOL_DESC    0
#define EAPOL_KEYINFO 1
#define EAPOL_KEYLEN  3
#define EAPOL_REPLAY  5
#define EAPOL_NONCE   13
#define EAPOL_IV      45
#define EAPOL_RSC     61
#define EAPOL_ID      69
#define EAPOL_MIC     77
#define EAPOL_DLEN    93
#define EAPOL_DATA    95

#define WPA_KI_VERSION  0x0002
#define WPA_KI_TYPE     0x0008
#define WPA_KI_INSTALL  0x0040
#define WPA_KI_ACK      0x0080
#define WPA_KI_MIC      0x0100
#define WPA_KI_SECURE   0x0200
#define WPA_KI_ENCR     0x1000

/* Identify an EAPOL-Key message of the 4-way handshake: 1..4, 0 if unknown. */
int eapol_key_msg_type(const u8 *frame, size_t len);

int eapol_selftest(void);   /* 0 = ok */
