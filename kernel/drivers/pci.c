/* Yart OS - minimal PCI bus enumerator.
 *
 * Walks bus 0..0, device 0..31, function 0..7 via the legacy
 * configuration space registers at 0xCF8/0xCFC and records the
 * first NIC and the first audio device we find.  Real driver hookups
 * (RTL8139 / Intel HDA) are TODO; for now we just expose presence so
 * the top-bar applets can show "Network: detected" instead of N/A.
 */
#include <yart/types.h>
#include <yart/io.h>
#include <yart/console.h>

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

#define MAX_PCI 32
static pci_dev_t g_pci[MAX_PCI];
static int       g_pci_count;
static pci_dev_t g_nic_dev;
static pci_dev_t g_audio_dev;
bool             g_nic_present;
bool             g_audio_present;

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
                if (g_pci_count < MAX_PCI) {
                    pci_dev_t *p = &g_pci[g_pci_count++];
                    p->vendor = w0 & 0xFFFF;
                    p->device = (w0 >> 16) & 0xFFFF;
                    p->class_ = class_;
                    p->subclass_ = subclass_;
                    p->bus = bus; p->dev = d; p->fn = f;
                }
                /* NIC: class 0x02 */
                if (class_ == 0x02 && !g_nic_present) {
                    g_nic_dev.vendor = w0 & 0xFFFF;
                    g_nic_dev.device = (w0 >> 16) & 0xFFFF;
                    g_nic_dev.bus = bus; g_nic_dev.dev = d; g_nic_dev.fn = f;
                    g_nic_present = true;
                }
                /* Audio: class 0x04 */
                if (class_ == 0x04 && !g_audio_present) {
                    g_audio_dev.vendor = w0 & 0xFFFF;
                    g_audio_dev.device = (w0 >> 16) & 0xFFFF;
                    g_audio_dev.bus = bus; g_audio_dev.dev = d; g_audio_dev.fn = f;
                    g_audio_present = true;
                }
                if (f == 0 && !(pci_cfg_read32(bus, d, 0, 0x0C) & 0x00800000))
                    break;     /* not multi-function */
            }
        }
    }
    kprintf("pci: enumerated %d device(s) (NIC=%d Audio=%d)\n",
            g_pci_count, g_nic_present, g_audio_present);
    if (g_nic_present)
        kprintf("pci: NIC %04x:%04x at %x:%x.%x\n",
                g_nic_dev.vendor, g_nic_dev.device,
                g_nic_dev.bus, g_nic_dev.dev, g_nic_dev.fn);
    if (g_audio_present)
        kprintf("pci: audio %04x:%04x at %x:%x.%x\n",
                g_audio_dev.vendor, g_audio_dev.device,
                g_audio_dev.bus, g_audio_dev.dev, g_audio_dev.fn);
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
