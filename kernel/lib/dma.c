/* Yart OS - 32-bit DMA memory (physically contiguous, below 4 GiB). */
#include <yart/types.h>
#include <yart/mm.h>
#include <yart/string.h>
#include <yart/dma.h>

void *dma_alloc32(size_t size, paddr_t *phys_out) {
    size_t np = PAGE_ALIGN_UP(size) / PAGE_SIZE;
    paddr_t phys = pmm_alloc_pages_below(np, 0x100000000ULL);
    if (!phys) return NULL;
    void *v = phys_to_virt(phys);
    memset(v, 0, np * PAGE_SIZE);
    if (phys_out) *phys_out = phys;
    return v;
}

void dma_free32(void *vaddr, size_t size) {
    if (!vaddr) return;
    size_t np = PAGE_ALIGN_UP(size) / PAGE_SIZE;
    pmm_free_pages(virt_to_phys(vaddr), np);
}

int dma_selftest(void) {
    paddr_t phys = 0;
    size_t total = 3 * PAGE_SIZE + 123;      /* deliberately not page-aligned */
    void *v = dma_alloc32(total, &phys);
    if (!v) return 1;
    if (phys & PAGE_MASK) { dma_free32(v, total); return 2; }          /* not aligned */
    if (phys >= 0x100000000ULL) { dma_free32(v, total); return 3; }    /* not < 4 GiB */
    if (virt_to_phys(v) != phys) { dma_free32(v, total); return 4; }   /* HHDM mismatch */
    u8 *p = v;
    for (size_t i = 0; i < total; i++) p[i] = (u8)(i * 7 + 1);
    for (size_t i = 0; i < total; i++)
        if (p[i] != (u8)(i * 7 + 1)) { dma_free32(v, total); return 5; }
    dma_free32(v, total);
    return 0;
}
