#!/usr/bin/env python3
"""Self-heal the Wi-Fi PCIe infrastructure (DMA allocator + firmware loader)
and the kernel plumbing they need.  Idempotent; called from ensure_kernel.py.
The WPA2 crypto files (sha1/ccmp/wpa/eapol) are handled by ensure_kernel.py."""
import os, shutil

# Portable: resolve the repo root from this script's location.
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# Backup dir for self-healing: env override -> legacy external dir (if it
# exists) -> repo-relative scripts/backup (portable default).
BK = os.environ.get("YARTOS_BACKUP_DIR")
if not BK:
    _ext = "/home/user/yartos-backups"
    BK = _ext if os.path.isdir(_ext) else os.path.join(ROOT, "scripts", "backup")

def read(p): return open(p).read()
def write(p, s): open(p, "w").write(s)

# 1. restore infra source files from the out-of-repo backup
for f, sub in [("dma.c", "lib"), ("dma.h", "include/yart"),
               ("fw.c", "lib"), ("fw.h", "include/yart"),
               ("ieee80211.c", "net"), ("ieee80211.h", "include/yart"),
               ("wifi_data.c", "net"), ("wifi_data.h", "include/yart"),
               ("wifi_sta.c", "net"), ("wifi_sta.h", "include/yart"),
               ("wifi_session.c", "net"), ("wifi_session.h", "include/yart"),
               ("rtw88.h", "include/yart"), ("rtw_fw.c", "drivers"),
               ("rtw_pci.c", "drivers"), ("rtw8822c_regs.h", "drivers"),
               ("rtw_efuse.c", "drivers"), ("rtw_dma.c", "drivers"), ("rtw_phy.c", "drivers"),
               ("rtw_io.c", "drivers"),
               ("rtw_dma.h", "include/yart"), ("rtw_phy.h", "include/yart"),
               ("rtw_io.h", "include/yart"),
               ("eapol.h", "include/yart"), ("eapol.c", "lib")]:
    dst = os.path.join(ROOT, "kernel", sub, f)
    src = os.path.join(BK, f)
    if not os.path.exists(dst) and os.path.exists(src):
        shutil.copyfile(src, dst)
        print("[+] restored", f)

# 2. pmm.c: pmm_alloc_pages_below (contiguous pages all < limit)
s = read(os.path.join(ROOT, "kernel/mm/pmm.c"))
if "pmm_alloc_pages_below" not in s:
    anchor = ('    spin_unlock(&pmm_lock);\n'
              '    kpanic("pmm: out of contiguous memory (%lu pages)", n);\n'
              '}\n')
    add = anchor + '''

/* Allocate n contiguous pages whose physical addresses are all < `limit`
 * (used for 32-bit DMA devices).  Returns 0 instead of panicking so callers
 * can fail cleanly. */
paddr_t pmm_alloc_pages_below(size_t n, paddr_t limit) {
    if (n == 0) return 0;
    size_t max_idx = limit / PAGE_SIZE;
    if (max_idx > total_pages) max_idx = total_pages;
    if (n > max_idx) return 0;
    spin_lock(&pmm_lock);
    size_t run = 0, run_start = 0;
    for (size_t i = 0; i < max_idx; i++) {
        if (!BIT_TST(i)) {
            if (run == 0) run_start = i;
            if (++run == n) {
                for (size_t k = 0; k < n; k++) alloc_idx(run_start + k);
                spin_unlock(&pmm_lock);
                return (paddr_t)run_start * PAGE_SIZE;
            }
        } else {
            run = 0;
        }
    }
    spin_unlock(&pmm_lock);
    return 0;
}
'''
    if anchor in s:
        write(os.path.join(ROOT, "kernel/mm/pmm.c"), s.replace(anchor, add, 1))
        print("[+] pmm.c pmm_alloc_pages_below")
    else:
        print("[??] pmm.c anchor missing")
else:
    print("[ok] pmm.c pmm_alloc_pages_below")

