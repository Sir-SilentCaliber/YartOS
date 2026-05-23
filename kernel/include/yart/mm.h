#pragma once
#include <yart/types.h>
#include <yart/limine.h>

/* HHDM helpers */
extern u64 g_hhdm_offset;
static ALWAYS_INLINE void *phys_to_virt(paddr_t p) { return (void *)(p + g_hhdm_offset); }
static ALWAYS_INLINE paddr_t virt_to_phys(void *v) { return (paddr_t)v - g_hhdm_offset; }

/* PMM */
void    pmm_init(struct limine_memmap_response *mm);
paddr_t pmm_alloc_page(void);
paddr_t pmm_alloc_pages(size_t n);
void    pmm_free_page(paddr_t p);
size_t  pmm_total_pages(void);
size_t  pmm_used_pages(void);

/* VMM (4-level paging) */
#define PTE_PRESENT  (1ULL << 0)
#define PTE_RW       (1ULL << 1)
#define PTE_US       (1ULL << 2)
#define PTE_PWT      (1ULL << 3)
#define PTE_PCD      (1ULL << 4)
#define PTE_ACCESSED (1ULL << 5)
#define PTE_DIRTY    (1ULL << 6)
#define PTE_HUGE     (1ULL << 7)
#define PTE_GLOBAL   (1ULL << 8)
#define PTE_NX       (1ULL << 63)

void   vmm_init(void);
void   vmm_map(vaddr_t v, paddr_t p, u64 flags);
void   vmm_unmap(vaddr_t v);
paddr_t vmm_translate(vaddr_t v);

/* heap */
void   heap_init(void);
void  *kmalloc(size_t n);
void  *kzalloc(size_t n);
void   kfree(void *p);
