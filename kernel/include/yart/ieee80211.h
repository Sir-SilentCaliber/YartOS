#pragma once
#include <yart/types.h>

/* Minimal IEEE 802.11 (Wi-Fi) frame codecs — the management-plane half of a
 * hand-rolled mac80211/cfg80211 replacement.  STA (supplicant) side:
 *   - build Probe Request / Authentication / Association Request
 *   - parse  Beacon / Probe Response / Auth Response / Assoc Response
 * Fields are little-endian per the 802.11 spec. */

#define WIFI_FC_TYPE_MGMT 0
#define WIFI_FC_TYPE_CTRL 1
#define WIFI_FC_TYPE_DATA 2

#define WIFI_ST_ASSOC_REQ  0
#define WIFI_ST_ASSOC_RESP 1
#define WIFI_ST_REASSOC_REQ 2
#define WIFI_ST_REASSOC_RESP 3
#define WIFI_ST_PROBE_REQ  4
#define WIFI_ST_PROBE_RESP 5
#define WIFI_ST_BEACON     8
#define WIFI_ST_DISASSOC   10
#define WIFI_ST_AUTH       11
#define WIFI_ST_DEAUTH     12

#define WIFI_AUTH_ALG_OPEN    0
#define WIFI_AUTH_ALG_SHARED  1
#define WIFI_STATUS_SUCCESS   0

#define WIFI_EID_SSID   0
#define WIFI_EID_RATES  1
#define WIFI_EID_DS     3
#define WIFI_EID_RSN    48

/* 802.11 management frame MAC header (24 bytes). */
typedef struct PACKED {
    u16 frame_control;
    u16 duration;
    u8  addr1[6];       /* DA / RA */
    u8  addr2[6];       /* SA / TA */
    u8  addr3[6];       /* BSSID */
    u16 seq_ctrl;
} wifi_mgmt_hdr_t;

/* A discovered/associated BSS, filled by the beacon/probe-response parser. */
typedef struct {
    u8   bssid[6];
    char ssid[33];
    int  ssid_len;
    u8   channel;
    u8   rates[8];      /* basic rates in 500 kbps units (e.g. 11 = 5.5 Mb/s) */
    int  n_rates;
    u16  capability;
    bool has_rsn;       /* RSN (WPA2) IE present */
    u8   rsn_cipher;    /* 4 = CCMP */
    int  signal;        /* dBm — filled in by the driver, not the frame */
} wifi_bss_t;

/* ---- builders (STA -> AP) ---- */
int wifi_build_probe_req(const u8 bssid[6], const char *ssid, u8 *frame, size_t cap);
int wifi_build_auth(const u8 bssid[6], const u8 sta[6], u16 seq, u8 *frame, size_t cap);
int wifi_build_assoc_req(const u8 bssid[6], const u8 sta[6], const char *ssid,
                         u8 *frame, size_t cap);

/* ---- parsers (AP -> STA).  0 = ok, -1 = malformed ---- */
int wifi_parse_beacon(const u8 *frame, size_t len, wifi_bss_t *bss);
int wifi_parse_probe_resp(const u8 *frame, size_t len, wifi_bss_t *bss);
int wifi_parse_auth_resp(const u8 *frame, size_t len, u16 *status);
int wifi_parse_assoc_resp(const u8 *frame, size_t len, u16 *status, u16 *aid);

int ieee80211_selftest(void);   /* 0 = ok */