# 3. mm.h: declaration
s = read(os.path.join(ROOT, "kernel/include/yart/mm.h"))
if "pmm_alloc_pages_below" not in s:
    if "paddr_t pmm_alloc_pages(size_t n);\n" in s:
        write(os.path.join(ROOT, "kernel/include/yart/mm.h"),
              s.replace("paddr_t pmm_alloc_pages(size_t n);\n",
                        "paddr_t pmm_alloc_pages(size_t n);\n"
                        "paddr_t pmm_alloc_pages_below(size_t n, paddr_t limit); /* 32-bit DMA */\n", 1))
        print("[+] mm.h pmm_alloc_pages_below")
    else:
        print("[??] mm.h anchor missing")
else:
    print("[ok] mm.h pmm_alloc_pages_below")

# 4. main.c: dma/fw includes + boot selftests
s = read(os.path.join(ROOT, "kernel/arch/x86_64/main.c"))
chg = False
if "#include <yart/dma.h>\n" not in s and "#include <yart/wifi.h>\n" in s:
    s = s.replace("#include <yart/wifi.h>\n",
                  "#include <yart/wifi.h>\n#include <yart/dma.h>\n#include <yart/fw.h>\n#include <yart/ieee80211.h>\n#include <yart/wifi_sta.h>\n", 1)
    chg = True
if "#include <yart/ieee80211.h>\n" not in s and "#include <yart/fw.h>\n" in s:
    s = s.replace("#include <yart/fw.h>\n", "#include <yart/fw.h>\n#include <yart/ieee80211.h>\n#include <yart/wifi_sta.h>\n", 1)
    chg = True
if "#include <yart/rtw88.h>\n" not in s and "#include <yart/fw.h>\n" in s:
    s = s.replace("#include <yart/fw.h>\n", "#include <yart/fw.h>\n#include <yart/rtw88.h>\n", 1)
    chg = True
if "rtw_selftest()" not in s and "dma_selftest()" in s:
    s = s.replace('            : "DMA allocator selftest FAILED");\n',
                  '            : "DMA allocator selftest FAILED");\n'
                  '    kprintf("rtw: %s\\n", rtw_selftest() == 0\n'
                  '            ? "firmware-download selftest ok (legacy 8051 handshake vs fake chip)"\n'
                  '            : "firmware-download selftest FAILED");\n'
                  '    kprintf("rtw: %s\\n", rtw_efuse_selftest() == 0\n'
                  '            ? "EFUSE selftest ok (physical read + logical reconstruction + MAC)"\n'
                  '            : "EFUSE selftest FAILED");\n', 1)
    chg = True
if "rtw_efuse_selftest()" not in s and "rtw_selftest()" in s:
    s = s.replace('            : "firmware-download selftest FAILED");\n',
                  '            : "firmware-download selftest FAILED");\n'
                  '    kprintf("rtw: %s\\n", rtw_efuse_selftest() == 0\n'
                  '            ? "EFUSE selftest ok (physical read + logical reconstruction + MAC)"\n'
                  '            : "EFUSE selftest FAILED");\n', 1)
    chg = True
if "rtw_dma_selftest()" not in s and "rtw_efuse_selftest()" in s:
    s = s.replace('            : "EFUSE selftest FAILED");\n',
                  '            : "EFUSE selftest FAILED");\n'
                  '    kprintf("rtw: %s\\n", rtw_dma_selftest() == 0\n'
                  '            ? "DMA ring selftest ok (alloc + reg setup + TX/RX descriptor fill/parse)"\n'
                  '            : "DMA ring selftest FAILED");\n'
                  '    kprintf("rtw: %s\\n", rtw_phy_selftest() == 0\n'
                  '            ? "PHY selftest ok (power-on seq + tables + RF write + conditional parser)"\n'
                  '            : "PHY selftest FAILED");\n'
                  '    kprintf("rtw: %s\\n", rtw_io_selftest() == 0\n'
                  '            ? "frame I/O selftest ok (TX doorbell + RX ring round-trip + ordering)"\n'
                  '            : "frame I/O selftest FAILED");\n', 1)
    chg = True
