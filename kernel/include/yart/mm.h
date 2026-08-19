#pragma once
#include <yart/types.h>
#include <yart/limine.h>

typedef struct task task_t;   /* forward decl (defined in sched.h) */

/* HHDM helpers */
extern u64 g_hhdm_offset;
static ALWAYS_INLINE void *phys_to_virt(paddr_t p) { return (void *)(p + g_hhdm_offset); }
static ALWAYS_INLINE paddr_t virt_to_phys(void *v) { return (paddr_t)v - g_hhdm_offset; }

/* MMIO mapping: ensure the physical region [p, p+n) is mapped into kernel
 * virtual address space at phys_to_virt(p) with PCD (cache-disabled) so that
 * device MMIO BARs above RAM (e.g. 64-bit BARs above 4 GiB, which Limine's
 * HHDM doesn't pre-map) are accessible.  Safe to call on already-mapped
 * ranges. */
void *mmio_map(paddr_t p, size_t n);

/* PMM - bitmap allocator + per-page refcounts */
void    pmm_init(struct limine_memmap_response *mm);
paddr_t pmm_alloc_page(void);
paddr_t pmm_alloc_pages(size_t n);
paddr_t pmm_alloc_pages_below(size_t n, paddr_t limit); /* 32-bit DMA */
void    pmm_free_page(paddr_t p);          /* == pmm_unref_page           */
void    pmm_mark_page_used(paddr_t p);     /* reserve a specific frame      */
void    pmm_mark_range_used(paddr_t base, size_t npages); /* reserve N frames */
bool    pmm_page_used(paddr_t p);          /* is this frame allocated?       */
u32     pmm_page_refs(paddr_t p);          /* refcount of a frame            */
void    pmm_free_pages(paddr_t p, size_t n);
void    pmm_ref_page(paddr_t p);           /* +1 ref (shared mapping)     */
void    pmm_unref_page(paddr_t p);         /* -1 ref; free at 0           */
u32     pmm_refcount(paddr_t p);
size_t  pmm_total_pages(void);
size_t  pmm_used_pages(void);
bool    pmm_selftest(void);
/* Debug hook for the OOM selftest: while forced, pmm_alloc_page takes the
 * OOM-kill path even though free pages remain, so the reclaim logic can be
 * verified deterministically without exhausting real RAM. */
void    pmm_oom_test_force(bool on);

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
#define PTE_COW      (1ULL << 9)   /* software: copy-on-write marker      */
#define PTE_SWAP     (1ULL << 10)  /* software: page is in the swap pool  */
#define PTE_NOSHR    (1ULL << 11)  /* software: kernel-shared page - never CoW'd,
                                      never swapped; fork() keeps the SAME frame
                                      (fb back-buffer, wm surface pages)      */
#define PTE_NX       (1ULL << 63)

void   vmm_init(void);
void   vmm_map(vaddr_t v, paddr_t p, u64 flags);
void   vmm_unmap(vaddr_t v);
void   vmm_unmap_in(u64 *pml4, vaddr_t v);   /* unmap in an explicit pml4 */

/* Mark the whole HHDM direct map NX (heap, kernel stacks, fb, initrd,
 * DMA buffers).  Call AFTER the APs are online - Limine's SMP trampoline
 * runs from the direct map and would triple-fault otherwise. */
void   vmm_nx_direct_map(void);

/* Unmap/remap a single physical page IN THE DIRECT MAP without touching
 * its refcount.  Used for kernel-stack guard pages: the guard frame stays
 * allocated but any access through phys_to_virt faults instead of
 * silently walking into the adjacent heap.  The mapping MUST be restored
 * (vmm_remap_direct_page) before the page is returned to the PMM. */
void   vmm_unmap_direct_page(paddr_t p);
void   vmm_remap_direct_page(paddr_t p);
paddr_t vmm_translate(vaddr_t v);
void   vmm_invlpg(vaddr_t v);
/* Re-set the flags of an already-mapped page (keeps the physical frame). */
void   vmm_set_flags(vaddr_t v, u64 flags);
void   vmm_set_flags_in(u64 *pml4, vaddr_t v, u64 flags);

