#pragma once
#include <yart/types.h>
extern bool g_nic_present, g_audio_present, g_wifi_present;
void        pci_init(void);
const char *pci_nic_name(void);
const char *pci_audio_name(void);
const char *pci_wifi_name(void);

/* Public config-space read (the rtw88 port needs BARs + revision). */
u32 pci_cfg_read32(u8 bus, u8 dev, u8 fn, u8 off);
