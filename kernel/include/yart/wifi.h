#pragma once
#include <yart/types.h>

/* Yart WiFi subsystem - v3 MAX push.
 * Provides real WiFi management on top of existing net stack.
 * - PCI detection of 802.11 wireless controllers (Intel, Realtek, Atheros, Broadcom)
 * - Software emulation over e1000 when no HW present (QEMU) - exposes as wlan0 with fake AP scan but real traffic via Ethernet backend
 * - Full state machine: SCAN -> AUTH -> ASSOC -> CONNECTED
 * - WPA2-PSK (simulated handshake, real traffic encrypted via existing TLS/AES layer)
 */

#define WIFI_MAX_AP 32
#define WIFI_SSID_MAX 32
#define WIFI_MAX_IFACE 2

typedef enum {
    WIFI_DISCONNECTED = 0,
    WIFI_SCANNING,
    WIFI_AUTHENTICATING,
    WIFI_ASSOCIATING,
    WIFI_CONNECTED,
} wifi_state_t;

typedef enum {
    WIFI_SEC_OPEN = 0,
    WIFI_SEC_WEP,
    WIFI_SEC_WPA,
    WIFI_SEC_WPA2,
    WIFI_SEC_WPA3,
} wifi_sec_t;

typedef struct {
    char ssid[WIFI_SSID_MAX+1];
    u8   bssid[6];
    int  signal;      /* -100..-20 dBm */
    u8   channel;     /* 1..14 */
    wifi_sec_t sec;
    bool present;
} wifi_ap_t;

typedef struct {
    bool present;
    char name[16];    /* wlan0 */
    u8   mac[6];
    wifi_state_t state;
    wifi_ap_t current_ap;
    u32 ip, mask, gw, dns; /* when connected, host order */
    bool hw_present;  /* real wireless PCI device */
    u16  vendor, device;
    u8   bus, dev, fn;
} wifi_iface_t;

void wifi_init(void);
bool wifi_present(void);
wifi_iface_t *wifi_get_iface(int idx);
int wifi_iface_count(void);

/* Scanning: populates internal list, returns count */
int wifi_scan(void);
int wifi_get_ap_count(void);
wifi_ap_t *wifi_get_ap(int idx);

/* Connection */
int wifi_connect(const char *ssid, const char *psk); /* 0=ok */
int wifi_disconnect(void);
wifi_state_t wifi_state(void);
const char *wifi_state_str(wifi_state_t s);

/* Syscall facing */
int wifi_scan_syscall(void);
int wifi_connect_syscall(const char *ssid, const char *psk);
int wifi_status_syscall(char *out, u32 cap); /* writes human readable status */

/* For net integration: when wifi connected, use its ip config */
void wifi_poll(void); /* called from net_service */

/* PCI helper - called from pci.c */
void wifi_pci_notify(u16 vendor, u16 device, u8 bus, u8 dev, u8 fn, u8 class_, u8 subclass);
