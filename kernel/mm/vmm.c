/* Yart OS - Virtual Memory Manager (4-level paging on top of Limine HHDM)
 *
 * Beyond basic map/unmap this provides the pieces a real VMM has:
 *   - demand paging : user regions can be reserved without physical pages;
 *                     vmm_resolve_user_fault() materializes them on touch
 *   - copy-on-write : vmm_cow_map()/vmm_cow_fork() share pages read-only;
 *                     write faults upgrade (refcount 1) or copy (refcount>1)
 *   - per-process page tables : vmm_clone_pml4() deep-copies the user half
 *                     (kernel half shared) for fork()/exec()
 *   - swap : evicted user pages live in a RAM-backed pool (no disk yet);
 *                     a not-present PTE marked PTE_SWAP faults them back in
 *   - per-task user regions : each process owns its demand map
 */
#include <yart/mm.h>
#include <yart/io.h>
#include <yart/console.h>
#include <yart/string.h>
#include <yart/user.h>     /* USER_VBASE / USER_STACK_TOP */
#include <yart/sched.h>    /* sched_current() for per-task regions */
#include <yart/cpu.h>      /* rdmsr64/wrmsr64 for EFER.NXE */
#include <yart/hal.h>      /* smp_tlb_shootdown() for shared-table edits */
#include <yart/spinlock.h> /* g_swap_lock */
#include <yart/blk.h>      /* disk-backed swap tier (virtio-blk)      */

u64 g_hhdm_offset = 0;
u32 g_cpu_features = 0;

static u64 *kernel_pml4;        /* the bootloader's tables (kernel identity) */
/* What CR3 points at right NOW on THIS CPU.  Per-CPU on SMP: the BSP and
 * every AP run different tasks with different page tables, so a single
 * global would be wrong for everyone but the last CPU to switch.
 * g_boot_pml4 is the fallback used before/without a per-CPU area. */
u64 *g_boot_pml4;
static ALWAYS_INLINE u64 *cur_pml4(void) {
    cpu_local_t *c = get_cpu_local();
    if (c && c->pml4_current) return c->pml4_current;
    return g_boot_pml4;
}

/* Boot-time user regions: the ELF loader and stack allocator reserve into
 * these before a task exists; sched_create_user() moves them into the task
 * via vmm_take_boot_regions(). */
static user_region_t g_boot_regions[MAX_USER_REGIONS];
static int g_boot_region_count;

/* Per-fault tracing.  Demand-fault and CoW-split happen on EVERY page of
 * every app's text/data/stack, so logging each one (kprintf -> serial, one
 * busy-wait per character) turned the page-fault hot path into a serial
 * bottleneck and drowned the log.  Real kernels use off-by-default
 * tracepoints here; so do we.  Flip to true only when debugging the VMM. */
static bool g_vmm_trace = false;

/* ---------------- page-table walk ---------------- */

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

static u64 *walk(u64 *pml4, u64 v, bool create, u64 flags) {
    int i4 = (v >> 39) & 0x1FF;
    int i3 = (v >> 30) & 0x1FF;
    int i2 = (v >> 21) & 0x1FF;
    int i1 = (v >> 12) & 0x1FF;
    u64 *pdpt = next_table(pml4, i4, create, flags); if (!pdpt) return NULL;
    u64 *pd   = next_table(pdpt, i3, create, flags); if (!pd)   return NULL;
    u64 *pt   = next_table(pd, i2, create, flags);   if (!pt)   return NULL;
    return &pt[i1];
}

void vmm_invlpg(vaddr_t v) { invlpg(v); }

void vmm_map(vaddr_t v, paddr_t p, u64 flags) {
    u64 *pte = walk(cur_pml4(), v, true, flags);
    *pte = (p & ~0xFFFULL) | flags | PTE_PRESENT;
    invlpg(v);
}

void vmm_set_flags(vaddr_t v, u64 flags) {
    vmm_set_flags_in(cur_pml4(), v, flags);
}

void vmm_set_flags_in(u64 *pml4, vaddr_t v, u64 flags) {
    u64 *pte = walk(pml4, v, false, 0);
    if (!pte || !(*pte & PTE_PRESENT)) return;
    paddr_t phys = *pte & ~0xFFFULL & ~PTE_NX;
    *pte = (phys & ~0xFFFULL) | flags | PTE_PRESENT;
    invlpg(v);
}

void vmm_unmap(vaddr_t v) {
    vmm_unmap_in(cur_pml4(), v);
}

/* Unmap one page inside an explicit pml4 (used for window surfaces that
 * are mapped into two address spaces at once). */
void vmm_unmap_in(u64 *pml4, vaddr_t v) {
    u64 *pte = walk(pml4, v, false, 0);
    if (!pte || !(*pte & PTE_PRESENT)) return;
    paddr_t phys = *pte & ~0xFFFULL & ~PTE_NX;
    *pte = 0;
    invlpg(v);
    pmm_unref_page(phys);       /* drop this mapping's reference          */
}

paddr_t vmm_translate(vaddr_t v) { return vmm_translate_in(cur_pml4(), v); }

paddr_t vmm_translate_in(u64 *pml4, u64 va) {
    u64 *pte = walk(pml4, va, false, 0);
    if (!pte || !(*pte & PTE_PRESENT)) return 0;
    return (*pte & ~0xFFFULL & ~PTE_NX) | (va & 0xFFF);
}

void vmm_map_in(u64 *pml4, u64 va, paddr_t p, u64 flags) {
    u64 *pte = walk(pml4, va, true, flags);
    *pte = (p & ~0xFFFULL) | flags | PTE_PRESENT;
    /*
     * Flush this VA from the TLB on the current CPU.
     *
     * Why this matters when we're mapping into a *different* pml4:
     *  - If pml4 is currently loaded in CR3 (e.g. we're mid-syscall and
     *    already switched to the target pml4), invlpg removes any stale
     *    entry for this VA so the next access re-walks to the new PTE.
     *  - If pml4 is NOT currently loaded (e.g. mapping fb pages into a
     *    user task's pml4 from a kernel syscall), the PTE is in a tree
     *    that CR3 doesn't point at right now; invlpg is a nop for the
     *    currently-loaded tree, and the eventual CR3 write in switch_to()
     *    does a full TLB flush for the target tree.
     *  - Critically, if the top-level (PML4E) for this VA range was just
     *    allocated by walk(), and the kernel pml4 had the same index *0*
     *    (which it does - kernel pml4[0] is 0 for the unused low half),
     *    the CPU's TLB might cache "PML4E[0] = not present" from a recent
     *    kernel-side miss.  invlpg here ensures the next access under the
     *    target pml4 re-walks the full 4-level table.  (On a full CR3
     *    write the TLB is flushed anyway, so this is a belt-and-braces
     *    guard for any future caller that does NOT switch CR3.)
     */
    invlpg(va);
}

