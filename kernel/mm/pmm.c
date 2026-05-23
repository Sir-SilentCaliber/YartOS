/* Yart OS - Physical Memory Manager (bitmap allocator) */
#include <yart/mm.h>
#include <yart/console.h>
#include <yart/string.h>

static u64    *bitmap;
static size_t  bitmap_pages;
static size_t  total_pages;
static size_t  used_pages;
static size_t  last_idx;

#define BIT_SET(i)   (bitmap[(i)/64] |=  (1ULL << ((i)%64)))
#define BIT_CLR(i)   (bitmap[(i)/64] &= ~(1ULL << ((i)%64)))
#define BIT_TST(i)   (bitmap[(i)/64] &   (1ULL << ((i)%64)))

void pmm_init(struct limine_memmap_response *mm) {
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
    size_t bm_bytes = (total_pages + 7) / 8;
    bm_bytes = PAGE_ALIGN_UP(bm_bytes);

    /* find a usable region big enough to host the bitmap */
    bitmap = NULL;
    for (u64 i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (e->type == LIMINE_MEMMAP_USABLE && e->length >= bm_bytes) {
            bitmap = (u64 *)phys_to_virt(e->base);
            memset(bitmap, 0xFF, bm_bytes);   /* mark everything used */
            e->base   += bm_bytes;
            e->length -= bm_bytes;
            bitmap_pages = bm_bytes / PAGE_SIZE;
            break;
        }
    }
    if (!bitmap) kpanic("pmm: no region big enough for bitmap");

    /* now mark usable regions free */
    for (u64 i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE) continue;
        for (u64 a = e->base; a + PAGE_SIZE <= e->base + e->length; a += PAGE_SIZE) {
            BIT_CLR(a / PAGE_SIZE);
        }
    }

    /* count used */
    used_pages = 0;
    for (size_t i = 0; i < total_pages; i++) if (BIT_TST(i)) used_pages++;

    kprintf("pmm: %lu MiB total, %lu MiB free, bitmap@%p (%lu pages)\n",
            total_pages * PAGE_SIZE / MB(1),
            (total_pages - used_pages) * PAGE_SIZE / MB(1),
            bitmap, bitmap_pages);
}

paddr_t pmm_alloc_page(void) {
    for (size_t step = 0; step < 2; step++) {
        size_t start = step == 0 ? last_idx : 0;
        size_t end   = step == 0 ? total_pages : last_idx;
        for (size_t i = start; i < end; i++) {
            if (!BIT_TST(i)) {
                BIT_SET(i);
                used_pages++;
                last_idx = i + 1;
                paddr_t p = (paddr_t)i * PAGE_SIZE;
                memset(phys_to_virt(p), 0, PAGE_SIZE);
                return p;
            }
        }
    }
    kpanic("pmm: out of memory");
}

paddr_t pmm_alloc_pages(size_t n) {
    if (n == 1) return pmm_alloc_page();
    /* simple linear scan for n contiguous pages */
    size_t run = 0, run_start = 0;
    for (size_t i = 0; i < total_pages; i++) {
        if (!BIT_TST(i)) {
            if (run == 0) run_start = i;
            run++;
            if (run == n) {
                for (size_t k = 0; k < n; k++) BIT_SET(run_start + k);
                used_pages += n;
                paddr_t p = (paddr_t)run_start * PAGE_SIZE;
                memset(phys_to_virt(p), 0, n * PAGE_SIZE);
                return p;
            }
        } else {
            run = 0;
        }
    }
    kpanic("pmm: out of contiguous memory (%lu pages)", n);
}

void pmm_free_page(paddr_t p) {
    size_t i = p / PAGE_SIZE;
    if (!BIT_TST(i)) return;
    BIT_CLR(i);
    used_pages--;
    if (i < last_idx) last_idx = i;
}

size_t pmm_total_pages(void) { return total_pages; }
size_t pmm_used_pages(void)  { return used_pages; }
