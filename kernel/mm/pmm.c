/* Yart OS - Physical Memory Manager (bitmap allocator + per-page refcounts)
 *
 * Every physical page has a reference count:
 *   - pmm_alloc_page()          -> count = 1 (owner is whoever mapped it)
 *   - pmm_ref_page(p) / cow_map -> count++ (a second mapping shares it)
 *   - pmm_unref_page(p) / unmap -> count-- ; when it hits 0 the page is
 *                                  zeroed (privacy) and returned to the
 *                                  allocator
 * This is the substrate that lets the VMM do copy-on-write safely, and it
 * turns double-frees / frees-of-foreign-pages into detectable kernel bugs.
 */
#include <yart/mm.h>
#include <yart/console.h>
#include <yart/string.h>
#include <yart/spinlock.h>

static u64    *bitmap;
static u16    *refs;             /* one refcount per physical page      */
static size_t  bitmap_pages;
static size_t  total_pages;
static size_t  used_pages;
static size_t  last_idx;

static u64 g_double_free_count;  /* kernel-bug detector counters        */
static u64 g_bad_ref_count;

/* SMP: the bitmap + refcounts are global, so every alloc/ref/unref runs
 * with this lock held (APs free user pages when their tasks exit while the
 * BSP allocates for new ones).  The lock is never held across vmm_evict_some
 * (which calls back into pmm_unref_page) - see pmm_alloc_page. */
static spinlock_t pmm_lock;

#define BIT_SET(i)   (bitmap[(i)/64] |=  (1ULL << ((i)%64)))
#define BIT_CLR(i)   (bitmap[(i)/64] &= ~(1ULL << ((i)%64)))
#define BIT_TST(i)   (bitmap[(i)/64] &   (1ULL << ((i)%64)))

/* Carve `need` bytes out of the first usable memmap region that fits and
 * return its virtual address (the region is shrunk so the free-marking pass
 * never hands those pages out). */
static void *carve_region(struct limine_memmap_response *mm, size_t need) {
    for (u64 i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (e->type == LIMINE_MEMMAP_USABLE && e->length >= need) {
            void *p = phys_to_virt(e->base);
            e->base   += need;
            e->length -= need;
            return p;
        }
    }
    return NULL;
}

void pmm_init(struct limine_memmap_response *mm) {
    spin_init(&pmm_lock);
    /* find highest usable address */
    u64 hi = 0;
    for (u64 i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (e->type == LIMINE_MEMMAP_USABLE) {
            u64 top = e->base + e->length;
            if (top > hi) hi = top;
        }
    }
    total_pages = hi / PAGE_SIZE;
    size_t bm_bytes = PAGE_ALIGN_UP((total_pages + 7) / 8);
    size_t rf_bytes = PAGE_ALIGN_UP(total_pages * sizeof(u16));

    bitmap = carve_region(mm, bm_bytes);
    if (!bitmap) kpanic("pmm: no region big enough for bitmap");
    bitmap_pages = bm_bytes / PAGE_SIZE;
    memset(bitmap, 0xFF, bm_bytes);            /* everything used */

    refs = carve_region(mm, rf_bytes);
    if (!refs) kpanic("pmm: no region big enough for refcount table");
    memset(refs, 0, rf_bytes);                 /* nothing referenced yet   */

    /* mark usable regions free (bitmap + refs memory was carved out above,
     * so those pages stay marked used) */
    for (u64 i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE) continue;
        for (u64 a = e->base; a + PAGE_SIZE <= e->base + e->length; a += PAGE_SIZE)
            BIT_CLR(a / PAGE_SIZE);
    }

    /* count used */
    used_pages = 0;
    for (size_t i = 0; i < total_pages; i++) if (BIT_TST(i)) used_pages++;

    kprintf("pmm: %lu MiB total, %lu MiB free, bitmap@%p (%lu pg) refs@%p (%lu pg)\n",
            total_pages * PAGE_SIZE / MB(1),
            (total_pages - used_pages) * PAGE_SIZE / MB(1),
            bitmap, bitmap_pages, refs, rf_bytes / PAGE_SIZE);
}

