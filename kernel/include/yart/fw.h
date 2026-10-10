#pragma once
#include <yart/types.h>

/* Firmware loader: read a blob from the OS filesystem (initrd/disk) into a
 * kernel heap buffer.  The RTL8822CE is a soft-MAC: it must DMA-download
 * rtw8822c_fw.bin before it can associate. */
int  fw_load(const char *path, u8 **out, size_t *out_len);   /* 0 = ok */
void fw_free(void *buf);

int fw_selftest(void);   /* 0 = ok */