u64 *vmm_kernel_pml4(void) { return kernel_pml4; }
u64 *vmm_current_pml4(void) { return cur_pml4(); }

void vmm_switch_pml4(u64 *pml4) {
    if (!pml4) pml4 = kernel_pml4;
    g_boot_pml4 = pml4;
    cpu_local_t *c = get_cpu_local();
    if (c) c->pml4_current = pml4;
    write_cr3(virt_to_phys(pml4));
}

/* ---------------- per-task user regions ---------------- */

static user_region_t *active_regions(int *count) {
    task_t *cur = sched_current();
    if (cur && cur->is_user) {
        *count = cur->region_count;
        return cur->regions;
    }
    *count = g_boot_region_count;
    return g_boot_regions;
}

static user_region_t *find_region(u64 va) {
    int n; user_region_t *rs = active_regions(&n);
    for (int i = 0; i < n; i++)
        if (va >= rs[i].start && va < rs[i].start + rs[i].npages * PAGE_SIZE)
            return &rs[i];
    return NULL;
}

/* tiny helper: bump the active region count */
static void set_region_count(int c) {
    task_t *cur = sched_current();
    if (cur && cur->is_user) cur->region_count = c;
    else g_boot_region_count = c;
}

void vmm_take_boot_regions(task_t *t) {
    memcpy(t->regions, g_boot_regions, sizeof g_boot_regions);
    t->region_count = g_boot_region_count;
    memset(&g_boot_regions, 0, sizeof g_boot_regions);
    g_boot_region_count = 0;
}

/* Generalised reserve: track + (optionally) map `npages` at `va` inside
 * `pml4`, recording the region in `rs`/`*count`.  Used by the exec path
 * (building a fresh address space) and by vmm_user_reserve (current
 * address space).  Does NOT charge memory - callers decide. */
int vmm_reserve_in(u64 *pml4, user_region_t *rs, int *count,
                   u64 va, u64 npages, u64 flags, u32 opts) {
    if (npages == 0) return -1;
    u64 start = PAGE_ALIGN_DOWN(va);
    u64 end   = start + npages * PAGE_SIZE;
    if (end < start) return -1;
    if (start < USER_VFLOOR || end > USER_STACK_TOP) return -1;
    for (int i = 0; i < *count; i++) {
        u64 r_end = rs[i].start + rs[i].npages * PAGE_SIZE;
        if (start < r_end && end > rs[i].start) return -1;   /* overlap */
    }
    if (*count >= MAX_USER_REGIONS) return -1;
    rs[*count].start  = start;
    rs[*count].npages = npages;
    rs[*count].flags  = flags | PTE_US;
    (*count)++;
    if (!(opts & VMM_USER_LAZY)) {
        for (u64 a = start; a < end; a += PAGE_SIZE) {
            paddr_t p = pmm_alloc_page();
            vmm_map_in(pml4, a, p, flags | PTE_US);
        }
    }
    return 0;
}

/* Quietly find the first free VA >= `start` (page-aligned) that fits `npages`
 * pages without overlapping any existing region, and return it, or 0 if none
 * before `limit`.  NO logging: used by mmap()'s probe loop, which otherwise
 * spams one "reserve FAIL" line per page as it walked past the ~17 MB
 * compositor image (init.elf carries the 16 MB wallpaper blob) that can
 * overlap the mmap arena depending on the ASLR bias.  O(regions), not
 * O(pages). */
u64 vmm_user_find_free(u64 start, u64 npages, u64 limit) {
    u64 need = npages * PAGE_SIZE;
    u64 cand = PAGE_ALIGN_UP(start);
    int n; user_region_t *rs = active_regions(&n);
    while (cand + need <= limit) {
        bool free = true;
        for (int i = 0; i < n; i++) {
            u64 r_end = rs[i].start + rs[i].npages * PAGE_SIZE;
            if (cand < r_end && cand + need > rs[i].start) {
                cand = r_end;          /* skip past this region's end */
                free = false;
                break;
            }
        }
        if (free) return cand;
    }
    return 0;
}

int vmm_user_reserve(u64 va, u64 npages, u64 flags, u32 opts) {
    if (npages == 0) return -1;
    /* CHARGE-AFTER-CHECKS FIX: validate everything first, charge only on
     * success.  The old order charged BEFORE the range/count/overlap
     * checks, so every failed reserve LEAKED its pages into the task's
     * memory accounting - a poisoned region list (or a full region table)
     * turned one failed mmap() into ~240 phantom MiBs of charge and then
     * a boot-long "over memory cap" storm. */
    u64 start = PAGE_ALIGN_DOWN(va);
    u64 end   = start + npages * PAGE_SIZE;
    if (end < start) return -1;
    if (start < USER_VFLOOR || end > USER_STACK_TOP) {
        if (sched_current() && sched_current()->pid == 4)
            kprintf("vmm: reserve FAIL va=0x%lx npages=%lu: out of user range\n",
                    (unsigned long)va, (unsigned long)npages);
        return -1;
    }
    int n; user_region_t *rs = active_regions(&n);
    for (int i = 0; i < n; i++) {
        u64 r_end = rs[i].start + rs[i].npages * PAGE_SIZE;
        if (start < r_end && end > rs[i].start) {
            if (sched_current() && sched_current()->pid == 4)
                kprintf("vmm: reserve FAIL va=0x%lx npages=%lu: overlaps region %d va=0x%lx npages=%lu (count=%d)\n",
                        (unsigned long)va, (unsigned long)npages, i,
                        (unsigned long)rs[i].start, (unsigned long)rs[i].npages, n);
            return -1;   /* overlap */
        }
    }
    if (n >= MAX_USER_REGIONS) {
        if (sched_current() && sched_current()->pid == 4)
            kprintf("vmm: reserve FAIL va=0x%lx npages=%lu: region table full (%d)\n",
                    (unsigned long)va, (unsigned long)npages, n);
        return -1;
    }
    /* per-task memory cap: charging happens even for lazy (demand) regions,
     * because the pages WILL be faulted in later */
    if (sched_current_is_user() && sched_mem_used() + npages > sched_mem_limit()) {
        kprintf("vmm: pid %d over memory cap: used=%llu want=%llu limit=%llu va=0x%lx\n",
                sched_current()->pid, (unsigned long long)sched_mem_used(),
                (unsigned long long)npages, (unsigned long long)sched_mem_limit(),
                (unsigned long)va);
        return -1;
    }
    sched_charge_pages((i64)npages);
    user_region_t *r = &rs[n];
    r->start  = start;
    r->npages = npages;
    r->flags  = flags | PTE_US;
    set_region_count(n + 1);
    if (!(opts & VMM_USER_LAZY)) {
        for (u64 a = start; a < end; a += PAGE_SIZE) {
            paddr_t p = pmm_alloc_page();
            vmm_map(a, p, r->flags);
        }
    }
    return 0;
}

