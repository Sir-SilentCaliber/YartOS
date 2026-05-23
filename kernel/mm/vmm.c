/* Yart OS - Virtual Memory Manager (4-level paging on top of Limine HHDM) */
#include <yart/mm.h>
#include <yart/io.h>
#include <yart/console.h>
#include <yart/string.h>

u64 g_hhdm_offset = 0;

static u64 *kernel_pml4;

static u64 *next_table(u64 *table, int idx, bool create, u64 prop_flags) {
    u64 e = table[idx];
    if (!(e & PTE_PRESENT)) {
        if (!create) return NULL;
        paddr_t p = pmm_alloc_page();
        table[idx] = p | PTE_PRESENT | PTE_RW | PTE_US;
        return (u64 *)phys_to_virt(p);
    }
    /* propagate user-accessible / writable bits down so the leaf flags
       are actually honoured by the CPU.  Linux does the same. */
    if (prop_flags & PTE_US) table[idx] |= PTE_US;
    if (prop_flags & PTE_RW) table[idx] |= PTE_RW;
    return (u64 *)phys_to_virt(e & ~0xFFFULL & ~PTE_NX);
}

void vmm_map(vaddr_t v, paddr_t p, u64 flags) {
    int i4 = (v >> 39) & 0x1FF;
    int i3 = (v >> 30) & 0x1FF;
    int i2 = (v >> 21) & 0x1FF;
    int i1 = (v >> 12) & 0x1FF;
    u64 *pml4 = kernel_pml4;
    u64 *pdpt = next_table(pml4, i4, true, flags);
    u64 *pd   = next_table(pdpt, i3, true, flags);
    u64 *pt   = next_table(pd, i2, true, flags);
    pt[i1] = (p & ~0xFFFULL) | flags | PTE_PRESENT;
    invlpg(v);
}

void vmm_unmap(vaddr_t v) {
    int i4 = (v >> 39) & 0x1FF;
    int i3 = (v >> 30) & 0x1FF;
    int i2 = (v >> 21) & 0x1FF;
    int i1 = (v >> 12) & 0x1FF;
    u64 *pml4 = kernel_pml4;
    u64 *pdpt = next_table(pml4, i4, false, 0); if (!pdpt) return;
    u64 *pd   = next_table(pdpt, i3, false, 0); if (!pd)   return;
    u64 *pt   = next_table(pd, i2, false, 0); if (!pt)   return;
    pt[i1] = 0;
    invlpg(v);
}

paddr_t vmm_translate(vaddr_t v) {
    int i4 = (v >> 39) & 0x1FF;
    int i3 = (v >> 30) & 0x1FF;
    int i2 = (v >> 21) & 0x1FF;
    int i1 = (v >> 12) & 0x1FF;
    u64 *pml4 = kernel_pml4;
    u64 *pdpt = next_table(pml4, i4, false, 0); if (!pdpt) return 0;
    u64 *pd   = next_table(pdpt, i3, false, 0); if (!pd)   return 0;
    u64 *pt   = next_table(pd, i2, false, 0); if (!pt)   return 0;
    if (!(pt[i1] & PTE_PRESENT)) return 0;
    return (pt[i1] & ~0xFFFULL & ~PTE_NX) | (v & 0xFFF);
}

void vmm_init(void) {
    /* We adopt the page tables Limine already built and just record the
       PML4.  Limine has set up:
         - identity / HHDM mapping for all RAM,
         - higher-half kernel mapping.
       We only need to be able to *modify* it for new allocations. */
    paddr_t cr3 = read_cr3() & ~0xFFFULL;
    kernel_pml4 = (u64 *)phys_to_virt(cr3);
    kprintf("vmm: PML4 @ phys %p (virt %p), HHDM offset %p\n",
            (void *)cr3, kernel_pml4, (void *)g_hhdm_offset);
}
