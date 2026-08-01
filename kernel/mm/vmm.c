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

u64 g_hhdm_offset = 0;

static u64 *kernel_pml4;        /* the bootloader's tables (kernel identity) */
/* What CR3 points at right NOW on THIS CPU.  Per-CPU on SMP: the BSP and
 * every AP run different tasks with different page tables, so a single
 * global would be wrong for everyone but the last CPU to switch.
 * g_boot_pml4 is the fallback used before/without a per-CPU area. */
static u64 *g_boot_pml4;
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
    u64 *pte = walk(cur_pml4(), v, false, 0);
    if (!pte || !(*pte & PTE_PRESENT)) return;
    paddr_t phys = *pte & ~0xFFFULL & ~PTE_NX;
    *pte = (phys & ~0xFFFULL) | flags | PTE_PRESENT;
    invlpg(v);
}

void vmm_unmap(vaddr_t v) {
    u64 *pte = walk(cur_pml4(), v, false, 0);
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

/* Split the boot-prepared user address space off into its own page tables:
 * clone the current (kernel) tables, CoW-share the prepared user pages,
 * drop the kernel's own copies, and hand regions + PML4 to the task. */
void vmm_give_current_regions_to(task_t *t) {
    int n = g_boot_region_count;
    if (n <= 0) return;
    u64 *clone = vmm_clone_pml4();
    if (!clone) {
        kprintf("vmm: clone failed - task shares kernel tables\n");
        return;
    }
    vmm_cow_fork(clone);                 /* share user pages RO (CoW)    */
    /* release the kernel's own copies of those pages (unref each)        */
    for (int i = 0; i < n; i++) {
        user_region_t r = g_boot_regions[i];
        for (u64 a = r.start; a < r.start + r.npages * PAGE_SIZE;
             a += PAGE_SIZE) {
            u64 *pte = walk(kernel_pml4, a, false, 0);
            if (pte && (*pte & PTE_PRESENT)) {
                paddr_t phys = *pte & ~0xFFFULL & ~PTE_NX;
                *pte = 0;
                invlpg(a);
                pmm_unref_page(phys);
            }
        }
    }
    vmm_take_boot_regions(t);
    t->pml4 = clone;
    kprintf("vmm: task [%u] now has private page tables (CR3 switched on schedule)\n",
            t->pid);
}

int vmm_user_reserve(u64 va, u64 npages, u64 flags, u32 opts) {
    if (npages == 0) return -1;
    /* per-task memory cap: charging happens even for lazy (demand) regions,
     * because the pages WILL be faulted in later */
    if (sched_current_is_user() && sched_mem_used() + npages > sched_mem_limit()) {
        kprintf("vmm: pid %d over memory cap (%llu pages)\n", sched_current()->pid,
                (unsigned long long)sched_mem_limit());
        return -1;
    }
    sched_charge_pages((i64)npages);
    u64 start = PAGE_ALIGN_DOWN(va);
    u64 end   = start + npages * PAGE_SIZE;
    if (end < start) return -1;
    if (start < USER_VBASE || end > USER_STACK_TOP) return -1;
    int n; user_region_t *rs = active_regions(&n);
    for (int i = 0; i < n; i++) {
        u64 r_end = rs[i].start + rs[i].npages * PAGE_SIZE;
        if (start < r_end && end > rs[i].start) return -1;   /* overlap */
    }
    if (n >= MAX_USER_REGIONS) return -1;
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
        vmm_swap_in(page);
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
        vmm_map(page, p, r->flags);
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
            u64 entry = (phys & ~0xFFFULL) | PTE_PRESENT | PTE_US | PTE_COW;
            *p = entry;                          /* parent -> RO           */
            u64 *cp = walk(child_pml4, a, true, PTE_US);
            *cp = entry;                         /* child  -> RO           */
            invlpg(a);
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

void vmm_free_pml4(u64 *pml4) {
    if (!pml4 || pml4 == kernel_pml4) return;
    if (pml4[0] & PTE_PRESENT) {
        u64 *pdpt = phys_to_virt(pml4[0] & ~0xFFFULL & ~PTE_NX);
        for (int i3 = 0; i3 < 512; i3++) {
            if (!(pdpt[i3] & PTE_PRESENT)) continue;
            u64 *pd = phys_to_virt(pdpt[i3] & ~0xFFFULL & ~PTE_NX);
            for (int i2 = 0; i2 < 512; i2++)
                if (pd[i2] & PTE_PRESENT)
                    pmm_unref_page(pd[i2] & ~0xFFFULL & ~PTE_NX);
            pmm_unref_page(pdpt[i3] & ~0xFFFULL & ~PTE_NX);
        }
        pmm_unref_page(pml4[0] & ~0xFFFULL & ~PTE_NX);
    }
    pmm_unref_page(virt_to_phys(pml4));
}

/* ---------------- swap (RAM-backed) ---------------- */

#define SWAP_SLOTS 1024
#define SWAP_CPUS 1        /* one pool per CPU; SMP grows this to NR_CPUS */
static u8  *swap_pool[SWAP_CPUS];
static u64  swap_used[SWAP_CPUS][SWAP_SLOTS / 64];

static int swap_cpu(void) {
    /* per-CPU swap pools: each CPU evicts into its OWN pool so two CPUs
     * never race on slot allocation */
    cpu_local_t *c = get_cpu_local();
    return c ? (int)(c->cpu_id % SWAP_CPUS) : 0;
}

void vmm_swap_init(void) {
    if (swap_pool[0]) return;
    for (int c = 0; c < SWAP_CPUS; c++) {
        paddr_t p = pmm_alloc_pages(SWAP_SLOTS);
        swap_pool[c] = phys_to_virt(p);
        memset(swap_used[c], 0, sizeof swap_used[c]);
    }
    kprintf("swap: %u KiB RAM-backed pool%s @%p\n",
            SWAP_SLOTS * PAGE_SIZE / 1024,
            SWAP_CPUS > 1 ? "s" : "", swap_pool[0]);
}

static int swap_alloc_slot(void) {
    int c = swap_cpu();
    for (int i = 0; i < SWAP_SLOTS; i++)
        if (!(swap_used[c][i / 64] & (1ULL << (i % 64)))) {
            swap_used[c][i / 64] |= (1ULL << (i % 64));
            return i;
        }
    return -1;
}
static void swap_free_slot(int s) {
    int c = swap_cpu();
    swap_used[c][s / 64] &= ~(1ULL << (s % 64));
}

int vmm_swap_out(u64 va) {
    u64 page = PAGE_ALIGN_DOWN(va);
    u64 *pte = walk(cur_pml4(), page, false, 0);
    if (!pte || !(*pte & PTE_PRESENT) || (*pte & PTE_SWAP)) return -1;
    int slot = swap_alloc_slot();
    if (slot < 0) return -1;
    paddr_t phys = *pte & ~0xFFFULL & ~PTE_NX;
    memcpy(swap_pool[swap_cpu()] + (u64)slot * PAGE_SIZE, phys_to_virt(phys), PAGE_SIZE);
    pmm_unref_page(phys);
    *pte = ((u64)slot << 12) | PTE_SWAP;         /* not-present + marker   */
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
    memcpy(phys_to_virt(p), swap_pool[swap_cpu()] + (u64)slot * PAGE_SIZE, PAGE_SIZE);
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
            if (vmm_swap_out(a) >= 0) done++;
        }
    }
    return done;
}

/* ---------------- user-pointer validation ---------------- */

bool vmm_user_range_ok(u64 va, u64 len) {
    if (len == 0) return true;
    if (va + len < va) return false;
    if (va < USER_VBASE || va + len > USER_STACK_TOP) return false;
    u64 first = PAGE_ALIGN_DOWN(va);
    u64 last  = PAGE_ALIGN_UP(va + len);
    for (u64 a = first; a < last; a += PAGE_SIZE) {
        u64 *pte = walk(cur_pml4(), a, false, 0);
        if (pte && (*pte & PTE_PRESENT)) {
            if (!(*pte & PTE_US)) return false;
            continue;
        }
        if (!find_region(a)) return false;       /* unmapped, not reserved */
    }
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
            if (a < USER_VBASE || a >= USER_STACK_TOP) return false;
            u64 *pte = walk(cur_pml4(), a, false, 0);
            if (pte && (*pte & PTE_PRESENT)) {
                if (!(*pte & PTE_US)) return false;
            } else if (!find_region(a)) {
                return false;
            }
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
