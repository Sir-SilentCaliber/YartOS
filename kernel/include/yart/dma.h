#pragma once
#include <yart/types.h>

/* 32-bit DMA memory: physically contiguous, below 4 GiB, kernel-accessible
 * via the HHDM mapping.  Needed by PCIe devices whose DMA engines only take
 * 32-bit addresses (e.g. the RTL8822CE descriptor rings). */
void *dma_alloc32(size_t size, paddr_t *phys_out);
void  dma_free32(void *vaddr, size_t size);

int dma_selftest(void);   /* 0 = ok */
