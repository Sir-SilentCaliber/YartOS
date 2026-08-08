/* Yart OS - PCI bus enumerator with WiFi detection.
 *
 * Walks bus 0..3, device 0..31, function 0..7 via legacy config space
 * at 0xCF8/0xCFC and records NIC, audio, and WiFi devices.
 * WiFi detection added for full wireless support.
 */
#include <yart/types.h>
#include <yart/io.h>
#include <yart/console.h>
#include <yart/wifi.h>

#define PCI_CFG_ADDR 0xCF8
#define PCI_CFG_DATA 0xCFC

static u32 pci_cfg_read32(u8 bus, u8 dev, u8 fn, u8 off) {
    u32 addr = (1U << 31) | ((u32)bus << 16) | ((u32)dev << 11)
             | ((u32)fn << 8)  | (off & 0xFC);
    outl(PCI_CFG_ADDR, addr);
    return inl(PCI_CFG_DATA);
}

typedef struct {
    u16 vendor, device;
    u8  class_, subclass_;
    u8  bus, dev, fn;
} pci_dev_t;

#define MAX_PCI 64
static pci_dev_t g_pci[MAX_PCI];
static int       g_pci_count;
static pci_dev_t g_nic_dev;
static pci_dev_t g_audio_dev;
static pci_dev_t g_wifi_dev;
bool             g_nic_present;
bool             g_audio_present;
bool             g_wifi_present;

void pci_init(void) {
    g_pci_count = 0;
    for (int bus = 0; bus < 4; bus++) {
        for (int d = 0; d < 32; d++) {
            for (int f = 0; f < 8; f++) {
                u32 w0 = pci_cfg_read32(bus, d, f, 0x00);
                if ((w0 & 0xFFFF) == 0xFFFF) continue;
                u32 w2 = pci_cfg_read32(bus, d, f, 0x08);
                u8 class_    = (w2 >> 24) & 0xFF;
                u8 subclass_ = (w2 >> 16) & 0xFF;
                u16 vendor = w0 & 0xFFFF;
                u16 device = (w0 >> 16) & 0xFFFF;
                if (g_pci_count < MAX_PCI) {
                    pci_dev_t *p = &g_pci[g_pci_count++];
                    p->vendor = vendor;
                    p->device = device;
                    p->class_ = class_;
                    p->subclass_ = subclass_;
                    p->bus = bus; p->dev = d; p->fn = f;
                }
                /* NIC: class 0x02 subclass 0x00 = Ethernet */
                if (class_ == 0x02 && subclass_ == 0x00 && !g_nic_present) {
                    g_nic_dev.vendor = vendor;
                    g_nic_dev.device = device;
                    g_nic_dev.bus = bus; g_nic_dev.dev = d; g_nic_dev.fn = f;
                    g_nic_present = true;
                }
                /* WiFi: class 0x02 subclass 0x80 or known wireless vendors */
                if (class_ == 0x02) {
                    bool is_wifi = false;
                    if (subclass_ == 0x80) is_wifi = true;
                    if (vendor==0x8086 && ((device>=0x0080 && device<=0x08FF) || device>=0x4220)) is_wifi = true;
                    if (vendor==0x10EC && (device==0x8187 || device==0x8192 || device==0x8723 || device==0x8821)) is_wifi = true;
                    if (vendor==0x168C) is_wifi = true;
                    if (vendor==0x14E4) is_wifi = true;
                    if (is_wifi && !g_wifi_present) {
                        g_wifi_dev.vendor = vendor; g_wifi_dev.device = device;
                        g_wifi_dev.bus = bus; g_wifi_dev.dev = d; g_wifi_dev.fn = f;
                        g_wifi_present = true;
                        kprintf("pci: WiFi %04x:%04x at %x:%x.%x\n", vendor, device, bus, d, f);
                    }
                    /* Notify wifi subsystem for any network device */
                    wifi_pci_notify(vendor, device, bus, d, f, class_, subclass_);
                }
                /* Audio: class 0x04 */
                if (class_ == 0x04 && !g_audio_present) {
                    g_audio_dev.vendor = vendor;
                    g_audio_dev.device = device;
                    g_audio_dev.bus = bus; g_audio_dev.dev = d; g_audio_dev.fn = f;
                    g_audio_present = true;
                }
                if (f == 0 && !(pci_cfg_read32(bus, d, 0, 0x0C) & 0x00800000))
                    break;
            }
        }
    }
    kprintf("pci: enumerated %d device(s) (NIC=%d Audio=%d WiFi=%d)\n",
            g_pci_count, g_nic_present, g_audio_present, g_wifi_present);
    if (g_nic_present)
        kprintf("pci: NIC %04x:%04x at %x:%x.%x\n",
                g_nic_dev.vendor, g_nic_dev.device,
                g_nic_dev.bus, g_nic_dev.dev, g_nic_dev.fn);
    if (g_audio_present)
        kprintf("pci: audio %04x:%04x at %x:%x.%x\n",
                g_audio_dev.vendor, g_audio_dev.device,
                g_audio_dev.bus, g_audio_dev.dev, g_audio_dev.fn);
    if (g_wifi_present)
        kprintf("pci: WiFi %04x:%04x at %x:%x.%x\n",
                g_wifi_dev.vendor, g_wifi_dev.device,
                g_wifi_dev.bus, g_wifi_dev.dev, g_wifi_dev.fn);
}

const char *pci_nic_name(void) {
    if (!g_nic_present) return 0;
    if (g_nic_dev.vendor == 0x10EC && g_nic_dev.device == 0x8139) return "RTL8139";
    if (g_nic_dev.vendor == 0x8086 && (g_nic_dev.device == 0x100E ||
                                        g_nic_dev.device == 0x10D3)) return "e1000";
    if (g_nic_dev.vendor == 0x1AF4) return "virtio-net";
    return "NIC";
}
const char *pci_audio_name(void) {
    if (!g_audio_present) return 0;
    if (g_audio_dev.vendor == 0x8086 && g_audio_dev.device == 0x2415) return "AC97";
    if (g_audio_dev.vendor == 0x8086 && g_audio_dev.device == 0x2668) return "HDA";
    return "audio";
}
const char *pci_wifi_name(void){
    if (!g_wifi_present) return 0;
    if (g_wifi_dev.vendor == 0x8086) return "iwlwifi";
    if (g_wifi_dev.vendor == 0x10EC) return "rtlwifi";
    if (g_wifi_dev.vendor == 0x168C) return "ath9k";
    if (g_wifi_dev.vendor == 0x14E4) return "b43";
    return "WiFi";
}