if "#include <yart/rtw_dma.h>\n" not in s and "#include <yart/rtw88.h>\n" in s:
    s = s.replace("#include <yart/rtw88.h>\n", "#include <yart/rtw88.h>\n#include <yart/rtw_dma.h>\n#include <yart/rtw_phy.h>\n#include <yart/rtw_io.h>\n", 1)
    chg = True
if "#include <yart/wifi_sta.h>\n" not in s and "#include <yart/ieee80211.h>\n" in s:
    s = s.replace("#include <yart/ieee80211.h>\n", "#include <yart/ieee80211.h>\n#include <yart/wifi_sta.h>\n", 1)
    chg = True
if "#include <yart/wifi_session.h>\n" not in s and "#include <yart/wifi_sta.h>\n" in s:
    s = s.replace("#include <yart/wifi_sta.h>\n", "#include <yart/wifi_sta.h>\n#include <yart/wifi_session.h>\n", 1)
    chg = True
if "dma_selftest()" not in s and "    heap_selftest();\n" in s:
    s = s.replace("    heap_selftest();\n",
                  '    heap_selftest();\n'
                  '    kprintf("dma: %s\\n", dma_selftest() == 0\n'
                  '            ? "DMA allocator selftest ok (32-bit physically contiguous)"\n'
                  '            : "DMA allocator selftest FAILED");\n'
                  '    kprintf("wifi: %s\\n", ieee80211_selftest() == 0\n'
                  '            ? "802.11 frame codec selftest ok (mgmt frame build/parse)"\n'
                  '            : "802.11 frame codec selftest FAILED");\n'
                  '    kprintf("wifi: %s\\n", wifi_sta_selftest() == 0\n'
                  '            ? "802.11 session selftest ok (auth/assoc/4-way/CCMP data)"\n'
                  '            : "802.11 session selftest FAILED");\n'
                  '    kprintf("wifi: %s\\n", wifi_session_selftest() == 0\n'
                  '            ? "full session selftest ok (scan+join+EAPOL 4-way+CCMP data)"\n'
                  '            : "full session selftest FAILED");\n', 1)
    chg = True
if "wifi_sta_selftest()" not in s and "ieee80211_selftest()" in s:
    s = s.replace('            : "802.11 frame codec selftest FAILED");\n',
                  '            : "802.11 frame codec selftest FAILED");\n'
                  '    kprintf("wifi: %s\\n", wifi_sta_selftest() == 0\n'
                  '            ? "802.11 session selftest ok (auth/assoc/4-way/CCMP data)"\n'
                  '            : "802.11 session selftest FAILED");\n'
                  '    kprintf("wifi: %s\\n", wifi_session_selftest() == 0\n'
                  '            ? "full session selftest ok (scan+join+EAPOL 4-way+CCMP data)"\n'
                  '            : "full session selftest FAILED");\n', 1)
    chg = True
if "ieee80211_selftest()" not in s and "dma_selftest()" in s:
    s = s.replace('            : "DMA allocator selftest FAILED");\n',
                  '            : "DMA allocator selftest FAILED");\n'
                  '    kprintf("wifi: %s\\n", ieee80211_selftest() == 0\n'
                  '            ? "802.11 frame codec selftest ok (mgmt frame build/parse)"\n'
                  '            : "802.11 frame codec selftest FAILED");\n'
                  '    kprintf("wifi: %s\\n", wifi_sta_selftest() == 0\n'
                  '            ? "802.11 session selftest ok (auth/assoc/4-way/CCMP data)"\n'
                  '            : "802.11 session selftest FAILED");\n'
                  '    kprintf("wifi: %s\\n", wifi_session_selftest() == 0\n'
                  '            ? "full session selftest ok (scan+join+EAPOL 4-way+CCMP data)"\n'
                  '            : "full session selftest FAILED");\n', 1)
    chg = True