int vmm_user_release(u64 va) {
    u64 start = PAGE_ALIGN_DOWN(va);
    int n; user_region_t *rs = active_regions(&n);
    for (int i = 0; i < n; i++) {
        if (rs[i].start != start) continue;
        user_region_t r = rs[i];
        for (u64 a = r.start; a < r.start + r.npages * PAGE_SIZE; a += PAGE_SIZE)
            vmm_unmap(a);
        sched_charge_pages(-(i64)r.npages);
        rs[i] = rs[--n];
        set_region_count(n);
        return 0;
    }
    return -1;
}

void vmm_user_teardown_all(void) {
    int n; user_region_t *rs = active_regions(&n);
    for (int i = 0; i < n; i++) {
        user_region_t r = rs[i];
        for (u64 a = r.start; a < r.start + r.npages * PAGE_SIZE; a += PAGE_SIZE)
            vmm_unmap(a);
    }
    if (n) set_region_count(0);
}

u64 vmm_user_region_count(void) {
    int n; active_regions(&n); return (u64)n;
}

/* Resolve a fault on a user VA: swap-in, demand-fault, or CoW. */
bool vmm_resolve_user_fault(u64 va, bool write) {
    user_region_t *r = find_region(va);
    u64 page = PAGE_ALIGN_DOWN(va);
    u64 *pte = walk(cur_pml4(), page, false, 0);
    bool present = pte && (*pte & PTE_PRESENT);

    if (pte && (*pte & PTE_SWAP)) {              /* swapped out -> in      */
        if (vmm_swap_in(page) != 0) return false; /* OOM/I-O error -> SIGSEGV */
        return true;
    }

    if (!present) {
        if (!r) return false;                    /* illegal access         */
        if (sched_current_is_user() && sched_mem_used() + 1 > sched_mem_limit()) {
            kprintf("vmm: pid %d demand-fault over memory cap\n",
                    sched_current() ? sched_current()->pid : 0);
            return false;
        }
        sched_charge_pages(1);
        paddr_t p = pmm_alloc_page();            /* zeroed on alloc        */
        if (!p) {                                /* OOM killed the task    */
            sched_charge_pages(-1);
            return false;
        }
        vmm_map(page, p, r->flags);
        if (g_vmm_trace)
            kprintf("vmm: demand-fault 0x%lx -> %p mapped (region %p flags=%x)\n",
                    page, (void *)p, (void *)r->start, r->flags);
        return true;
    }

    if (write && pte && (*pte & PTE_COW) && !(*pte & PTE_RW)) {
        paddr_t phys = *pte & ~0xFFFULL & ~PTE_NX;
        if (pmm_refcount(phys) == 1) {
            *pte |= PTE_RW;
            *pte &= ~PTE_COW;
            invlpg(page);
            return true;
        }
        paddr_t copy = pmm_alloc_page();
        memcpy(phys_to_virt(copy), phys_to_virt(phys), PAGE_SIZE);
        pmm_unref_page(phys);
        vmm_map(page, copy, PTE_RW | PTE_US);
        if (g_vmm_trace)
            kprintf("vmm: CoW copy 0x%lx old=%p new=%p\n",
                    page, (void *)phys, (void *)copy);
        return true;
    }
    return false;
}

/* ---------------- copy-on-write ---------------- */

int vmm_cow_map(u64 va, paddr_t phys) {
    u64 page = PAGE_ALIGN_DOWN(va);
    pmm_ref_page(phys);
    u64 *pte = walk(cur_pml4(), page, true, PTE_US);
    *pte = (phys & ~0xFFFULL) | PTE_PRESENT | PTE_US | PTE_COW;  /* RO */
    invlpg(page);
    return 0;
}

/* After a fork, share every already-mapped user page read-only between the
 * parent (current tables) and the child (child_pml4).
 *
 * IMPORTANT: every page shared with the child must take a NEW reference,
 * EVEN IF the parent's PTE is already marked COW.  The COW bit means "this
 * page may be shared with another table" - it does not mean "this child
 * already has its ref".  vmm_clone_pml4() copies the leaf PTEs verbatim
 * (child points at the same frames without refs), so this function is the
 * only place the child's refs are accounted.  Skipping COW pages here let
 * the child's teardown free pages the parent still mapped (double-free +
 * stale PTEs). */
