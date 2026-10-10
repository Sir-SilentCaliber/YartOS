#pragma once
#include <yart/types.h>

/* WPA2 key-handshake primitives (IEEE 802.11i):
 *  - wpa_prf:        the SHA-1 based PRF (8.5.1.1), used for all key expansion
 *  - wpa_ptk_derive: PTK = PRF-X(PMK, "Pairwise key expansion",
 *                    min(AA,SPA)||max(AA,SPA)||min(ANonce,SNonce)||max(...))
 *  - wpa_eapol_mic:  EAPOL-Key MIC = HMAC-SHA1(KCK, frame with MIC zeroed),
 *                    truncated to 16 bytes
 *
 * Self-test pins the PRF to the IEEE 802.11i PRF vectors (as published in the
 * hostap test suite) and pins PTK derivation to a reference vector plus a
 * min/max-ordering invariance check (swapped MACs/nonces must give the same
 * PTK). */
void wpa_prf(const u8 *key, size_t key_len, const char *label,
             const u8 *data, size_t data_len, u8 *out, size_t out_len);

void wpa_ptk_derive(const u8 pmk[32], const u8 aa[6], const u8 spa[6],
                    const u8 anonce[32], const u8 snonce[32], u8 ptk[48]);

void wpa_eapol_mic(const u8 kck[16], const u8 *frame, size_t len, u8 mic[16]);

int wpa_selftest(void);   /* 0 = ok */
