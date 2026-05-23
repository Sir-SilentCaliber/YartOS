/* Yart OS - simple linked-list kernel heap on top of PMM/HHDM */
#include <yart/mm.h>
#include <yart/string.h>
#include <yart/console.h>

typedef struct block {
    size_t size;          /* payload size, not including header */
    bool   free;
    struct block *next;
    struct block *prev;
} block_t;

#define HEAP_MAGIC_GAP 16

static block_t *head;
static size_t   heap_pages_total;
static u64      heap_base_v;

static block_t *grow(size_t need_payload) {
    size_t need = sizeof(block_t) + need_payload;
    size_t pages = PAGE_ALIGN_UP(need) / PAGE_SIZE;
    if (pages < 8) pages = 8;
    paddr_t p = pmm_alloc_pages(pages);
    block_t *b = (block_t *)phys_to_virt(p);
    b->size = pages * PAGE_SIZE - sizeof(block_t);
    b->free = true;
    b->next = NULL;
    b->prev = NULL;
    if (!head) {
        head = b;
        heap_base_v = (u64)b;
    } else {
        /* append at tail */
        block_t *t = head; while (t->next) t = t->next;
        t->next = b; b->prev = t;
    }
    heap_pages_total += pages;
    return b;
}

void heap_init(void) {
    head = NULL;
    heap_pages_total = 0;
    grow(MB(1));
    kprintf("heap: initial %lu KiB\n", heap_pages_total * PAGE_SIZE / 1024);
}

static void split(block_t *b, size_t want) {
    if (b->size < want + sizeof(block_t) + 32) return;
    block_t *n = (block_t *)((u8 *)b + sizeof(block_t) + want);
    n->size = b->size - want - sizeof(block_t);
    n->free = true;
    n->next = b->next;
    n->prev = b;
    if (b->next) b->next->prev = n;
    b->next = n;
    b->size = want;
}

void *kmalloc(size_t n) {
    if (!n) return NULL;
    n = (n + 15) & ~15ULL;
    for (block_t *b = head; b; b = b->next) {
        if (b->free && b->size >= n) {
            split(b, n);
            b->free = false;
            return (void *)((u8 *)b + sizeof(block_t));
        }
    }
    block_t *b = grow(n);
    split(b, n);
    b->free = false;
    return (void *)((u8 *)b + sizeof(block_t));
}

void *kzalloc(size_t n) {
    void *p = kmalloc(n);
    if (p) memset(p, 0, n);
    return p;
}

void kfree(void *p) {
    if (!p) return;
    block_t *b = (block_t *)((u8 *)p - sizeof(block_t));
    b->free = true;
    /* coalesce */
    if (b->next && b->next->free) {
        b->size += sizeof(block_t) + b->next->size;
        b->next = b->next->next;
        if (b->next) b->next->prev = b;
    }
    if (b->prev && b->prev->free) {
        b->prev->size += sizeof(block_t) + b->size;
        b->prev->next = b->next;
        if (b->next) b->next->prev = b->prev;
    }
}