void vmm_cow_fork(u64 *child_pml4) {
    int n; user_region_t *rs = active_regions(&n);
    for (int i = 0; i < n; i++) {
        for (u64 a = rs[i].start;
             a < rs[i].start + rs[i].npages * PAGE_SIZE; a += PAGE_SIZE) {
            u64 *p = walk(cur_pml4(), a, false, 0);
            if (!p || !(*p & PTE_PRESENT)) continue;
            paddr_t phys = *p & ~0xFFFULL & ~PTE_NX;
            if (*p & PTE_SWAP) continue;
            pmm_ref_page(phys);                  /* one ref per mapping    */
            /* Kernel-shared buffers (fb back-buffer, wm surfaces) must stay
             * on the SAME frame in parent AND child: the kernel blits from
             * g_fb.pixels / g_wm_surfs[].pages, so a CoW copy would make
             * the compositor draw into a frame nobody ever displays.
             * vmm_clone_pml4() already copied the PTE verbatim (RW|NOSHR),
             * so just take the child's ref and leave both mappings alone. */
            if (*p & PTE_NOSHR) continue;
            u64 entry = (phys & ~0xFFFULL) | PTE_PRESENT | PTE_US | PTE_COW;
            *p = entry;                          /* parent -> RO           */
            u64 *cp = walk(child_pml4, a, true, PTE_US);
            *cp = entry;                         /* child  -> RO           */
            invlpg(a);
        }
    }
    /* Pass 2: NOSHR pages mapped OUTSIDE any region.  The wm-side window
     * canvases (WM_SURF_WM_BASE + id*stride) are mapped straight into the
     * compositor's PML4 via vmm_map_in() - no region covers them, so the
     * loop above never visits them.  But vmm_clone_pml4() copies the whole
     * user half VERBATIM, so the child DOES get a PTE for every canvas.
     * Without this pass the child's copy is unaccounted, and the exec path
     * (vmm_free_pml4) unrefs it with no matching ref: every app launch
     * under-counted every live/deferred surface frame, the slot-reuse
     * reclaim then freed a still-mapped frame ("free of non-allocated
     * page" wall), and the freed frame corrupted whatever task reused it -
     * the real root cause of the intermittent SMP frame-corruption panics. */
    u64 *pml4 = cur_pml4();
    if (pml4[0] & PTE_PRESENT) {
        u64 *pdpt = phys_to_virt(pml4[0] & ~0xFFFULL & ~PTE_NX);
        for (u32 i3 = 0; i3 < 512; i3++) {          /* PDPT index (bits 30-38) */
            if (!(pdpt[i3] & PTE_PRESENT)) continue;
            u64 *pd = phys_to_virt(pdpt[i3] & ~0xFFFULL & ~PTE_NX);
            for (u32 i2 = 0; i2 < 512; i2++) {      /* PD index   (bits 21-29) */
                if (!(pd[i2] & PTE_PRESENT) || (pd[i2] & PTE_HUGE)) continue;
                u64 *pt = phys_to_virt(pd[i2] & ~0xFFFULL & ~PTE_NX);
                for (u32 i1 = 0; i1 < 512; i1++) {  /* PT index   (bits 12-20) */
                    u64 *pte = &pt[i1];
                    if (!(*pte & PTE_PRESENT)) continue;
                    if (!(*pte & PTE_NOSHR) || (*pte & PTE_SWAP)) continue;
                    u64 va = ((u64)i3 << 30) | ((u64)i2 << 21) |
                             ((u64)i1 << 12);
                    /* region-resident NOSHR pages (the fb) already got
                     * their ref in the loop above - skip them here */
                    if (find_region(va)) continue;
                    pmm_ref_page(*pte & ~0xFFFULL & ~PTE_NX);
                }
            }
        }
    }
}

/* ---------------- per-process page tables ---------------- */

/* Deep-copy the user half (PML4 entry 0); kernel entries stay shared. */
/* A brand-new PML4 for a fresh address space: copy the kernel's high-half
 * entries (indices 256+) but start with an empty user half (index 0). */
u64 *vmm_new_pml4(void) {
    paddr_t np = pmm_alloc_page();
    u64 *newpml4 = phys_to_virt(np);
    memset(newpml4, 0, PAGE_SIZE);
    for (int i = 256; i < 512; i++)
        newpml4[i] = kernel_pml4[i];   /* share kernel mappings */
    return newpml4;
}

u64 *vmm_clone_pml4(void) {
    paddr_t np = pmm_alloc_page();
    u64 *newpml4 = phys_to_virt(np);
    memcpy(newpml4, cur_pml4(), PAGE_SIZE);
    if (cur_pml4()[0] & PTE_PRESENT) {
        paddr_t pdpt_p = pmm_alloc_page();
        u64 *new_pdpt = phys_to_virt(pdpt_p);
        u64 *old_pdpt = phys_to_virt(cur_pml4()[0] & ~0xFFFULL & ~PTE_NX);
        memcpy(new_pdpt, old_pdpt, PAGE_SIZE);
        newpml4[0] = pdpt_p | (cur_pml4()[0] & 0xFFF);
        for (int i3 = 0; i3 < 512; i3++) {
            if (!(old_pdpt[i3] & PTE_PRESENT)) continue;
            paddr_t pd_p = pmm_alloc_page();
            u64 *new_pd = phys_to_virt(pd_p);
            u64 *old_pd = phys_to_virt(old_pdpt[i3] & ~0xFFFULL & ~PTE_NX);
            memcpy(new_pd, old_pd, PAGE_SIZE);
            new_pdpt[i3] = pd_p | (old_pdpt[i3] & 0xFFF);
            for (int i2 = 0; i2 < 512; i2++) {
                if (!(old_pd[i2] & PTE_PRESENT)) continue;
                paddr_t pt_p = pmm_alloc_page();
                u64 *new_pt = phys_to_virt(pt_p);
                u64 *old_pt = phys_to_virt(old_pd[i2] & ~0xFFFULL & ~PTE_NX);
                memcpy(new_pt, old_pt, PAGE_SIZE);
                new_pd[i2] = pt_p | (old_pd[i2] & 0xFFF);
            }
        }
    }
    return newpml4;
}

/* Free a task's whole private address space.  Because a task whose PML4 we
 * free here is guaranteed NOT running on any CPU (sched_kill/reap only call
 * this when the task is not ap_current anywhere), every page in the user
 * half is safe to drop: page tables, the PDPT, the PML4, AND every mapped
 * user data page.  CoW pages shared with a live sibling have refcount > 1,
 * so pmm_unref_page() leaves them in place.  (Fixing this to also free the
 * data pages closes a real SIGKILL/OOM memory leak where only the page-table
 * frames were reclaimed.) */
void vmm_free_pml4(u64 *pml4) {
    if (!pml4 || pml4 == kernel_pml4) return;
    if (pml4[0] & PTE_PRESENT) {
        u64 *pdpt = phys_to_virt(pml4[0] & ~0xFFFULL & ~PTE_NX);
        for (int i3 = 0; i3 < 512; i3++) {
            if (!(pdpt[i3] & PTE_PRESENT)) continue;
            u64 *pd = phys_to_virt(pdpt[i3] & ~0xFFFULL & ~PTE_NX);
            for (int i2 = 0; i2 < 512; i2++) {
                if (!(pd[i2] & PTE_PRESENT)) continue;
                if (pd[i2] & PTE_HUGE) {          /* 2 MiB huge page          */
                    pmm_unref_page(pd[i2] & ~0x1FFFFFULL & ~PTE_NX);
                    continue;
                }
                u64 *pt = phys_to_virt(pd[i2] & ~0xFFFULL & ~PTE_NX);
                for (int i1 = 0; i1 < 512; i1++)
                    if (pt[i1] & PTE_PRESENT)     /* free the user data page  */
                        pmm_unref_page(pt[i1] & ~0xFFFULL & ~PTE_NX);
                pmm_unref_page(pd[i2] & ~0xFFFULL & ~PTE_NX);  /* the PT page  */
            }
            pmm_unref_page(pdpt[i3] & ~0xFFFULL & ~PTE_NX);    /* the PD page  */
        }
        pmm_unref_page(pml4[0] & ~0xFFFULL & ~PTE_NX);         /* the PDPT page*/
    }
    pmm_unref_page(virt_to_phys(pml4));                        /* the PML4 page*/
}

