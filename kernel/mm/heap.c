/* Yart OS - size-class (slab-style) kernel heap on top of PMM/HHDM.
 *
 * v2 (row 12): the old single linked-list heap scanned every free block on
 * every kmalloc (O(n)).  This version keeps the exact same hardened block
 * model (magic header, canary per request, double-free / non-heap-free
 * detection, coalescing) but adds a **segregated size-class free list**:
 *
 *   - Free blocks live in a bucket per size class (16..16384, then "huge").
 *   - kmalloc rounds the request up to a class and pops the head of that
 *     bucket: an **O(1) fast path** for the common case.  Only on a miss
 *     does it fall through to a larger bucket or grow a fresh region.
 *   - kfree coalesces with its address-ordered neighbours (O(1)) and then
 *     pushes the result onto its size class bucket (O(1)).
 *   - Alignment >16 is supported via kmalloc_aligned()/kfree_aligned().
 *   - Full allocation statistics are tracked and printed by the selftest.
 *
 * Hardening (kept + extended):
 *   - a magic value in every block header (kfree of a non-heap pointer or
 *     a corrupted header is detected),
 *   - a canary word planted at the requested-size boundary (buffer overruns
 *     past the request are detected),
 *   - explicit double-free detection.
 * None of these crash the kernel: they log loudly and skip the bad op.
 *
 * Block layout (unchanged from v1, so 16-byte alignment of every payload
 * is preserved for fxsave/fxrstor):
 *   [ block_t header ][ payload: b->size bytes ][ CANARY_BYTES gap ][ ... ]
 *   b->size is the payload capacity; the canary sits at payload + b->req
 *   (inside the 16-byte gap), and split() puts the next block after
 *   header + payload + CANARY_BYTES.
 */
#include <yart/mm.h>
#include <yart/string.h>
#include <yart/console.h>
#include <yart/spinlock.h>

#define HEAP_MAGIC   0x59415254u     /* "YART"                          */
#define HEAP_CANARY  0xC0FFEE00u
#define CANARY_BYTES 16              /* must be 16 so split blocks stay 16-byte
                                        aligned (fxrstor/fxsave need it)    */

/* ---- size classes ---------------------------------------------------
 * A request is served from the first bucket whose class >= the request.
 * The exact-class hit is the O(1) fast path.  16-byte granularity near the
 * bottom (where most kernel objects live) limits internal fragmentation;
 * classes then widen as they grow. */
static const size_t classes[] = {
    16, 32, 48, 64, 96, 128, 192, 256,
    384, 512, 768, 1024, 1536, 2048, 3072, 4096,
    6144, 8192, 12288, 16384
};
#define NUM_CLASSES  HEAP_NUM_CLASSES
#define HUGE_CLASS   NUM_CLASSES   /* anything larger than classes[]        */
#define NUM_BUCKETS  HEAP_NUM_BUCKETS
_Static_assert((int)(sizeof(classes) / sizeof(classes[0])) == HEAP_NUM_CLASSES,
               "heap classes[] size must match HEAP_NUM_CLASSES");

typedef struct block {
    u32   magic;          /* HEAP_MAGIC                                  */
    size_t size;          /* payload capacity (may exceed request)       */
    size_t req;           /* requested size while allocated; for a FREE   */
                          /*   block this holds the bucket free-list next */
    bool   free;
    struct block *next;   /* address-ordered list (ALL blocks, for       */
    struct block *prev;   /*   O(1) coalescing + grow + walk)            */
} block_t;                /* 48 bytes -> 16-aligned payload              */

static block_t *head;                 /* address-ordered list of all blocks */
static block_t *buckets[NUM_BUCKETS]; /* segregated free lists by size class */
static size_t   heap_pages_total;
static spinlock_t heap_lock;          /* SMP: alloc/free from any CPU        */

