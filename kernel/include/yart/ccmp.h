#pragma once
#include <yart/types.h>

/* AES-CCMP (IEEE 802.11i): AES-CCM with M=8 (8-byte MIC) and L=2
 * (13-byte nonce).  This is the encryption + authentication that protects
 * every data frame in WPA2.  Built on the existing AES-128 block cipher.
 *
 * Self-test pins the implementation to RFC 3610 Packet Vectors #1/#3/#13
 * (the M=8, L=2 CCM cases). */

/* out = ciphertext (pt_len bytes); mic[8] = integrity check value. */
void ccmp_encrypt(const u8 key[16], const u8 nonce[13],
                  const u8 *aad, size_t aad_len,
                  const u8 *pt, size_t pt_len,
                  u8 *ct_out, u8 mic[8]);

/* out = plaintext.  Returns 0 on success, -1 if the MIC does not verify. */
int ccmp_decrypt(const u8 key[16], const u8 nonce[13],
                 const u8 *aad, size_t aad_len,
                 const u8 *ct, size_t ct_len, const u8 mic[8],
                 u8 *pt_out);

int ccmp_selftest(void);   /* 0 = all vectors matched */