static paddr_t alloc_idx(size_t i) {
    BIT_SET(i);
    used_pages++;
    last_idx = i + 1;
    refs[i] = 1;                               /* owner starts at 1        */
    paddr_t p = (paddr_t)i * PAGE_SIZE;
    memset(phys_to_virt(p), 0, PAGE_SIZE);
    return p;
}

/* ---- OOM killer ---- */
extern int sched_oom_kill_one(void);   /* sched.c: kill the hungriest user task
                                          (1 = killed another task, 0 = nothing,
                                          -1 = killed the current faulting task) */
#define OOM_MAX_KILLS 16
static bool g_oom_force;               /* OOM selftest debug hook           */

void pmm_oom_test_force(bool on) { g_oom_force = on; }

static paddr_t find_free_locked(void) {
    for (size_t step = 0; step < 2; step++) {
        size_t start = step == 0 ? last_idx : 0;
        size_t end   = step == 0 ? total_pages : last_idx;
        for (size_t i = start; i < end; i++)
            if (!BIT_TST(i))
                return alloc_idx(i);
    }
    return 0;
}

paddr_t pmm_alloc_page(void) {
    /* two attempts; between them, swap out user pages to relieve pressure.
     * The eviction runs WITHOUT the lock: it calls back into pmm_unref_page
     * (refcounts) and a spinlock is not re-entrant. */
    for (int attempt = 0; attempt < 2 && !g_oom_force; attempt++) {
        spin_lock(&pmm_lock);
        paddr_t p = find_free_locked();
        spin_unlock(&pmm_lock);
        if (p) return p;
        if (attempt == 0)
            vmm_evict_some(16);       /* make room by swapping user pages */
    }

    /* OOM: instead of panicking, kill user tasks (hungriest first) and retry,
     * so one memory-hogging app cannot take down the whole OS.  Killing a
     * task frees its physical pages via sched_kill/vmm_free_pml4. */
    for (int attempt = 0; attempt < OOM_MAX_KILLS; attempt++) {
        int r = sched_oom_kill_one();
        if (r == 0) break;                    /* nothing left to reclaim */
        if (r < 0) return 0;                  /* killed the faulting task; let
                                                 the fault path kill it cleanly */
        spin_lock(&pmm_lock);
        paddr_t p = find_free_locked();
        spin_unlock(&pmm_lock);
        if (p) return p;                      /* reclaimed enough */
    }
    g_oom_force = false;
    kpanic("pmm: out of memory (no reclaimable user task)");
}

paddr_t pmm_alloc_pages(size_t n) {
    if (n == 1) return pmm_alloc_page();
    size_t run = 0, run_start = 0;
    spin_lock(&pmm_lock);
    for (size_t i = 0; i < total_pages; i++) {
        if (!BIT_TST(i)) {
            if (run == 0) run_start = i;
            if (++run == n) {
                for (size_t k = 0; k < n; k++) alloc_idx(run_start + k);
                spin_unlock(&pmm_lock);
                return (paddr_t)run_start * PAGE_SIZE;
            }
        } else {
            run = 0;
        }
    }
    spin_unlock(&pmm_lock);
    kpanic("pmm: out of contiguous memory (%lu pages)", n);
}

/* An extra reference on an allocated page (CoW sharing, aliased mappings). */
void pmm_ref_page(paddr_t p) {
    spin_lock(&pmm_lock);
    size_t i = p / PAGE_SIZE;
    if (i >= total_pages || !BIT_TST(i)) {
        g_bad_ref_count++;
        spin_unlock(&pmm_lock);
        kprintf("pmm: !! ref of non-allocated page %p\n", (void *)p);
        return;
    }
    if (refs[i] < 0xFFFF) refs[i]++;
    spin_unlock(&pmm_lock);
}

u32 pmm_refcount(paddr_t p) {
    size_t i = p / PAGE_SIZE;
    if (i >= total_pages) return 0;
    return refs[i];
}

/* Drop one reference; free (zero + release) when it reaches zero.  Detects
 * double-frees and frees of pages that were never allocated. */
