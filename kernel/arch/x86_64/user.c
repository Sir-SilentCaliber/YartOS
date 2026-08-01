/* Yart OS - prepare a ring-3 task from an ELF file in the VFS.
 *
 * Pattern:
 *   1) parse ELF64 (must be ET_DYN / PIE so it can be loaded anywhere),
 *      pick a RANDOM load base (ASLR), reserve + map each PT_LOAD segment
 *      at base+offset honouring the segment's exec/write/read flags, and
 *      apply NX (no-execute) to every non-executable page.
 *   2) reserve a user stack at a RANDOM address (ASLR), with a guard page
 *      below it, NX-marked.
 *   3) return the entry/stack; the scheduler (sched_create_user) turns this
 *      into a real task with its own page tables (private PML4).
 *
 * ASLR: on every boot/exec the code lands at a different address and the
 * stack at a different address, so attackers cannot predict where anything
 * is.  NX: data/stack pages refuse to execute, killing the classic
 * stack-smashing exploit.
 */
#include <yart/user.h>
#include <yart/mm.h>
#include <yart/fs.h>
#include <yart/string.h>
#include <yart/console.h>
#include <yart/io.h>
#include <yart/cpu.h>      /* rdtsc seed for ASLR */
#include <yart/hal.h>      /* pit_ticks() for the ASLR seed */

#define ELF_MAGIC 0x464C457FU
#define EM_X86_64 62
#define ET_EXEC   2
#define ET_DYN    3
typedef struct PACKED {
    u32 magic; u8 cls,data,ver,osabi; u8 pad[8];
    u16 type, machine; u32 version;
    u64 entry, phoff, shoff;
    u32 flags;
    u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} ehdr_t;
typedef struct PACKED {
    u32 type, flags;
    u64 offset, vaddr, paddr, filesz, memsz, align;
} phdr_t;
#define PT_LOAD 1

/* ELF program-header permission bits */
#define PF_X 1
#define PF_W 2
#define PF_R 4

/* ---------- ASLR ---------- */
static u64 g_aslr_state;

static u64 aslr_rand(void) {
    /* xorshift64* - good enough entropy for address randomization */
    u64 x = g_aslr_state;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    g_aslr_state = x;
    return x * 0x2545F4914F6CDD1DULL;
}

/* Code lands in [USER_VBASE, USER_VBASE+256MiB), stack in
 * [0x60000000, USER_STACK_TOP).  Both page-aligned; gap >= 256 MiB. */
#define CODE_ASLR_SPAN   (256u * 1024 * 1024)
#define STACK_ASLR_SPAN  (128u * 1024 * 1024)

static u64 pick_code_base(void) {
    return USER_VBASE +
           (aslr_rand() % (CODE_ASLR_SPAN >> 12)) * PAGE_SIZE;
}
static u64 pick_stack_top(void) {
    return USER_STACK_TOP -
           (aslr_rand() % (STACK_ASLR_SPAN >> 12)) * PAGE_SIZE;
}

/* A malicious or corrupt ELF must not map pages over kernel memory: every
 * segment's virtual range is checked to stay inside the user VA region and
 * every file range is checked against the on-disk size.  Each segment is
 * *reserved* as a demand-paged region; file bytes are mapped + copied
 * eagerly, the memsz-filesz tail (BSS) faults in later.  Permissions come
 * from the ELF p_flags: writable -> RW, non-executable -> NX. */
