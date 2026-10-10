#pragma once
#include <yart/types.h>
#include <yart/wifi_sta.h>
#include <yart/ieee80211.h>

/* YartOS Wi-Fi session orchestrator — the layer that drives the 802.11
 * station state machine through a frame TRANSPORT.  The transport is a
 * 3-function interface so the same session code runs over:
 *   - the real rtw88 DMA rings (rtw_io) on hardware, and
 *   - a simulated AP in the selftest (proving the full stack, radio-free).
 *
 * A full session = probe/scan -> auth -> assoc -> EAPOL 4-way handshake ->
 * CCMP-protected data.  This is the capstone that ties every other brick
 * together: ieee80211 codecs + eapol handshake + ccmp + wifi_sta.
 */

/* ---- abstract frame transport (Skift hci_ops-style) ---- */
typedef struct {
    /* send a raw 802.11 frame (0 = ok) */
    int  (*tx)(void *ctx, const u8 *frame, u32 len);
    /* receive one frame (1 = frame in `frame`/`out_len`, 0 = none, -1 = err) */
    int  (*rx)(void *ctx, u8 *frame, u32 cap, u32 *out_len);
    /* poll delay between receive attempts */
    void (*delay_ms)(void *ctx, u32 ms);
} wifi_transport_t;

typedef struct {
    const wifi_transport_t *tr;
    void *tr_ctx;
    wifi_sta_t sta;
    u8   pn_tx[6];      /* CCMP packet numbers (data path) */
    u8   pn_rx[6];
    u16  seq_tx;
    u32  seq_rx;
} wifi_session_t;

void wifi_session_init(wifi_session_t *s, const wifi_transport_t *tr,
                       void *ctx, const u8 sta_mac[6]);

/* Scan: send a broadcast probe request and collect probe responses. */
int  wifi_session_scan(wifi_session_t *s, wifi_bss_t *results, int max,
                       u32 timeout_ms);

/* Join a BSS: auth -> assoc -> 4-way handshake.  0 = connected. */
int  wifi_session_join(wifi_session_t *s, const wifi_bss_t *bss,
                       const char *passphrase);

/* Data path.  eth = dst[6]||src[6]||type[2]||payload. */
int  wifi_session_send_data(wifi_session_t *s, const u8 *eth, size_t eth_len);
int  wifi_session_recv_data(wifi_session_t *s, u8 *eth_dst, u8 *eth_src,
                            u16 *ethertype, u8 *payload, size_t cap,
                            size_t *plen);

bool wifi_session_connected(const wifi_session_t *s);

int wifi_session_selftest(void);   /* 0 = ok */
