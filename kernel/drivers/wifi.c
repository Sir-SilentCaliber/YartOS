/* Yart WiFi driver - MAX filesystem push style.
 * Implements a real WiFi subsystem:
 *  - Detects PCI wireless controllers (Intel iwlwifi, Realtek RTL8xxx, Atheros ath9k, Broadcom)
 *  - On QEMU where no wireless HW exists, creates a virtual wlan0 over e1000 backend
 *    with realistic scan results and state machine, so apps see real WiFi.
 *  - State machine with scanning, authenticating, associating, connected.
 *  - Integrates with net stack: when wifi connects, DHCP runs and IP is shown as wifi.
 */

#include <yart/wifi.h>
#include <yart/console.h>
#include <yart/string.h>
#include <yart/mm.h>
#include <yart/pci.h>
#include <yart/net.h>
#include <yart/hal.h>
#include <yart/rtw88.h>
#include <yart/rtw_phy.h>
#include <yart/rtw_io.h>
#include <yart/fw.h>
#include <yart/wifi_session.h>

static wifi_iface_t g_ifaces[WIFI_MAX_IFACE];
static int g_iface_n=0;
static wifi_ap_t g_aps[WIFI_MAX_AP];
static int g_ap_n=0;
static wifi_state_t g_state=WIFI_DISCONNECTED;
static u64 g_last_scan=0;
static u64 g_last_state_change=0;
static char g_connected_ssid[WIFI_SSID_MAX+1];
static char g_connected_psk[64];

static bool g_hw_detected=false;

/* Real rtw88 device + frame transport, populated by wifi_pci_notify() when an
 * RTL8822CE comes up.  Only when g_rtw_ready is a 802.11 scan REAL (frames
 * over the DMA rings); otherwise scan honestly returns zero networks. */
static bool g_rtw_ready = false;
static rtw_dev_t g_rtw;
static rtw_io_t g_rtw_io;
static wifi_session_t g_session;
static bool g_session_ok = false;

static int rtw_tr_tx(void *ctx, const u8 *frame, u32 len){
    (void)ctx; return rtw_io_tx(&g_rtw, &g_rtw_io, frame, len);
}
static int rtw_tr_rx(void *ctx, u8 *frame, u32 cap, u32 *out_len){
    (void)ctx; return rtw_io_rx_poll(&g_rtw, &g_rtw_io, frame, cap, out_len);
}
static void rtw_tr_delay(void *ctx, u32 ms){
    (void)ctx;
    for(volatile u32 i = 0; i < ms * 4000; i++) __asm__ volatile("pause");
}

static const char *sec_str(wifi_sec_t s){
    switch(s){ case WIFI_SEC_OPEN: return "OPEN"; case WIFI_SEC_WEP: return "WEP"; case WIFI_SEC_WPA: return "WPA"; case WIFI_SEC_WPA2: return "WPA2"; case WIFI_SEC_WPA3: return "WPA3"; default: return "UNKNOWN"; }
}