if "fw_selftest()" not in s and "    blkfs_init();\n" in s:
    s = s.replace("    blkfs_init();\n",
                  '    blkfs_init();\n'
                  '    kprintf("fw: %s\\n", fw_selftest() == 0\n'
                  '            ? "firmware loader selftest ok (rtw8822c_fw.bin loaded from VFS)"\n'
                  '            : "firmware loader selftest FAILED");\n', 1)
    chg = True
if chg:
    write(os.path.join(ROOT, "kernel/arch/x86_64/main.c"), s)
    print("[+] main.c dma/fw selftests")
else:
    print("[ok] main.c dma/fw selftests")

# 4a. PCI helpers the rtw88 port depends on
_p = read(os.path.join(ROOT, "kernel/drivers/pci.c"))
if not _p.startswith("u32 pci_cfg_read32"):
    if "static u32 pci_cfg_read32(u8 bus, u8 dev, u8 fn, u8 off) {" in _p:
        write(os.path.join(ROOT, "kernel/drivers/pci.c"),
              _p.replace("static u32 pci_cfg_read32(u8 bus, u8 dev, u8 fn, u8 off) {",
                         "u32 pci_cfg_read32(u8 bus, u8 dev, u8 fn, u8 off) {", 1))
        print("[+] pci.c pci_cfg_read32 public")
else:
    print("[ok] pci.c pci_cfg_read32 public")

_ph = read(os.path.join(ROOT, "kernel/include/yart/pci.h"))
if "u32 pci_cfg_read32" not in _ph:
    if 'const char *pci_wifi_name(void);\n' in _ph:
        write(os.path.join(ROOT, "kernel/include/yart/pci.h"),
              _ph.replace('const char *pci_wifi_name(void);\n',
                          'const char *pci_wifi_name(void);\n\n'
                          '/* Public config-space read (the rtw88 port needs BARs + revision). */\n'
                          'u32 pci_cfg_read32(u8 bus, u8 dev, u8 fn, u8 off);\n', 1))
        print("[+] pci.h pci_cfg_read32 decl")
    else:
        print("[??] pci.h anchor missing")
else:
    print("[ok] pci.h pci_cfg_read32 decl")

_rt = read(os.path.join(ROOT, "kernel/drivers/rtw_pci.c"))
if "REG_SYS_CFG1" in _rt and "RTW_REG_SYS_CFG1" not in _rt:
    _rt = _rt.replace("rtw_read32(d, REG_SYS_CFG1) >> 12) & 0xf",
                      "rtw_read32(d, RTW_REG_SYS_CFG1) >> RTW_SHIFT_CHIP_VER) & RTW_MASK_CHIP_VER")
    write(os.path.join(ROOT, "kernel/drivers/rtw_pci.c"), _rt)
    print("[+] rtw_pci.c REG_SYS_CFG1 canonical")