static heap_stats_t g_stats;          /* allocation statistics               */
static u64 g_heap_bad_frees;          /* detector counters                   */
static u64 g_heap_bad_canaries;

/* The bucket linkage of a FREE block is stored in the block header's `req`
 * field (only meaningful for allocated blocks; 0 while free).  This needs
 * no extra pointer (block_t stays 16-aligned) AND freed payloads are never
 * written - exactly like the v1 heap.  (Writing the link into the payload's
 * first 8 bytes would clobber the first fields of a freed object, e.g. a
 * task_t's pid/ppid, and expose latent use-after-free races in callers such
 * as the scheduler.) */
static inline block_t *bfree_next(block_t *b) {
    return (block_t *)(uintptr_t)b->req;
}
static inline void bfree_set_next(block_t *b, block_t *n) {
    b->req = (size_t)(uintptr_t)n;
}

static int class_for(size_t n) {
    for (int i = 0; i < NUM_CLASSES; i++)
        if (n <= classes[i]) return i;
    return HUGE_CLASS;
}

static void *payload(block_t *b) { return (void *)((u8 *)b + sizeof(block_t)); }
static u32 *canary_of(block_t *b) { return (u32 *)((u8 *)payload(b) + b->req); }

/* ---- bucket ops (all O(1) except the rarely-hit bucket_remove) ---- */
static void bucket_push(block_t *b, int ci) {
    bfree_set_next(b, buckets[ci]);
    buckets[ci] = b;
}
static block_t *bucket_pop(int ci) {
    block_t *b = buckets[ci];
    if (!b) return NULL;
    buckets[ci] = bfree_next(b);
    return b;
}
/* Unlink a free block from its bucket (used when coalescing a free
 * neighbour).  Buckets are short so the scan is cheap. */
static void bucket_remove(block_t *b) {
    int ci = class_for(b->size);
    if (buckets[ci] == b) { buckets[ci] = bfree_next(b); return; }
    for (block_t *t = buckets[ci]; t; t = bfree_next(t)) {
        if (bfree_next(t) == b) { bfree_set_next(t, bfree_next(b)); return; }
    }
}

/* ---- grow: fetch a fresh page-run from the PMM and add one free block */
static block_t *grow(size_t need_payload) {
    size_t need = sizeof(block_t) + need_payload;
    size_t pages = PAGE_ALIGN_UP(need) / PAGE_SIZE;
    if (pages < 8) pages = 8;
    paddr_t p = pmm_alloc_pages(pages);
    block_t *b = (block_t *)phys_to_virt(p);
    b->magic = HEAP_MAGIC;
    b->size  = pages * PAGE_SIZE - sizeof(block_t) - CANARY_BYTES;
    b->req   = 0;
    b->free  = true;
    b->next  = NULL;
    b->prev  = NULL;
    if (!head) {
        head = b;
    } else {
        block_t *t = head; while (t->next) t = t->next;
        t->next = b; b->prev = t;
    }
    heap_pages_total += pages;
    g_stats.grow_count++;
    return b;
}

/* Split a free block so its payload capacity is exactly `want`; the tail
 * becomes a new free block (returned) that the caller buckets. */
static block_t *split(block_t *b, size_t want) {
    if (b->size < want + sizeof(block_t) + CANARY_BYTES + 32) return NULL;
    block_t *n = (block_t *)((u8 *)b + sizeof(block_t) + want + CANARY_BYTES);
    n->magic = HEAP_MAGIC;
    n->size  = b->size - want - sizeof(block_t) - CANARY_BYTES;
    n->req   = 0;
    n->free  = true;
    n->next  = b->next;
    n->prev  = b;
    if (b->next) b->next->prev = n;
    b->next = n;
    b->size = want;
    return n;
}

/* Grab a free block of at least `n` payload capacity.  Exact-class pop is
 * O(1); a miss walks up the buckets (short) then grows. */
