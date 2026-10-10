#pragma once
#include <yart/types.h>
#include <yart/eapol.h>
#include <yart/ieee80211.h>

/* WPA2 station (supplicant) connection state machine:
 *   DISCONNECTED -> AUTHENTICATING -> ASSOCIATING -> ASSOCIATED
 *   -> (4-way handshake) -> CONNECTED
 * The radio I/O (actually sending/reading frames) is done by the driver; this
 * state machine consumes/produces frames and drives the EAPOL handshake. */
typedef enum {
    WIFI_STA_DISCONNECTED = 0,
    WIFI_STA_AUTHENTICATING,
    WIFI_STA_ASSOCIATING,
    WIFI_STA_ASSOCIATED,     /* associated; 4-way handshake in progress */
    WIFI_STA_CONNECTED,
} wifi_sta_state_t;

typedef struct {
    wifi_sta_state_t state;
    u8   sta_mac[6];
    u8   bssid[6];
    char ssid[33];
    u8   channel;
    u16  aid;
    u16  seq;
    eapol_supplicant_t eapol;
    bool pmk_set;
    u8   tk[16];             /* temporal key (installed after the handshake) */
    bool tk_installed;
} wifi_sta_t;

void wifi_sta_init(wifi_sta_t *s, const u8 sta_mac[6]);
void wifi_sta_set_pmk(wifi_sta_t *s, const u8 pmk[32]);
void wifi_sta_set_passphrase(wifi_sta_t *s, const char *passphrase, const char *ssid);

/* Begin joining a BSS (found via scan). -> AUTHENTICATING */
void wifi_sta_start_join(wifi_sta_t *s, const wifi_bss_t *bss);

/* Auth phase: build request; feed response (0 = ok). */
int  wifi_sta_build_auth(wifi_sta_t *s, u8 *frame, size_t cap);
int  wifi_sta_on_auth_resp(wifi_sta_t *s, const u8 *frame, size_t len);

/* Assoc phase. */
int  wifi_sta_build_assoc(wifi_sta_t *s, u8 *frame, size_t cap);
int  wifi_sta_on_assoc_resp(wifi_sta_t *s, const u8 *frame, size_t len);

/* 4-way handshake: feed an EAPOL-Key descriptor; returns reply length (>0 =
 * a reply frame was written), 0 = no reply, -1 = failed. */
int  wifi_sta_eapol_in(wifi_sta_t *s, const u8 *frame, size_t len,
                       u8 *reply, size_t cap);

bool wifi_sta_connected(const wifi_sta_t *s);

int wifi_sta_selftest(void);   /* 0 = ok */