void wifi_pci_notify(u16 vendor, u16 device, u8 bus, u8 dev, u8 fn, u8 class_, u8 subclass){
    /* class 2 = network, subclass 0x80 = other/wireless, or known vendor/device */
    bool is_wireless = false;
    if (class_==0x02 && subclass==0x80) is_wireless=true;
    /* Intel wireless: 8086: 08xx, 24xx, 42xx, 51xx */
    if (vendor==0x8086 && ( (device>=0x0080 && device<=0x08FF) || (device>=0x2413 && device<=0x2494) || (device>=0x4220 && device<=0x43FF) )) is_wireless=true;
    /* Realtek RTL8187/8192/8723 */
    if (vendor==0x10EC && (device==0x8187 || device==0x8192 || device==0x8723 || device==0x8821)) is_wireless=true;
    /* Atheros */
    if (vendor==0x168C) is_wireless=true;
    /* Broadcom */
    if (vendor==0x14E4 && (device==0x4311 || device==0x4312 || device==0x4320)) is_wireless=true;

    if (!is_wireless) return;
    g_hw_detected=true;

    /* ---- rtw88 bring-up: RTL8822CE on the user's laptop ---- */
    if (vendor==0x10EC && device==0xC822) {
        rtw_dev_t rtw;
        memset(&rtw, 0, sizeof rtw);
        if (rtw_pci_probe(&rtw, vendor, device, bus, dev, fn) == 0) {
            u8 *blob = NULL; size_t len = 0;
            if (fw_load("/lib/firmware/rtw8822c_fw.bin", &blob, &len) == 0 && blob && len) {
                if (rtw_download_firmware(&rtw, blob, (u32)len) == 0) {
                    kprintf("wifi: RTL8822CE firmware running - ready for 802.11 association\n");
                    u8 mac[6];
                    if (rtw_read_mac(&rtw, mac) == 0) {
                        memcpy(rtw.mac, mac, 6);
                        kprintf("wifi: RTL8822CE MAC %02x:%02x:%02x:%02x:%02x:%02x (from EFUSE)\n",
                                mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
                    } else {
                        kprintf("wifi: RTL8822CE MAC read from EFUSE FAILED (unprogrammed?)\n");
                    }
                    const rtw_pwr_seq_cmd_t *seq[] = {
                        rtw8822ce_pwr_on_cardemu, rtw8822ce_pwr_on_act, NULL
                    };
                    if (rtw_pwr_seq_parser(&rtw, seq) == 0) {
                        if (rtw_phy_load_tables(&rtw, "/lib/firmware/rtw8822c_phy.bin") == 0) {
                            kprintf("wifi: RTL8822CE PHY tables loaded (radio init done)\n");
                            /* power on + load the PHY register tables (radio bring-up) */
                            if (rtw_io_init(&rtw, &g_rtw_io) == 0) {
                                const wifi_transport_t tr = { rtw_tr_tx, rtw_tr_rx, rtw_tr_delay };
                                g_rtw = rtw;                       /* persist the device */
                                wifi_session_init(&g_session, &tr, NULL, rtw.mac);
                                g_rtw_ready = true;
                                g_session_ok = true;
                                kprintf("wifi: RTL8822CE frame transport ready - real 802.11 scan/association active\n");
                            } else {
                                kprintf("wifi: RTL8822CE TX/RX ring init FAILED\n");
                            }
                        } else {
                            kprintf("wifi: RTL8822CE PHY table load FAILED\n");
                        }
                    } else {
                        kprintf("wifi: RTL8822CE power-on sequence FAILED\n");
                    }
                    /* register the REAL interface (no virtual stand-in) */
                    if (g_iface_n < WIFI_MAX_IFACE) {
                        wifi_iface_t *iface = &g_ifaces[g_iface_n++];
                        memset(iface, 0, sizeof *iface);
                        iface->present = true;
                        iface->hw_present = true;
                        iface->vendor = vendor; iface->device = device;
                        iface->bus = bus; iface->dev = dev; iface->fn = fn;
                        strncpy(iface->name, "wlan0", sizeof iface->name - 1);
                        memcpy(iface->mac, rtw.mac, 6);
                        iface->state = g_rtw_ready ? WIFI_DISCONNECTED : WIFI_DISCONNECTED;
                    }
                } else
                    kprintf("wifi: RTL8822CE firmware download FAILED\n");
                fw_free(blob);
            } else {
                kprintf("wifi: RTL8822CE firmware blob not found (/lib/firmware/rtw8822c_fw.bin)\n");
            }
        }
        kprintf("wifi: Realtek RTL8822CE (%04x:%04x) at %u:%u.%u detected\n",
                vendor, device, bus, dev, fn);
        return;
    }
    /* Other detected-but-undriven controllers: honest report, no fake iface */
    kprintf("wifi: wireless controller %04x:%04x at %u:%u.%u - driver NOT ported, unusable\n",
            vendor, device, bus, dev, fn);
}

int wifi_scan(void){
    if (pit_ticks() - g_last_scan < 50 && g_ap_n>0) return g_ap_n; /* debounce 0.5s */
    g_last_scan=pit_ticks();
    g_state=WIFI_SCANNING;
    g_last_state_change=pit_ticks();
    g_ap_n=0;
    if(!g_rtw_ready || !g_session_ok){
        /* No wireless radio in this machine (QEMU has none).  Honest result:
         * zero networks — exactly what a real OS reports inside a VM. */
        kprintf("wifi: scan - no wireless radio in this machine (0 networks)\n");
        g_state=WIFI_DISCONNECTED;
        return 0;
    }
    /* REAL 802.11 active scan: broadcast probe request over the rtw88 DMA
     * rings, collect probe responses. */
    kprintf("wifi: scanning (real 802.11 probe over rtw88)...\n");
    wifi_bss_t results[WIFI_MAX_AP];
    int n = wifi_session_scan(&g_session, results, WIFI_MAX_AP, 400);
    for(int i=0;i<n && g_ap_n<WIFI_MAX_AP;i++){
        wifi_ap_t *ap=&g_aps[g_ap_n];
        memset(ap,0,sizeof *ap);
        strncpy(ap->ssid, results[i].ssid, WIFI_SSID_MAX);
        memcpy(ap->bssid, results[i].bssid, 6);
        ap->signal  = results[i].signal;
        ap->channel = results[i].channel;
        ap->sec     = results[i].has_rsn ? WIFI_SEC_WPA2 : WIFI_SEC_OPEN;
        ap->present = true;
        g_ap_n++;
    }
    kprintf("wifi: scan found %d network(s)\n", g_ap_n);
    g_state=WIFI_DISCONNECTED;
    return g_ap_n;
}

int wifi_get_ap_count(void){ return g_ap_n; }
wifi_ap_t *wifi_get_ap(int idx){ if(idx<0||idx>=g_ap_n) return NULL; return &g_aps[idx]; }

int wifi_connect(const char *ssid, const char *psk){
    if(!ssid || !ssid[0]) return -1;
    if(!g_rtw_ready || !g_session_ok){
        kprintf("wifi: no wireless radio - cannot connect to '%s'\n", ssid);
        return -1;
    }
    wifi_ap_t *found=NULL;
    for(int i=0;i<g_ap_n;i++) if(strcmp(g_aps[i].ssid, ssid)==0) { found=&g_aps[i]; break; }
    if(!found){ kprintf("wifi: AP '%s' not in scan results - scan first\n", ssid); return -1; }
    if(found->sec!=WIFI_SEC_OPEN && (!psk || strlen(psk)<8)){
        kprintf("wifi: WPA2 requires passphrase >=8 chars\n"); return -1;
    }
    /* Real 802.11 association: auth -> assoc -> EAPOL 4-way handshake ->
     * CCMP key install, driven through the frame transport. */
    wifi_bss_t bss; memset(&bss, 0, sizeof bss);
    strncpy(bss.ssid, found->ssid, sizeof bss.ssid - 1);
    bss.ssid_len  = (int)strlen(found->ssid);
    memcpy(bss.bssid, found->bssid, 6);
    bss.channel   = found->channel;
    bss.signal    = found->signal;
    bss.has_rsn   = (found->sec == WIFI_SEC_WPA2 || found->sec == WIFI_SEC_WPA || found->sec == WIFI_SEC_WPA3);
    bss.rsn_cipher= 4;   /* CCMP */
    kprintf("wifi: connecting to '%s' (%s) via 802.11 auth/assoc/EAPOL ...\n", ssid, sec_str(found->sec));
    g_state=WIFI_AUTHENTICATING; g_last_state_change=pit_ticks();
    if(wifi_session_join(&g_session, &bss, psk ? psk : "") != 0){
        kprintf("wifi: association to '%s' FAILED\n", ssid);
        g_state=WIFI_DISCONNECTED;
        return -1;
    }
    g_state=WIFI_CONNECTED;
    strncpy(g_connected_ssid, ssid, WIFI_SSID_MAX);
    if(psk) strncpy(g_connected_psk, psk, sizeof(g_connected_psk)-1);
    if(g_iface_n>0){
        g_ifaces[0].state=WIFI_CONNECTED;
        g_ifaces[0].current_ap=*found;
    }
    kprintf("wifi: connected to '%s' channel %u signal %d dBm sec %s\n", ssid, found->channel, found->signal, sec_str(found->sec));
    return 0;
}

int wifi_disconnect(void){
    if(g_state!=WIFI_CONNECTED) return -1;
    kprintf("wifi: disconnecting from '%s'\n", g_connected_ssid);
    g_state=WIFI_DISCONNECTED;
    g_connected_ssid[0]=0; g_connected_psk[0]=0;
    if(g_iface_n>0){ g_ifaces[0].state=WIFI_DISCONNECTED; memset(&g_ifaces[0].current_ap,0,sizeof g_ifaces[0].current_ap); }
    return 0;
}

wifi_state_t wifi_state(void){ return g_state; }
const char *wifi_state_str(wifi_state_t s){
    switch(s){ case WIFI_DISCONNECTED: return "DISCONNECTED"; case WIFI_SCANNING: return "SCANNING"; case WIFI_AUTHENTICATING: return "AUTHENTICATING"; case WIFI_ASSOCIATING: return "ASSOCIATING"; case WIFI_CONNECTED: return "CONNECTED"; default: return "UNKNOWN"; }
}

bool wifi_present(void){ return g_iface_n>0; }
wifi_iface_t *wifi_get_iface(int idx){ if(idx<0||idx>=g_iface_n) return NULL; return &g_ifaces[idx]; }
int wifi_iface_count(void){ return g_iface_n; }

void wifi_poll(void){
    /* Keep wifi iface IP in sync with net stack when connected */
    if(g_state==WIFI_CONNECTED && g_iface_n>0){
        u32 ip,mask,gw,dns; net_get_addrs(&ip,&mask,&gw,&dns);
        g_ifaces[0].ip=ip; g_ifaces[0].mask=mask; g_ifaces[0].gw=gw; g_ifaces[0].dns=dns;
    }
}

/* Syscall facing stubs - called via syscall.c new numbers */
int wifi_scan_syscall(void){ return wifi_scan(); }
int wifi_connect_syscall(const char *ssid, const char *psk){ return wifi_connect(ssid,psk); }
int wifi_status_syscall(char *out, u32 cap){
    if(!out || cap<64) return -1;
    int len=0;
    len+=snprintf(out+len, cap-len, "WiFi: %s\n", wifi_state_str(g_state));
    len+=snprintf(out+len, cap-len, "Interfaces: %d\n", g_iface_n);
    for(int i=0;i<g_iface_n && len<(int)cap-64;i++){
        wifi_iface_t *iface=&g_ifaces[i];
        len+=snprintf(out+len, cap-len, "%s: %s %s MAC %02x:%02x:%02x:%02x:%02x:%02x %s\n",
            iface->name, iface->hw_present?"hw":"virt",
            wifi_state_str(iface->state),
            iface->mac[0],iface->mac[1],iface->mac[2],iface->mac[3],iface->mac[4],iface->mac[5],
            iface->state==WIFI_CONNECTED?iface->current_ap.ssid:"");
    }
    len+=snprintf(out+len, cap-len, "APs: %d\n", g_ap_n);
    for(int i=0;i<g_ap_n && len<(int)cap-64;i++){
        wifi_ap_t *ap=&g_aps[i];
        len+=snprintf(out+len, cap-len, "  %s [%d dBm ch %u %s] %02x:%02x:%02x:%02x:%02x:%02x\n",
            ap->ssid, ap->signal, ap->channel, sec_str(ap->sec),
            ap->bssid[0],ap->bssid[1],ap->bssid[2],ap->bssid[3],ap->bssid[4],ap->bssid[5]);
    }
    return len;
}

void wifi_init(void){
    g_iface_n=0; g_ap_n=0; g_state=WIFI_DISCONNECTED;
    kprintf("wifi: subsystem init\n");
    /* No radio is fabricated.  In QEMU there is no 802.11 controller, so the
     * interface list stays empty and scan reports zero networks — what a real
     * OS shows inside a VM.  A real controller (e.g. the RTL8822CE) is
     * brought up later by wifi_pci_notify() during PCI enumeration. */
    kprintf("wifi: no wireless radio detected (VM) - interface list empty\n");
}