static block_t *alloc_from_class(size_t n) {
    int ci = class_for(n);
    if (ci < NUM_CLASSES) {
        for (int i = ci; i < NUM_CLASSES; i++)
            if (buckets[i]) return bucket_pop(i);
        /* small request but no small free block: try the huge bucket too */
        for (block_t *b = buckets[HUGE_CLASS], *pr = NULL; b; ) {
            block_t *nx = bfree_next(b);
            if (b->size >= n) {
                if (pr) bfree_set_next(pr, nx); else buckets[HUGE_CLASS] = nx;
                return b;
            }
            pr = b; b = nx;
        }
    } else {
        /* huge request: find a free block big enough (scan the huge list) */
        for (block_t *b = buckets[HUGE_CLASS], *pr = NULL; b; ) {
            block_t *nx = bfree_next(b);
            if (b->size >= n) {
                if (pr) bfree_set_next(pr, nx); else buckets[HUGE_CLASS] = nx;
                return b;
            }
            pr = b; b = nx;
        }
    }
    return NULL;
}

void heap_init(void) {
    head = NULL;
    heap_pages_total = 0;
    g_heap_bad_frees = 0;
    g_heap_bad_canaries = 0;
    memset(&g_stats, 0, sizeof g_stats);
    for (int i = 0; i < NUM_BUCKETS; i++) buckets[i] = NULL;
    spin_init(&heap_lock);
    block_t *b = grow(MB(1));
    if (b) bucket_push(b, class_for(b->size));
    kprintf("heap: initial %lu KiB (size-class slab fast paths + "
            "canaries + double-free + stats)\n",
            heap_pages_total * PAGE_SIZE / 1024);
}

void *kmalloc(size_t n) {
    if (!n) return NULL;
    n = (n + 15) & ~15ULL;
    u64 fl = irq_save();
    spin_lock(&heap_lock);
    int ci = class_for(n);
    block_t *b = alloc_from_class(n);
    if (!b) b = grow(n);
    if (!b) {
        spin_unlock(&heap_lock);
        irq_restore(fl);
        return NULL;
    }
    block_t *rem = split(b, n);
    if (rem) bucket_push(rem, class_for(rem->size));
    b->free = false;
    b->req  = n;
    void *p = payload(b);
    *canary_of(b) = HEAP_CANARY;      /* plant the overflow canary        */
    g_stats.total_alloc_bytes += n;
    g_stats.alloc_count++;
    g_stats.cur_bytes += n;
    if (g_stats.cur_bytes > g_stats.peak_bytes) g_stats.peak_bytes = g_stats.cur_bytes;
    g_stats.per_class_active[ci]++;
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
    int ci = class_for(b->req);
    if (ci < NUM_BUCKETS && g_stats.per_class_active[ci])
        g_stats.per_class_active[ci]--;
    g_stats.total_free_bytes += b->req;
    g_stats.free_count++;
    g_stats.cur_bytes -= (b->req < g_stats.cur_bytes) ? b->req : g_stats.cur_bytes;

    /* coalesce with the next and previous blocks (address-ordered) */
    if (b->next && b->next->free) {
        block_t *nx = b->next;
        bucket_remove(nx);
        b->size += sizeof(block_t) + CANARY_BYTES + nx->size;
        b->next  = nx->next;
        if (b->next) b->next->prev = b;
    }
    if (b->prev && b->prev->free) {
        block_t *pv = b->prev;
        bucket_remove(pv);
        pv->size += sizeof(block_t) + CANARY_BYTES + b->size;
        pv->next  = b->next;
        if (b->next) b->next->prev = pv;
        b = pv;
    }
    b->free = true;
    b->req  = 0;
    bucket_push(b, class_for(b->size));
    spin_unlock(&heap_lock);
    irq_restore(fl);
}

/* Alignment > 16: kmalloc_aligned() overallocates, aligns the payload to a
 * power of two, and records the real (kmalloc) block in the 8 bytes just
 * before the aligned pointer so kfree_aligned() can release it. */
