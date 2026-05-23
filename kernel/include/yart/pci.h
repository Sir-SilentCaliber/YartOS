#pragma once
#include <yart/types.h>
extern bool g_nic_present, g_audio_present;
void        pci_init(void);
const char *pci_nic_name(void);
const char *pci_audio_name(void);