/* ---------------- swap: RAM pool + optional disk-backed tier ------- */

#define SWAP_SLOTS 1024
#define SWAP_CPUS 1        /* one pool per CPU; SMP grows this to NR_CPUS */
#define SWAP_DISK_SECTORS 2048  /* trailing disk sectors reserved for swap */
#define SWAP_PAGE_SECTORS 8     /* 4 KiB page = 8 x 512 B sectors         */

static u8  *swap_pool[SWAP_CPUS];
static u64  swap_used[SWAP_CPUS][SWAP_SLOTS / 64];
/* Protects the slot bitmaps (RAM + disk tiers).  pmm_alloc_page drops the
 * PMM lock before eviction, so two CPUs can reach the bitmap concurrently. */
static spinlock_t g_swap_lock;

/* disk-backed tier: armed when a virtio-blk disk is present.  Evicted
 * pages go to disk sectors (freeing the RAM frame for real) instead of the
 * RAM pool; the RAM pool stays as the boot-time / no-disk fallback. */
static bool swap_disk_armed;
static u64  swap_disk_base_sector;
static u64  swap_disk_npages;      /* SWAP_DISK_SECTORS / 8            */
static u64 *swap_disk_used;        /* bitmap of disk slots             */
static u64  swap_disk_out, swap_disk_in;   /* stats                    */

static int swap_cpu(void) {
    /* per-CPU swap pools: each CPU evicts into its OWN pool so two CPUs
     * never race on slot allocation */
    cpu_local_t *c = get_cpu_local();
    return c ? (int)(c->cpu_id % SWAP_CPUS) : 0;
}

void vmm_swap_init(void) {
    if (swap_pool[0]) return;
    spin_init(&g_swap_lock);
    for (int c = 0; c < SWAP_CPUS; c++) {
        paddr_t p = pmm_alloc_pages(SWAP_SLOTS);
        swap_pool[c] = phys_to_virt(p);
        memset(swap_used[c], 0, sizeof swap_used[c]);
    }
    swap_disk_armed = false;
    swap_disk_out = swap_disk_in = 0;
    kprintf("swap: %u KiB RAM-backed pool%s @%p (disk tier off until a disk)\n",
            SWAP_SLOTS * PAGE_SIZE / 1024,
            SWAP_CPUS > 1 ? "s" : "", swap_pool[0]);
}

/* The last SWAP_DISK_SECTORS of the disk are reserved for swap; blkfs sizes
 * itself to disk_size - this, so it never touches the swap region. */
u64 vmm_swap_disk_reserve_sectors(void) { return SWAP_DISK_SECTORS; }

bool vmm_swap_disk_armed(void) { return swap_disk_armed; }

/* Arm disk-backed swap.  Called once after blkfs_init() has mounted the disk.
 * Region = [disk_size - SWAP_DISK_SECTORS, disk_size).  Then prove a page
 * survives a disk round-trip (out + in) so we know the tier actually works. */
void vmm_swap_disk_init(void) {
    if (swap_disk_armed) return;
    if (!blk_disk_present()) { kprintf("swap: no disk - RAM-only pool\n"); return; }
    u64 disk = blk_disk_sectors();
    if (disk <= SWAP_DISK_SECTORS) { kprintf("swap: disk too small for swap\n"); return; }
    swap_disk_base_sector = disk - SWAP_DISK_SECTORS;
    swap_disk_npages = SWAP_DISK_SECTORS / SWAP_PAGE_SECTORS;
    swap_disk_used = kzalloc((swap_disk_npages / 64 + 1) * 8);
    swap_disk_armed = true;
    kprintf("swap: disk-backed tier armed: %u pages @ sector %llu (disk %llu, "
            "%llu MiB)\n", (u32)swap_disk_npages,
            (unsigned long long)swap_disk_base_sector,
            (unsigned long long)disk, (unsigned long long)disk / 2048);

    /* selftest: write a known pattern through the disk swap region and read
     * it back, proving a real disk round-trip (out + in) works. */
    u8 pg[PAGE_SIZE];
    for (u32 i = 0; i < PAGE_SIZE; i++) pg[i] = (u8)(0xA0 + (i & 0x0F));
    if (blk_write_sectors(swap_disk_base_sector, SWAP_PAGE_SECTORS, pg) == 0) {
        u8 rd[PAGE_SIZE];
        if (blk_read_sectors(swap_disk_base_sector, SWAP_PAGE_SECTORS, rd) == 0 &&
            memcmp(pg, rd, PAGE_SIZE) == 0)
            kprintf("swap: disk round-trip selftest PASS\n");
        else
            kprintf("swap: !! disk round-trip selftest FAIL\n");
    } else {
        kprintf("swap: !! disk write failed in selftest\n");
    }
}

/* Slot space: the RAM pool occupies slots [0, SWAP_SLOTS); the disk tier (if
 * armed) occupies [SWAP_SLOTS, SWAP_SLOTS + swap_disk_npages).  RAM slots are
 * tried first (fast), disk is the extra capacity that genuinely frees RAM.
 * This is strictly more swap than the old RAM-only pool, never less. */
#define SWAP_DISK_SLOT_BASE SWAP_SLOTS

static int swap_alloc_slot(void) {
    int c = swap_cpu();
    u64 fl = irq_save();
    spin_lock(&g_swap_lock);
    for (int i = 0; i < SWAP_SLOTS; i++)
        if (!(swap_used[c][i / 64] & (1ULL << (i % 64)))) {
            swap_used[c][i / 64] |= (1ULL << (i % 64));
            spin_unlock(&g_swap_lock);
            irq_restore(fl);
            return i;
        }
    if (swap_disk_armed) {
        for (u64 i = 0; i < swap_disk_npages; i++)
            if (!(swap_disk_used[i / 64] & (1ULL << (i % 64)))) {
                swap_disk_used[i / 64] |= (1ULL << (i % 64));
                spin_unlock(&g_swap_lock);
                irq_restore(fl);
                return (int)(SWAP_DISK_SLOT_BASE + i);
            }
    }
    spin_unlock(&g_swap_lock);
    irq_restore(fl);
    return -1;                 /* both tiers full: caller sees OOM */
}
static void swap_free_slot(int s) {
    u64 fl = irq_save();
    spin_lock(&g_swap_lock);
    if (swap_disk_armed && s >= SWAP_DISK_SLOT_BASE) {
        u64 i = (u64)(s - SWAP_DISK_SLOT_BASE);
        swap_disk_used[i / 64] &= ~(1ULL << (i % 64));
        spin_unlock(&g_swap_lock);
        irq_restore(fl);
        return;
    }
    int c = swap_cpu();
    swap_used[c][s / 64] &= ~(1ULL << (s % 64));
    spin_unlock(&g_swap_lock);
    irq_restore(fl);
}
static bool slot_is_disk(int s) {
    return swap_disk_armed && s >= SWAP_DISK_SLOT_BASE;
}