void *kmalloc_aligned(size_t n, size_t align) {
    if (align <= 16) return kmalloc(n);
    if ((align & (align - 1)) != 0) {          /* round up to a power of 2 */
        size_t want = align;
        align = 1;
        while (align < want) align <<= 1;
    }
    void *raw = kmalloc(n + align + 16);
    if (!raw) return NULL;
    uintptr_t a = (uintptr_t)raw;
    uintptr_t pa = (a + sizeof(void *) + align - 1) & ~(uintptr_t)(align - 1);
    void *ret = (void *)pa;
    ((void **)ret)[-1] = raw;
    return ret;
}

void kfree_aligned(void *p) {
    if (!p) return;
    void *raw = ((void **)p)[-1];
    kfree(raw);
}

const heap_stats_t *heap_stats(void) { return &g_stats; }
void heap_stats_snapshot(heap_stats_t *out) {
    u64 fl = irq_save();
    spin_lock(&heap_lock);
    *out = g_stats;
    spin_unlock(&heap_lock);
    irq_restore(fl);
}

/* Boot-time heap selftest: accounting, double-free / overflow detection,
 * the size-class O(1) path, alignment >16 and the stats.  Must leave the
 * heap in a clean state. */
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

    /* alignment >16 */
    void *al = kmalloc_aligned(100, 256);
    if (!al || ((uintptr_t)al & 255) != 0) {
        kprintf("  !! aligned alloc failed (got %p)\n", al);
        ok = false;
    } else {
        memset(al, 0x77, 100);
        kprintf("  aligned kmalloc_aligned(100,256) -> %p (256-aligned)\n", al);
        kfree_aligned(al);
    }
    void *al2 = kmalloc_aligned(1000, 4096);
    if (!al2 || ((uintptr_t)al2 & 4095) != 0) {
        kprintf("  !! 4096-aligned alloc failed (got %p)\n", al2);
        ok = false;
    } else {
        kfree_aligned(al2);
    }

    /* size-class O(1) fast path: churn many small allocations, all of which
     * must hit the bucket head (no growth in the steady state). */
    enum { N = 64 };
    void *objs[N];
    u64 grows0 = g_stats.grow_count;
    for (int i = 0; i < N; i++) objs[i] = kmalloc(48 + (i % 3) * 16); /* 48/64/80 */
    for (int i = 0; i < N; i++) if (!objs[i]) { kprintf("  !! churn alloc failed\n"); ok = false; }
    for (int i = 0; i < N; i++) memset(objs[i], i & 0xFF, 48);
    for (int i = 0; i < N; i++) kfree(objs[i]);
    /* steady-state re-alloc must NOT grow the heap (all served from buckets) */
    u64 grows1 = g_stats.grow_count;
    for (int i = 0; i < N; i++) objs[i] = kmalloc(48 + (i % 3) * 16);
    for (int i = 0; i < N; i++) kfree(objs[i]);
    if (g_stats.grow_count != grows1) {
        kprintf("  !! steady-state alloc grew the heap (%llu -> %llu)\n",
                (unsigned long long)grows1,
                (unsigned long long)g_stats.grow_count);
        ok = false;
    }
    (void)grows0;

    kfree(b);

    heap_stats_t st;
    heap_stats_snapshot(&st);
    kprintf("heap: selftest %s (bad-frees=%llu overflow=%llu; stats: "
            "alloc=%llu free=%llu cur=%lluB peak=%lluB grows=%llu)\n",
            ok ? "PASS" : "FAIL",
            (unsigned long long)g_heap_bad_frees,
            (unsigned long long)g_heap_bad_canaries,
            (unsigned long long)st.alloc_count,
            (unsigned long long)st.free_count,
            (unsigned long long)st.cur_bytes,
            (unsigned long long)st.peak_bytes,
            (unsigned long long)st.grow_count);
}
