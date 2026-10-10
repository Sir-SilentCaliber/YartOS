#pragma once
#include <yart/types.h>

/* 802.11 CCMP data-frame codec (STA side, non-QoS data + LLC/SNAP).
 * Mirrors IEEE 802.11i: the AAD is the masked MAC header and the nonce is
 * Priority(0) || A2 || PN, which is exactly how Linux mac80211 builds it. */

#define WIFI_HDRLEN       24
#define WIFI_CCMP_HDRLEN  8
#define WIFI_CCMP_MICLEN  8
#define WIFI_SNAP_HDRLEN  8

/* Encrypt an Ethernet frame (dst+src+type+payload) into an 802.11 protected
 * data frame.  to_ds=true for STA->AP.  pn[6] is little-endian (PN0 first)
 * and is advanced by the caller.  Returns total frame length or -1. */
int wifi_data_encrypt(u8 *out, size_t cap, const u8 tk[16], const u8 pn[6],
                      u16 seq, const u8 bssid[6], const u8 sta_mac[6],
                      bool to_ds, const u8 *eth, size_t eth_len);

/* Decrypt a protected 802.11 data frame IN PLACE (plaintext lands at
 * frame+WIFI_HDRLEN+WIFI_CCMP_HDRLEN: LLC/SNAP + payload).  Returns 0 on
 * success (MIC verified) and fills dst/src/ethertype; -1 on MIC failure. */
int wifi_data_decrypt(const u8 tk[16], u8 *frame, size_t len,
                      u8 *eth_dst, u8 *eth_src, u16 *ethertype);

int wifi_data_selftest(void);   /* 0 = ok */