void pmm_unref_page(paddr_t p) {
    spin_lock(&pmm_lock);
    size_t i = p / PAGE_SIZE;
    if (i >= total_pages || !BIT_TST(i)) {
        g_double_free_count++;
        spin_unlock(&pmm_lock);
        kprintf("pmm: !! free of non-allocated page %p\n", (void *)p);
        return;
    }
    if (refs[i] == 0) {
        g_double_free_count++;
        spin_unlock(&pmm_lock);
        kprintf("pmm: !! double free of page %p\n", (void *)p);
        return;
    }
    if (--refs[i] == 0) {
        memset(phys_to_virt(p), 0, PAGE_SIZE);   /* zero-on-free (privacy) */
        BIT_CLR(i);
        used_pages--;
        if (i < last_idx) last_idx = i;
    }
    spin_unlock(&pmm_lock);
}

void pmm_free_page(paddr_t p)  { pmm_unref_page(p); }
void pmm_free_pages(paddr_t p, size_t n) {
    for (size_t k = 0; k < n; k++) pmm_unref_page(p + k * PAGE_SIZE);
}

size_t pmm_total_pages(void) { return total_pages; }
size_t pmm_used_pages(void)  { return used_pages; }

/* Mark a specific physical page as used (e.g. the low trampoline page). */
void pmm_mark_page_used(paddr_t p) {
    spin_lock(&pmm_lock);
    size_t i = p / PAGE_SIZE;
    if (i >= total_pages || BIT_TST(i)) { spin_unlock(&pmm_lock); return; }
    BIT_SET(i);
    used_pages++;
    refs[i] = 1;
    spin_unlock(&pmm_lock);
}

/* Reserve a contiguous range (e.g. the bootloader's initrd module: Limine
 * loads modules into memory the memmap still reports as USABLE, so the
 * PMM would happily allocate + ZERO those frames - destroying the initrd
 * the moment a heap grow lands inside it during vfs import).  Called
 * BEFORE any pmm allocation. */
void pmm_mark_range_used(paddr_t base, size_t npages) {
    for (size_t k = 0; k < npages; k++)
        pmm_mark_page_used(base + k * PAGE_SIZE);
}

bool pmm_page_used(paddr_t p) {
    size_t i = p / PAGE_SIZE;
    if (i >= total_pages) return true;
    return BIT_TST(i);
}

u32 pmm_page_refs(paddr_t p) {
    size_t i = p / PAGE_SIZE;
    if (i >= total_pages) return 0;
    return refs[i];
}

/* Boot-time battery: alloc/ref/unref/free accounting, zero-on-free, and
 * double-free detection.  Returns true when everything checks out. */
bool pmm_selftest(void) {
    kprintf("pmm: selftest\n");
    bool ok = true;
    size_t used0 = used_pages;

    paddr_t a = pmm_alloc_page();
    paddr_t b = pmm_alloc_page();
    /* fresh pages must be zeroed */
    ok &= (*(volatile u64 *)phys_to_virt(a) == 0);

    pmm_ref_page(a);                                  /* 1 -> 2 */
    if (pmm_refcount(a) != 2) { kprintf("  !! refcount != 2 after ref\n"); ok = false; }
    pmm_unref_page(a);                                /* 2 -> 1, not freed */
    if (pmm_refcount(a) != 1 || !BIT_TST(a / PAGE_SIZE)) {
        kprintf("  !! unref to 1 freed the page early\n"); ok = false;
    }
    pmm_unref_page(a);                                /* 1 -> 0 -> freed */
    if (pmm_refcount(a) != 0) { kprintf("  !! page not freed at refcount 0\n"); ok = false; }
    /* zero-on-free must have wiped it */
    if (*(volatile u64 *)phys_to_virt(a) != 0) { kprintf("  !! zero-on-free failed\n"); ok = false; }

    /* double-free must be detected (logged, not fatal) */
    u64 df0 = g_double_free_count;
    pmm_unref_page(a);                                /* already freed */
    if (g_double_free_count != df0 + 1) { kprintf("  !! double-free not detected\n"); ok = false; }

    pmm_unref_page(b);                                /* free b */
    if (used_pages != used0) {
        kprintf("  !! used_pages drift: %lu -> %lu\n", used0, used_pages);
        ok = false;
    }
    kprintf("pmm: selftest %s (used=%lu double-frees=%llu)\n",
            ok ? "PASS" : "FAIL", (unsigned long)used_pages,
            (unsigned long long)g_double_free_count);
    return ok;
}