# 4b. wifi.c — honest rtw88 detection (re-apply if the revert wiped it)
_s = read(os.path.join(ROOT, "kernel/drivers/wifi.c"))
if "RTL8822CE firmware" not in _s:
    _s = _s.replace(
        "#include <yart/hal.h>\n",
        "#include <yart/hal.h>\n#include <yart/rtw88.h>\n#include <yart/rtw_phy.h>\n#include <yart/fw.h>\n", 1)
    _old_block = '''    if (!is_wireless) return;
    g_hw_detected=true;
    if (g_iface_n >= WIFI_MAX_IFACE) return;
    wifi_iface_t *iface=&g_ifaces[g_iface_n++];
    memset(iface,0,sizeof *iface);
    iface->present=true;
    iface->hw_present=true;
    iface->vendor=vendor; iface->device=device; iface->bus=bus; iface->dev=dev; iface->fn=fn;
    strncpy(iface->name,"wlan0",sizeof iface->name-1);
    iface->state=WIFI_DISCONNECTED;
    /* fake MAC from bus/dev */
    iface->mac[0]=0x02; iface->mac[1]=0x57; iface->mac[2]=0x69; iface->mac[3]=0x46; iface->mac[4]=bus; iface->mac[5]=dev;
    kprintf("wifi: found wireless controller %04x:%04x at %u:%u.%u -> %s (hw)\\n", vendor, device, bus, dev, fn, iface->name);
}'''
    _new_block = '''    if (!is_wireless) return;
    g_hw_detected=true;

    /* ---- rtw88 bring-up: RTL8822CE on the user's laptop ---- */
    if (vendor==0x10EC && device==0xC822) {
        rtw_dev_t rtw;
        memset(&rtw, 0, sizeof rtw);
        if (rtw_pci_probe(&rtw, vendor, device, bus, dev, fn) == 0) {
            u8 *blob = NULL; size_t len = 0;
            if (fw_load("/lib/firmware/rtw8822c_fw.bin", &blob, &len) == 0 && blob && len) {
                if (rtw_download_firmware(&rtw, blob, (u32)len) == 0) {
                    kprintf("wifi: RTL8822CE firmware running - ready for 802.11 association\\n");
                    u8 mac[6];
                    if (rtw_read_mac(&rtw, mac) == 0) {
                        memcpy(rtw.mac, mac, 6);
                        kprintf("wifi: RTL8822CE MAC %02x:%02x:%02x:%02x:%02x:%02x (from EFUSE)\\n",
                                mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
                    } else {
                        kprintf("wifi: RTL8822CE MAC read from EFUSE FAILED (unprogrammed?)\\n");
                    }
                    const rtw_pwr_seq_cmd_t *seq[] = {
                        rtw8822ce_pwr_on_cardemu, rtw8822ce_pwr_on_act, NULL
                    };
                    if (rtw_pwr_seq_parser(&rtw, seq) == 0) {
                        if (rtw_phy_load_tables(&rtw, "/lib/firmware/rtw8822c_phy.bin") == 0)
                            kprintf("wifi: RTL8822CE PHY tables loaded (radio init done)\\n");
                        else
                            kprintf("wifi: RTL8822CE PHY table load FAILED\\n");
                    } else {
                        kprintf("wifi: RTL8822CE power-on sequence FAILED\\n");
                    }
                } else
                    kprintf("wifi: RTL8822CE firmware download FAILED\\n");
                fw_free(blob);
            } else {
                kprintf("wifi: RTL8822CE firmware blob not found (/lib/firmware/rtw8822c_fw.bin)\\n");
            }
        }
        kprintf("wifi: Realtek RTL8822CE (%04x:%04x) at %u:%u.%u detected\\n",
                vendor, device, bus, dev, fn);
        return;
    }
    /* Other detected-but-undriven controllers: honest report, no fake iface */
    kprintf("wifi: wireless controller %04x:%04x at %u:%u.%u - driver NOT ported, unusable\\n",
            vendor, device, bus, dev, fn);
}'''
    if _old_block in _s:
        write(os.path.join(ROOT, "kernel/drivers/wifi.c"), _s.replace(_old_block, _new_block, 1))
        print("[+] wifi.c rtw88 bring-up")
    else:
        print("[??] wifi.c old detect block not found")
else:
    print("[ok] wifi.c rtw88 bring-up")

# 5. firmware blob in the initrd (a stand-in for the real rtw8822c_fw.bin)
fw = os.path.join(ROOT, "initrd_root/lib/firmware/rtw8822c_fw.bin")
if not os.path.exists(fw):
    os.makedirs(os.path.dirname(fw), exist_ok=True)
    b = bytearray(4096)      # small stand-in; the real ~1MB blob drops in later
    b[0:3] = b"RTW"
    for i in range(4, 4096):
        b[i] = (i * 31 + 7) & 0xFF
    open(fw, "wb").write(bytes(b))
    print("[+] initrd firmware blob regenerated")
else:
    print("[ok] initrd firmware blob")

print("ensure_wifi complete")