int vmm_swap_out(u64 va) {
    u64 page = PAGE_ALIGN_DOWN(va);
    u64 *pte = walk(cur_pml4(), page, false, 0);
    if (!pte || !(*pte & PTE_PRESENT) || (*pte & PTE_SWAP)) return -1;
    int slot = swap_alloc_slot();
    if (slot < 0) return -1;
    paddr_t phys = *pte & ~0xFFFULL & ~PTE_NX;
    if (slot_is_disk(slot)) {
        u64 sec = swap_disk_base_sector +
                  (u64)(slot - SWAP_DISK_SLOT_BASE) * SWAP_PAGE_SECTORS;
        if (blk_write_sectors(sec, SWAP_PAGE_SECTORS, phys_to_virt(phys)) != 0) {
            swap_free_slot(slot);
            return -1;
        }
        swap_disk_out++;
    } else {
        memcpy(swap_pool[swap_cpu()] + (u64)slot * PAGE_SIZE,
               phys_to_virt(phys), PAGE_SIZE);
    }
    pmm_unref_page(phys);               /* the RAM frame is freed - real room */
    *pte = ((u64)slot << 12) | PTE_SWAP;/* not-present + marker   */
    invlpg(page);
    return slot;
}

int vmm_swap_in(u64 va) {
    u64 page = PAGE_ALIGN_DOWN(va);
    u64 *pte = walk(cur_pml4(), page, false, 0);
    if (!pte || !(*pte & PTE_SWAP)) return -1;
    int slot = (*pte >> 12) & 0xFFFFF;
    user_region_t *r = find_region(page);
    paddr_t p = pmm_alloc_page();
    if (!p) return -1;                    /* OOM: caller treats as a kill */
    if (slot_is_disk(slot)) {
        u64 sec = swap_disk_base_sector +
                  (u64)(slot - SWAP_DISK_SLOT_BASE) * SWAP_PAGE_SECTORS;
        if (blk_read_sectors(sec, SWAP_PAGE_SECTORS, phys_to_virt(p)) != 0) {
            pmm_unref_page(p);
            swap_free_slot(slot);
            return -1;
        }
        swap_disk_in++;
    } else {
        memcpy(phys_to_virt(p), swap_pool[swap_cpu()] + (u64)slot * PAGE_SIZE,
               PAGE_SIZE);
    }
    swap_free_slot(slot);
    vmm_map(page, p, r ? r->flags : (PTE_RW | PTE_US));
    return 0;
}

int vmm_evict_some(int max) {
    int n; user_region_t *rs = active_regions(&n);
    int done = 0;
    for (int i = 0; i < n && done < max; i++) {
        for (u64 a = rs[i].start;
             a < rs[i].start + rs[i].npages * PAGE_SIZE && done < max;
             a += PAGE_SIZE) {
            u64 *pte = walk(cur_pml4(), a, false, 0);
            if (!pte || !(*pte & PTE_PRESENT) || (*pte & PTE_SWAP)) continue;
            if (*pte & PTE_COW) continue;        /* shared: don't evict    */
            if (*pte & PTE_NOSHR) continue;      /* kernel-shared buffer: swapping
                                                    would break the kernel/user
                                                    frame identity (fb, wm surfs) */
            if (!(*pte & PTE_NX)) continue;      /* executable (code): don't
                                                    evict - a task must be
                                                    able to keep running while
                                                    we reclaim its data/stack */
            if (vmm_swap_out(a) >= 0) done++;
        }
    }
    return done;
}

/* ---------------- user-pointer validation ---------------- */

/* A page is readable by the kernel (stac) iff it is present + PTE_US.
 * Reserved-but-unmapped (lazy) pages are MATERIALIZED first: a kernel
 * read of a lazy page would otherwise take a kernel-mode page fault and
 * panic - validating with resolve first makes every uptr()/copy_user_str()
 * caller crash-proof against lazily-reserved regions. */
static bool page_readable_for_kernel(u64 a) {
    u64 *pte = walk(cur_pml4(), a, false, 0);
    if (pte && (*pte & PTE_PRESENT))
        return (*pte & PTE_US) != 0;
    if (!find_region(a))
        return false;                          /* not even reserved */
    if (!vmm_resolve_user_fault(a, false))
        return false;                          /* demand-fault failed */
    pte = walk(cur_pml4(), a, false, 0);
    return pte && (*pte & PTE_PRESENT) && (*pte & PTE_US);
}

bool vmm_user_range_ok(u64 va, u64 len) {
    if (len == 0) return true;
    if (va + len < va) return false;
    if (va < USER_VFLOOR || va + len > USER_STACK_TOP) return false;
    u64 first = PAGE_ALIGN_DOWN(va);
    u64 last  = PAGE_ALIGN_UP(va + len);
    for (u64 a = first; a < last; a += PAGE_SIZE)
        if (!page_readable_for_kernel(a)) return false;
    return true;
}

bool vmm_user_str_ok(u64 s, u64 max) {
    if (s == 0 || max == 0) return false;
    /* Scan byte-by-byte, validating each new page right before reading it,
     * and stop at the NUL.  We must NOT pre-validate the whole `max` window:
     * a stack string near USER_STACK_TOP would push s+max past the top of
     * the user range and be wrongly rejected. */
    u64 cur_page = ~0ULL;
    for (u64 i = 0; i < max; i++) {
        u64 a = s + i;
        u64 pg = PAGE_ALIGN_DOWN(a);
        if (pg != cur_page) {
            cur_page = pg;
            if (a < USER_VFLOOR || a >= USER_STACK_TOP) return false;
            if (!page_readable_for_kernel(a)) return false;
        }
        char c;
        stac();
        c = ((const char *)s)[i];
        clac();
        if (c == 0) return true;
    }
    return false;
}

/* ---------------- init + selftest ---------------- */

#define SCRATCH 0x50000000UL

/* Set the NX bit on the ENTIRE HHDM direct map (PML4 index 256, base
 * 0xffff800000000000): kernel heap, kernel stacks, framebuffer, initrd
 * and driver DMA bounce buffers live there and none of it is ever
 * executed - this is the "kernel heap/data NX" hardening.  The kernel
 * image itself is mapped at 0xffffffff80000000 (PML4 index 511) by the
 * bootloader and stays executable.  Per-CPU page tables copy the kernel
 * half verbatim, so one pass on the boot PML4 covers every address space. */