static u64 load_user_elf(vnode_t *v, u64 bias) {
    if (!v || v->type != VN_FILE || v->size < sizeof(ehdr_t)) return 0;
    ehdr_t *eh = v->data;
    if (eh->magic != ELF_MAGIC || eh->cls != 2 || eh->machine != EM_X86_64) {
        kprintf("user: bad ELF header (magic/cls/machine)\n");
        return 0;
    }
    if (eh->type != ET_DYN) {
        kprintf("user: ELF must be PIE (ET_DYN) for ASLR, got type %u\n",
                eh->type);
        return 0;
    }
    if (eh->phentsize < sizeof(phdr_t) || eh->phnum == 0 || eh->phnum > 64) {
        kprintf("user: ELF bad program headers\n");
        return 0;
    }
    if (eh->phoff > v->size ||
        eh->phoff + (u64)eh->phnum * sizeof(phdr_t) > v->size) {
        kprintf("user: ELF headers out of file bounds\n");
        return 0;
    }
    phdr_t *ph = (phdr_t *)((u8 *)v->data + eh->phoff);

    /* Pass 1: validate every PT_LOAD and compute one combined VA span so we
     * reserve a single region (the segments share page boundaries, and the
     * region model forbids overlapping reservations).  BSS tails fault in
     * with the safe default RW+NX, which matches how compilers lay out
     * zero-filled data. */
    u64 span_first = ~0ULL, span_last = 0;
    for (int i = 0; i < eh->phnum; i++) {
        if (ph[i].type != PT_LOAD) continue;
        u64 va  = bias + ph[i].vaddr;
        u64 mem = ph[i].memsz, file = ph[i].filesz;
        if (va + mem < va) {                       /* overflow              */
            kprintf("user: ELF segment overflow\n");
            return 0;
        }
        if (va < USER_VBASE || va + mem > USER_STACK_TOP) {
            kprintf("user: ELF segment outside user region (va=0x%lx)\n", va);
            return 0;
        }
        if (ph[i].offset > v->size ||
            ph[i].offset + file > v->size) {       /* reads out of the file */
            kprintf("user: ELF segment reads out of file\n");
            return 0;
        }
        u64 f = PAGE_ALIGN_DOWN(va);
        u64 l = PAGE_ALIGN_UP(va + mem);
        if (f < span_first) span_first = f;
        if (l > span_last)  span_last  = l;
    }
    if (span_first == ~0ULL || span_last <= span_first) return 0;
    if (vmm_user_reserve(span_first, (span_last - span_first) / PAGE_SIZE,
                         PTE_RW | PTE_US | PTE_NX, VMM_USER_LAZY) != 0) {
        kprintf("user: ELF span reserve failed (va=0x%lx)\n", span_first);
        return 0;
    }

    /* Pass 2: map each page of the span ONCE (pages can be shared between
     * segments - the boundary page holds the tail of the text segment and
     * the head of the data segment).  For every page:
     *   - allocate + zero it,
     *   - copy in the file bytes of every segment that intersects it,
     *   - set the UNION of permissions: executable if any covering segment
     *     is executable, writable if any is writable, NX otherwise. */
    for (u64 a = span_first; a < span_last; a += PAGE_SIZE) {
        paddr_t p = pmm_alloc_page();              /* zeroed by the PMM    */
        vmm_map(a, p, PTE_RW | PTE_US);            /* writable during copy */
        bool want_exec = false, want_write = false;
        for (int i = 0; i < eh->phnum; i++) {
            if (ph[i].type != PT_LOAD) continue;
            u64 sva = bias + ph[i].vaddr;
            u64 sfile = ph[i].filesz;
            if (a + PAGE_SIZE <= sva || a >= sva + sfile) continue;
            /* copy the intersection of [a, a+4096) and [sva, sva+sfile) */
            u64 src_off = (a > sva ? a : sva) - sva;
            u64 dst = (a > sva ? a : sva);
            u64 n = (a + PAGE_SIZE < sva + sfile ? a + PAGE_SIZE : sva + sfile) - dst;
            stac();   /* writing into user pages while SMAP is on */
            memcpy((void *)dst, (u8 *)v->data + ph[i].offset + src_off, n);
            clac();
            if (ph[i].flags & PF_X) want_exec = true;
            if (ph[i].flags & PF_W) want_write = true;
        }
        u64 flags = PTE_US;
        if (want_write) flags |= PTE_RW;
        if (!want_exec) flags |= PTE_NX;
        vmm_set_flags(a, flags);
    }
    return bias + eh->entry;                 /* PIE entry is an offset      */
}

/* User stack: a lazy region at a random address with one guard page below
 * (never mapped), NX-marked (data must not run), so a stack overflow
 * SIGSEGVs instead of silently clobbering memory. */
static u64 alloc_user_stack(u64 top) {
    u64 base = top - USER_STACK_PAGES * PAGE_SIZE;
    if (vmm_user_reserve(base, USER_STACK_PAGES,
                         PTE_RW | PTE_NX, VMM_USER_LAZY) != 0) {
        kprintf("user: stack region reserve failed\n");
        return 0;
    }
    /* SysV function-call convention: gcc compiles _start like any function
     * and expects rsp%16 == 8 at entry (as if a return address were on the
     * stack), so its movaps/movdqa stay 16-byte aligned.  With SSE enabled
     * this alignment is mandatory or every stack spill misaligns -> #GP. */
    return top - 8;
}

bool user_prepare_elf(vnode_t *v, u64 *entry_out, u64 *rsp_out) {
    /* seed the ASLR generator with hardware + time entropy */
    u64 tsc;
    __asm__ volatile ("rdtsc" : "=A"(tsc) :: "memory");
    g_aslr_state = tsc ^ ((u64)pit_ticks() << 32) ^ (u64)(u64)v;

    u64 bias = pick_code_base();
    u64 top  = pick_stack_top();
    u64 entry = load_user_elf(v, bias);
    if (!entry) {
        kprintf("user: load failed\n");
        return false;
    }
    u64 user_rsp = alloc_user_stack(top);
    if (!user_rsp) return false;
    if (entry_out) *entry_out = entry;
    if (rsp_out)   *rsp_out = user_rsp;
    kprintf("user: prepared ring-3 (ASLR) entry=%p stack=%p\n",
            (void *)entry, (void *)user_rsp);
    return true;
}
