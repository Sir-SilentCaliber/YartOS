/* YartOS rtw88 port - PCI transport for the RTL8822CE (10ec:c822).
 *
 * Bring-up path (real hardware):
 *   1. probe: find the device on the PCI bus, read BAR2, map it
 *   2. register MMIO accessors over the mapped BAR
 *   3. read REG_SYS_CFG1 (0x00F0) bits 15:12 -> chip version (proof of life:
 *      the chip answers over MMIO)
 *   4. hand off to rtw_download_firmware() (see rtw_fw.c)
 *
 * Transcribed from Linux rtw88 pci.c / reg.h (values verified against
 * mainline).  On QEMU there is no such device, so none of this runs there;
 * the firmware-download state machine is proven separately in rtw_fw.c's
 * selftest against a fake chip register file.
 */
#include <yart/types.h>
#include <yart/console.h>
#include <yart/string.h>
#include <yart/mm.h>
#include <yart/pci.h>
#include <yart/rtw88.h>
#include "rtw8822c_regs.h"

/* ---- MMIO accessors over the mapped BAR2 ---- */
static u8 rtw_pci_read8(rtw_dev_t *d, u32 a)  { return *(volatile u8 *)((u8 *)d->mmio + a); }
static u16 rtw_pci_read16(rtw_dev_t *d, u32 a) { return *(volatile u16 *)((u8 *)d->mmio + a); }
static u32 rtw_pci_read32(rtw_dev_t *d, u32 a) { return *(volatile u32 *)((u8 *)d->mmio + a); }
static void rtw_pci_write8(rtw_dev_t *d, u32 a, u8 v)  { *(volatile u8 *)((u8 *)d->mmio + a) = v; }
static void rtw_pci_write16(rtw_dev_t *d, u32 a, u16 v) { *(volatile u16 *)((u8 *)d->mmio + a) = v; }
static void rtw_pci_write32(rtw_dev_t *d, u32 a, u32 v) { *(volatile u32 *)((u8 *)d->mmio + a) = v; }
static void rtw_pci_sleep_ms(rtw_dev_t *d, u32 ms) { (void)d; (void)ms; /* polled via PIT on hw */ }

static const rtw_hci_ops_t rtw_pci_ops = {
    .read8  = rtw_pci_read8,  .read16 = rtw_pci_read16,  .read32 = rtw_pci_read32,
    .write8 = rtw_pci_write8, .write16 = rtw_pci_write16, .write32 = rtw_pci_write32,
    .sleep_ms = rtw_pci_sleep_ms,
};

/* Read the chip version from REG_SYS_CFG1 bits 15:12 (BIT_GET_CHIP_VER). */
static u32 rtw_pci_chip_version(rtw_dev_t *d) {
    return (rtw_read32(d, RTW_REG_SYS_CFG1) >> RTW_SHIFT_CHIP_VER) & RTW_MASK_CHIP_VER;
}

int rtw_pci_probe(rtw_dev_t *d, u16 vendor, u16 device, u8 bus, u8 dev, u8 fn) {
    d->vendor = vendor;
    d->device = device;
    d->bus = bus; d->dev = dev; d->fn = fn;

    /* BAR2 (config offset 0x18) holds the MMIO register space */
    u32 bar2 = pci_cfg_read32(bus, dev, fn, 0x18);
    if (!(bar2 & 0x1)) {                       /* must be a memory BAR */
        kprintf("rtw: BAR2 is not a memory BAR (0x%x)\n", bar2);
        return -1;
    }
    u32 bar_base = bar2 & 0xFFFFFFF0u;
    u32 bar_size = 0x10000;                    /* 64 KiB window (min)      */
    d->mmio = mmio_map((paddr_t)bar_base, bar_size);
    if (!d->mmio) {
        kprintf("rtw: failed to map BAR2 @ 0x%x\n", bar_base);
        return -1;
    }
    d->mmio_len = bar_size;
    d->ops = &rtw_pci_ops;

    u32 ver = rtw_pci_chip_version(d);
    kprintf("rtw: RTL8822CE (%04x:%04x) BAR2=0x%x mapped, chip version 0x%x\n",
            vendor, device, bar_base, ver);
    return 0;
}