void vmm_nx_direct_map(void) {
    u64 *pml4 = kernel_pml4;
    if (!(pml4[256] & PTE_PRESENT)) return;
    if (pml4[256] & PTE_HUGE) {                 /* 512 GiB huge page      */
        pml4[256] |= PTE_NX;
        goto done;
    }
    u64 *pdpt = (u64 *)phys_to_virt(pml4[256] & ~0xFFFULL & ~PTE_NX);
    for (int i = 0; i < 512; i++) {
        if (!(pdpt[i] & PTE_PRESENT)) continue;
        if (pdpt[i] & PTE_HUGE) {               /* 1 GiB page             */
            pdpt[i] |= PTE_NX;
            continue;
        }
        u64 *pd = (u64 *)phys_to_virt(pdpt[i] & ~0xFFFULL & ~PTE_NX);
        for (int j = 0; j < 512; j++) {
            if (!(pd[j] & PTE_PRESENT)) continue;
            if (pd[j] & PTE_HUGE) {             /* 2 MiB page             */
                pd[j] |= PTE_NX;
                continue;
            }
            u64 *pt = (u64 *)phys_to_virt(pd[j] & ~0xFFFULL & ~PTE_NX);
            for (int k = 0; k < 512; k++)
                if (pt[k] & PTE_PRESENT) pt[k] |= PTE_NX;
        }
    }
done:
    /* full TLB flush so the new NX bits take effect on EVERY CPU - a
     * local-only CR3 reload left the APs with executable direct-map
     * entries (heap code-exec on the other cores). */
    smp_tlb_shootdown_all();
    kprintf("vmm: HHDM direct map marked NX (heap/stacks/fb cannot execute)\n");
}


/* Guard-page helpers.  IMPORTANT: these walk kernel_pml4 directly - the
 * kernel half of every PML4 points at the SAME physical page tables, so
 * a change here is visible from every address space (and every CPU).  The
 * refcount is deliberately NOT touched: the guard frame stays allocated.
 *
 * The bootloader maps the direct map with HUGE pages (2 MiB or 1 GiB), so
 * clearing a single 4 KiB guard PTE requires SPLITTING the containing
 * huge page first.
 *
 * CRITICAL FIX: a 1 GiB page holds 512 x 2 MiB pages, so the split stride
 * is 0x200000 per entry - writing 0x40000000 (1 GiB) here left the direct
 * map covering only the first 2 MiB plus garbage, and every phys_to_virt
 * past 2 MiB faulted once the TLB evicted (the red-panic boot crash). */
static u64 *direct_pte_splitting(paddr_t p) {
    u64 va = (u64)phys_to_virt(p);
    u64 *pml4 = kernel_pml4;
    int i4 = (va >> 39) & 0x1FF;
    int i3 = (va >> 30) & 0x1FF;
    int i2 = (va >> 21) & 0x1FF;
    int i1 = (va >> 12) & 0x1FF;
    if (!(pml4[i4] & PTE_PRESENT)) return NULL;
    if (pml4[i4] & PTE_HUGE) return NULL;   /* 512 GiB page: fail open */
    u64 *pdpt = (u64 *)phys_to_virt(pml4[i4] & ~0xFFFULL & ~PTE_NX);
    if (!(pdpt[i3] & PTE_PRESENT)) return NULL;
    if (pdpt[i3] & PTE_HUGE) {
        /* 1 GiB page -> split into 512 x 2 MiB pages (stride 0x200000) */
        u64 base = pdpt[i3] & ~0x3FFFFFFFULL & ~PTE_NX;
        u64 fl   = pdpt[i3] & (PTE_PWT | PTE_PCD | PTE_NX);
        paddr_t pd_p = pmm_alloc_page();
        u64 *pd = (u64 *)phys_to_virt(pd_p);
        for (int j = 0; j < 512; j++)
            pd[j] = (base + (u64)j * 0x200000ULL) |
                    PTE_PRESENT | PTE_RW | PTE_HUGE | fl;
        pdpt[i3] = pd_p | PTE_PRESENT | PTE_RW |
                   (pml4[i4] & (PTE_PWT | PTE_PCD | PTE_NX));
    }
    u64 *pd = (u64 *)phys_to_virt(pdpt[i3] & ~0xFFFULL & ~PTE_NX);
    if (!(pd[i2] & PTE_PRESENT)) return NULL;
    if (pd[i2] & PTE_HUGE) {
        /* 2 MiB page -> split into 512 x 4 KiB pages */
        u64 base = pd[i2] & ~0x1FFFFFULL & ~PTE_NX;
        u64 fl   = pd[i2] & (PTE_PWT | PTE_PCD | PTE_NX);
        paddr_t pt_p = pmm_alloc_page();
        u64 *pt = (u64 *)phys_to_virt(pt_p);
        for (int j = 0; j < 512; j++)
            pt[j] = (base + (u64)j * PAGE_SIZE) |
                    PTE_PRESENT | PTE_RW | fl;
        pd[i2] = pt_p | PTE_PRESENT | PTE_RW |
                 (pdpt[i3] & (PTE_PWT | PTE_PCD | PTE_NX));
    }
    u64 *pt = (u64 *)phys_to_virt(pd[i2] & ~0xFFFULL & ~PTE_NX);
    return &pt[i1];
}

void vmm_unmap_direct_page(paddr_t p) {
    u64 *pte = direct_pte_splitting(p);
    if (!pte || !(*pte & PTE_PRESENT)) return;
    *pte = 0;
    /* Shoot down: the direct map is SHARED kernel page tables - another
     * CPU's TLB may still hold this page (kstack guard).  Local-only
     * invlpg left stale entries on the other cores. */
    smp_tlb_shootdown((u64)phys_to_virt(p));
}

void vmm_remap_direct_page(paddr_t p) {
    u64 *pte = direct_pte_splitting(p);
    if (!pte) return;
    *pte = (p & ~0xFFFULL) | PTE_PRESENT | PTE_RW | PTE_NX;
    smp_tlb_shootdown((u64)phys_to_virt(p));
}

