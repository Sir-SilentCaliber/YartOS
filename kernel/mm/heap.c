/* Yart OS - simple linked-list kernel heap on top of PMM/HHDM.
 *
 * Hardened with:
 *   - a magic value in every block header (catches kfree() of a pointer
 *     that is not a heap allocation, or a corrupted header),
 *   - a canary word right after the payload (catches buffer overruns),
 *   - explicit double-free detection (kfree of an already-free block).
 * None of these crash the kernel: they log loudly and skip the bad op, so
 * a bug is visible instead of silently corrupting memory.
 */
#include <yart/mm.h>
#include <yart/string.h>
#include <yart/console.h>
#include <yart/spinlock.h>

#define HEAP_MAGIC   0x59415254u     /* "YART"                          */
#define HEAP_CANARY  0xC0FFEE00u
#define CANARY_BYTES 16              /* must be 16 so split blocks stay 16-byte
                                        aligned (fxrstor/fxsave need it)    */

typedef struct block {
    u32   magic;          /* HEAP_MAGIC                                  */
    size_t size;          /* full payload capacity (may exceed request)  */
    size_t req;           /* the size the caller actually requested      */
    bool   free;
    struct block *next;
    struct block *prev;
} block_t;

static block_t *head;
static size_t   heap_pages_total;
static u64      g_heap_bad_frees;      /* detector counters              */
static u64      g_heap_bad_canaries;
static spinlock_t heap_lock;          /* SMP: alloc/free from any CPU    */

static block_t *grow(size_t need_payload) {
    size_t need = sizeof(block_t) + need_payload;
    size_t pages = PAGE_ALIGN_UP(need) / PAGE_SIZE;
    if (pages < 8) pages = 8;
    paddr_t p = pmm_alloc_pages(pages);
    block_t *b = (block_t *)phys_to_virt(p);
    b->magic = HEAP_MAGIC;
    b->size = pages * PAGE_SIZE - sizeof(block_t);
    b->req  = 0;
    b->free = true;
    b->next = NULL;
    b->prev = NULL;
    if (!head) {
        head = b;
    } else {
        block_t *t = head; while (t->next) t = t->next;
        t->next = b; b->prev = t;
    }
    heap_pages_total += pages;
    return b;
}

void heap_init(void) {
    head = NULL;
    heap_pages_total = 0;
    g_heap_bad_frees = 0;
    g_heap_bad_canaries = 0;
    spin_init(&heap_lock);
    grow(MB(1));
    kprintf("heap: initial %lu KiB (canaries + double-free detection on)\n",
            heap_pages_total * PAGE_SIZE / 1024);
}

static void split(block_t *b, size_t want) {
    if (b->size < want + sizeof(block_t) + CANARY_BYTES + 32) return;
    block_t *n = (block_t *)((u8 *)b + sizeof(block_t) + want + CANARY_BYTES);
    n->magic = HEAP_MAGIC;
    n->size = b->size - want - sizeof(block_t) - CANARY_BYTES;
    n->req  = 0;
    n->free = true;
    n->next = b->next;
    n->prev = b;
    if (b->next) b->next->prev = n;
    b->next = n;
    b->size = want;
}

static void *payload(block_t *b) { return (void *)((u8 *)b + sizeof(block_t)); }
static u32 *canary_of(block_t *b) {
    return (u32 *)((u8 *)payload(b) + b->req);
}

void *kmalloc(size_t n) {
    if (!n) return NULL;
    n = (n + 15) & ~15ULL;
    u64 fl = irq_save();
    spin_lock(&heap_lock);
    for (block_t *b = head; b; b = b->next) {
        if (b->free && b->size >= n) {
            split(b, n);
            b->free = false;
            b->req = n;
            void *p = payload(b);
            *canary_of(b) = HEAP_CANARY;   /* plant the overflow canary  */
            spin_unlock(&heap_lock);
            irq_restore(fl);
            return p;
        }
    }
    block_t *b = grow(n);
    split(b, n);
    b->free = false;
    b->req = n;
    void *p = payload(b);
    *canary_of(b) = HEAP_CANARY;
    spin_unlock(&heap_lock);
    irq_restore(fl);
    return p;
}

void *kzalloc(size_t n) {
    void *p = kmalloc(n);
    if (p) memset(p, 0, n);
    return p;
}

void kfree(void *p) {
    if (!p) return;
    u64 fl = irq_save();
    spin_lock(&heap_lock);
    block_t *b = (block_t *)((u8 *)p - sizeof(block_t));

    /* not a heap block (bad pointer / header corrupted) */
    if (b->magic != HEAP_MAGIC) {
        g_heap_bad_frees++;
        spin_unlock(&heap_lock);
        irq_restore(fl);
        kprintf("heap: !! kfree of non-heap pointer %p (ignored)\n", p);
        return;
    }
    /* double free */
    if (b->free) {
        g_heap_bad_frees++;
        spin_unlock(&heap_lock);
        irq_restore(fl);
        kprintf("heap: !! double free of %p (ignored)\n", p);
        return;
    }
    /* overflow canary check */
    if (*canary_of(b) != HEAP_CANARY) {
        g_heap_bad_canaries++;
        kprintf("heap: !! buffer overflow detected in block %p (size=%lu)\n",
                p, b->size);
        /* still free it - the corruption is already logged */
    }

    b->free = true;
    /* coalesce */
    if (b->next && b->next->free) {
        b->size += sizeof(block_t) + CANARY_BYTES + b->next->size;
        b->next = b->next->next;
        if (b->next) b->next->prev = b;
    }
    if (b->prev && b->prev->free) {
        b->prev->size += sizeof(block_t) + CANARY_BYTES + b->size;
        b->prev->next = b->next;
        if (b->next) b->next->prev = b->prev;
    }
    spin_unlock(&heap_lock);
    irq_restore(fl);
}

/* Boot-time heap selftest: alloc/free accounting, double-free detection,
 * overflow detection.  Must leave the heap in a clean state. */
void heap_selftest(void) {
    kprintf("heap: selftest\n");
    bool ok = true;

    void *a = kmalloc(64);
    void *b = kmalloc(128);
    if (!a || !b) { kprintf("  !! alloc failed\n"); ok = false; }
    memset(a, 0xAB, 64);                 /* in-bounds write              */

    /* double-free must be detected, not crash */
    u64 f0 = g_heap_bad_frees;
    kfree(a);
    kfree(a);                            /* second free -> detector fires */
    if (g_heap_bad_frees != f0 + 1) { kprintf("  !! double-free not caught\n"); ok = false; }

    /* overflow must be detected */
    u64 c0 = g_heap_bad_canaries;
    char *c = kmalloc(32);
    if (!c) { kprintf("  !! alloc failed\n"); ok = false; }
    memset(c + 32, 0xEE, 8);             /* write PAST the payload        */
    kfree(c);                            /* canary check fires            */
    if (g_heap_bad_canaries != c0 + 1) { kprintf("  !! overflow not caught\n"); ok = false; }

    kfree(b);
    kprintf("heap: selftest %s (bad-frees=%llu overflow=%llu)\n",
            ok ? "PASS" : "FAIL",
            (unsigned long long)g_heap_bad_frees,
            (unsigned long long)g_heap_bad_canaries);
}