/* Demand paging: a virtual region can be *reserved* without allocating any
 * physical page; the page-fault handler materializes pages on first touch.
 * `lazy` means "do not map anything now" (fault-in later). */
#define VMM_USER_LAZY 1
#define MAX_USER_REGIONS 32
typedef struct {
    u64 start;      /* page-aligned                                   */
    u64 npages;
    u64 flags;      /* PTE flags (u64: includes PTE_NX bit 63)        */
} user_region_t;

int  vmm_user_reserve(u64 va, u64 npages, u64 flags, u32 opts);
u64  vmm_user_find_free(u64 start, u64 npages, u64 limit);  /* quiet, no log */
/* Generalised reserve: track + map inside an explicit pml4 (exec path). */
int  vmm_reserve_in(u64 *pml4, user_region_t *rs, int *count,
                    u64 va, u64 npages, u64 flags, u32 opts);
int  vmm_user_release(u64 va);
void vmm_user_teardown_all(void);             /* free every user region   */
u64  vmm_user_region_count(void);

/* Resolve a page fault inside the user address space (demand-fault, swap-in
 * or CoW write).  Returns true if the fault was handled. */
bool vmm_resolve_user_fault(u64 va, bool write);

/* Copy-on-write */
int  vmm_cow_map(u64 va, paddr_t phys);
void vmm_cow_fork(u64 *child_pml4);   /* CoW-share all mapped user pages */

/* Per-process page tables */
u64 *vmm_clone_pml4(void);
u64 *vmm_new_pml4(void);   /* fresh user space (kernel half shared) */
void  vmm_free_pml4(u64 *pml4);
void  vmm_switch_pml4(u64 *pml4);
u64  *vmm_current_pml4(void);
u64  *vmm_kernel_pml4(void);
paddr_t vmm_translate_in(u64 *pml4, u64 va);
void   vmm_map_in(u64 *pml4, u64 va, paddr_t p, u64 flags);

/* Swap: a RAM pool always exists as the boot-time/fallback backend; when a
 * virtio-blk disk is present vmm_swap_disk_init() arms a disk-backed tier so
 * evicted pages go to the disk and genuinely free RAM. */
void vmm_swap_init(void);
int  vmm_swap_out(u64 va);           /* move one mapped page to the pool */
int  vmm_swap_in(u64 va);            /* fault a swapped page back in     */
int  vmm_evict_some(int max);        /* swap out up to max pages         */
u64  vmm_swap_disk_reserve_sectors(void); /* trailing disk sectors swap owns */
void vmm_swap_disk_init(void);       /* arm disk swap after blkfs is up  */
bool vmm_swap_disk_armed(void);

/* User-pointer validation for syscalls */
bool vmm_user_range_ok(u64 va, u64 len);
bool vmm_user_str_ok(u64 s, u64 max);

bool vmm_selftest(void);

/* heap - size-class (slab-style) allocator with O(1) fast paths, canary +
 * double-free hardening, alignment >16, and allocation stats (heap.c). */
#define HEAP_NUM_CLASSES  20   /* size classes in heap.c (16..16384) */
#define HEAP_NUM_BUCKETS  (HEAP_NUM_CLASSES + 1)  /* + the "huge" bucket */
typedef struct {
    u64 total_alloc_bytes;
    u64 total_free_bytes;
    u64 cur_bytes;
    u64 peak_bytes;
    u64 alloc_count;
    u64 free_count;
    u64 grow_count;
    u64 per_class_active[HEAP_NUM_BUCKETS];
} heap_stats_t;
void   heap_init(void);
void  *kmalloc(size_t n);
void  *kzalloc(size_t n);
void   kfree(void *p);
void  *kmalloc_aligned(size_t n, size_t align);  /* align power-of-two >16 */
void   kfree_aligned(void *p);
const heap_stats_t *heap_stats(void);
void   heap_stats_snapshot(heap_stats_t *out);
void   heap_selftest(void);