void vmm_init(void) {
    paddr_t cr3 = read_cr3() & ~0xFFFULL;
    kernel_pml4 = (u64 *)phys_to_virt(cr3);
    g_boot_pml4 = kernel_pml4;
    memset(g_boot_regions, 0, sizeof g_boot_regions);
    g_boot_region_count = 0;
    /* Enable the No-eXecute bit: EFER.NXE must be set before any PTE_NX
     * is meaningful, or the CPU treats the bit as reserved and faults. */
    u64 efer = rdmsr64(0xC0000080);
    if (!(efer & (1ULL << 11))) {
        wrmsr64(0xC0000080, efer | (1ULL << 11));
        kprintf("vmm: NXE (no-execute) enabled\n");
    }
    u32 sm = smep_smap_enable();
    kprintf("vmm: SMEP=%s SMAP=%s (kernel cannot exec/access user pages)\n",
            (sm & 1) ? "on" : "n/a", (sm & 2) ? "on" : "n/a");
    /* NOTE: the HHDM direct map is marked NX in kmain AFTER
     * the APs come online (vmm_nx_direct_map) - Limine's SMP
     * trampoline executes from the direct map, so NX-before-APs
     * triple-faults every AP. */
    kprintf("vmm: PML4 @ phys %p (virt %p), HHDM offset %p\n",
            (void *)cr3, kernel_pml4, (void *)g_hhdm_offset);
    vmm_swap_init();
}

bool vmm_selftest(void) {
    kprintf("vmm: selftest\n");
    bool ok = true;
    stac();   /* the test reads/writes USER pages from kernel mode (SMAP) */

    /* --- demand paging --- */
    if (vmm_user_reserve(SCRATCH, 1, PTE_RW, VMM_USER_LAZY) != 0) {
        kprintf("  !! lazy reserve failed\n"); ok = false;
    }
    if (vmm_translate(SCRATCH) != 0) {
        kprintf("  !! lazy page materialized before touch\n"); ok = false;
    }
    if (!vmm_resolve_user_fault(SCRATCH, false)) {
        kprintf("  !! demand fault not resolved\n"); ok = false;
    }
    if (vmm_translate(SCRATCH) == 0) {
        kprintf("  !! demand page not mapped after fault\n"); ok = false;
    }
    vmm_user_release(SCRATCH);
    if (vmm_translate(SCRATCH) != 0) {
        kprintf("  !! released region still mapped\n"); ok = false;
    }

    /* --- copy-on-write --- */
    if (vmm_user_reserve(SCRATCH, 2, PTE_RW, VMM_USER_LAZY) != 0) {
        kprintf("  !! cow region reserve failed\n"); ok = false;
    }
    paddr_t shared = pmm_alloc_page();
    memset(phys_to_virt(shared), 0xAB, PAGE_SIZE);
    if (vmm_cow_map(SCRATCH, shared) != 0 ||
        vmm_cow_map(SCRATCH + PAGE_SIZE, shared) != 0) {
        kprintf("  !! cow_map failed\n"); ok = false;
    }
    if (pmm_refcount(shared) != 3) {
        kprintf("  !! cow refcount=%u expected 3\n", pmm_refcount(shared));
        ok = false;
    }
    if (!vmm_resolve_user_fault(SCRATCH, true)) {
        kprintf("  !! cow write not resolved\n"); ok = false;
    }
    if (pmm_refcount(shared) != 2) {
        kprintf("  !! shared refcount after one cow write=%u expected 2\n",
                pmm_refcount(shared));
        ok = false;
    }
    if (*(volatile u8 *)SCRATCH != 0xAB) {
        kprintf("  !! cow copy content mismatch\n"); ok = false;
    }
    *(volatile u8 *)SCRATCH = 0x11;
    if (*(volatile u8 *)(SCRATCH + PAGE_SIZE) != 0xAB) {
        kprintf("  !! alias saw private write (CoW violated)\n"); ok = false;
    }
    vmm_user_release(SCRATCH);
    vmm_user_release(SCRATCH + PAGE_SIZE);
    if (pmm_refcount(shared) != 1) {
        kprintf("  !! shared not back to 1 after cow unmaps\n"); ok = false;
    }
    pmm_unref_page(shared);

    /* --- swap: out + in restores content --- */
    if (vmm_user_reserve(SCRATCH, 1, PTE_RW, 0) != 0) {  /* eager map */
        kprintf("  !! swap region reserve failed\n"); ok = false;
    }
    *(volatile u64 *)SCRATCH = 0xDEADBEEF12345678ULL;
    if (vmm_swap_out(SCRATCH) < 0) {
        kprintf("  !! swap_out failed\n"); ok = false;
    } else {
        if (vmm_translate(SCRATCH) != 0) {
            kprintf("  !! swapped page still physically mapped\n"); ok = false;
        }
        if (!vmm_resolve_user_fault(SCRATCH, false)) {
            kprintf("  !! swap-in fault not resolved\n"); ok = false;
        } else {
            if (*(volatile u64 *)SCRATCH != 0xDEADBEEF12345678ULL) {
                kprintf("  !! swap lost page content\n"); ok = false;
            }
        }
    }
    vmm_user_release(SCRATCH);

    /* --- per-process tables: clone isolates --- */
    u64 *clone = vmm_clone_pml4();
    if (!clone) { kprintf("  !! clone failed\n"); ok = false; }
    else {
        paddr_t t = pmm_alloc_page();
        vmm_map_in(clone, SCRATCH, t, PTE_RW | PTE_US);
        if (!vmm_translate_in(clone, SCRATCH)) {
            kprintf("  !! clone missing its own mapping\n"); ok = false;
        }
        if (vmm_translate_in(kernel_pml4, SCRATCH)) {
            kprintf("  !! original saw the clone's mapping (no isolation)\n");
            ok = false;
        }
        u64 *pte = walk(clone, SCRATCH, false, 0);
        if (pte && (*pte & PTE_PRESENT)) {
            paddr_t phys = *pte & ~0xFFFULL & ~PTE_NX;
            *pte = 0;
            pmm_unref_page(phys);
        }
        vmm_free_pml4(clone);
    }

    if (vmm_user_region_count() != 0) {
        kprintf("  !! %lu regions leaked by selftest\n",
                (unsigned long)vmm_user_region_count());
        ok = false;
    }
    clac();
    kprintf("vmm: selftest %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

void *mmio_map(paddr_t p, size_t n) {
    vaddr_t start = (vaddr_t)phys_to_virt(p);
    vaddr_t end   = start + n;
    start &= ~(vaddr_t)0xFFF;
    end   = (end + 0xFFF) & ~(vaddr_t)0xFFF;
    for (vaddr_t v = start; v < end; v += 0x1000)
        vmm_map(v, (paddr_t)(v - g_hhdm_offset),
                PTE_PRESENT | PTE_RW | PTE_PCD | PTE_PWT | PTE_NX);
    return (void *)((vaddr_t)phys_to_virt(p));
}
