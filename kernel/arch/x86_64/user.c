/* Yart OS - prepare a ring-3 task from an ELF file in the VFS.
 *
 * Both boot (user_prepare_elf) and exec (user_exec) build a COMPLETE
 * address space in a fresh private PML4:
 *   1) parse ELF64 (must be ET_DYN / PIE so it can be loaded anywhere),
 *      pick a RANDOM load base (ASLR), reserve + map each PT_LOAD segment
 *      honouring the segment's exec/write/read flags, and apply NX to
 *      every non-executable page.
 *   2) reserve a user stack at a RANDOM address (ASLR), NX-marked.
 *   3) write a SysV process image on the stack: argc, argv[], envp[],
 *      NULL terminators and the strings - so a standard _start (see
 *      userland/start.c) can pick argc/argv/envp off the stack exactly
 *      like a real OS.
 *
 * ASLR: on every boot/exec the code lands at a different address and the
 * stack at a different address.  NX: data/stack pages refuse to execute.
 */
#include <yart/user.h>
#include <yart/mm.h>
#include <yart/fs.h>
#include <yart/string.h>
#include <yart/console.h>
#include <yart/io.h>
#include <yart/cpu.h>      /* rdtsc seed for ASLR */
#include <yart/hal.h>      /* pit_ticks() for the ASLR seed */
#include <yart/sched.h>    /* sched_current() for exec */

#define ELF_MAGIC 0x464C457FU
#define EM_X86_64 62
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
#define SHT_RELA 4          /* .rela.dyn section type (NOT DT_RELA=7!) */
#define R_X86_64_RELATIVE 8

/* ELF section header (for finding .rela.dyn without section names).
 * NOTE: sh_link/sh_info are u32 each - declaring them u64 makes the
 * struct 72 bytes instead of the real 64 and the shentsize guard below
 * rejects every file (64 < 72). */
typedef struct PACKED {
    u32 name, type;
    u64 flags, addr, offset, size;
    u32 link, info;
    u64 addralign, entsize;
} shdr_t;

/* Apply R_X86_64_RELATIVE relocations: for a PIE linked with
 * -z combreloc, every absolute pointer in .data.rel.ro (e.g.
 * char *argv[] = {...}) is stored as a bias-relative addend and needs
 * the load bias added.  Without this, an exec'd binary's first argv
 * access reads an unrelocated ~0xf018 and crashes.  The relocation
 * section (.rela.dyn) is NON-alloc, so we read it straight from the
 * file buffer (v->data) - no mapping needed. */
static void apply_relocations(vnode_t *v, const ehdr_t *eh, u64 bias) {
    if (eh->shoff == 0 || eh->shnum == 0 || eh->shentsize < sizeof(shdr_t))
        return;
    if (eh->shoff > v->size) return;
    u64 n = (u64)eh->shnum;
    if (eh->shoff + n * eh->shentsize > v->size) return;
    for (u64 i = 0; i < n; i++) {
        shdr_t *sh = (shdr_t *)((u8 *)v->data + eh->shoff + i * eh->shentsize);
        if (sh->type != SHT_RELA) continue;      /* SHT_RELA = 4 */
        if (sh->offset > v->size ||
            sh->offset + sh->size > v->size) continue;
        u8 *base = (u8 *)v->data + sh->offset;
        for (u64 off = 0; off + 24 <= sh->size; off += 24) {
            u64 r_offset = *(u64 *)(base + off);        /* +0 */
            u64 r_info   = *(u64 *)(base + off + 8);    /* +8 */
            u64 r_addend = *(u64 *)(base + off + 16);   /* +16 */
            if ((r_info & 0xFFFFFFFFu) != R_X86_64_RELATIVE) continue;
            /* For -z combreloc PIE, the addend is the value to place
             * (bias-relative, e.g. 0xf018).  The final value = bias + addend. */
            u64 *slot = (u64 *)(bias + r_offset);
            stac();
            *slot = bias + r_addend;
            clac();
        }
    }
}

/* vdso-style sigreturn trampoline, mapped RX into every user process at a
 * fixed VA.  A signal handler returns into it; it syscalls SYS_SIGRETURN
 * so the kernel can restore the interrupted frame.  Must match
 * SIGRETURN_TRAMP_VA in sched.c and SYS_SIGRETURN_NR. */
#define SIGRETURN_TRAMP_VA 0x6F000000UL
#define SIGRETURN_TRAMP_NR 51

static void user_map_trampoline(u64 *pml4, user_region_t *rs, int *nrs) {
    /* mov rdi, [rsp-16] ; at trampoline entry rsp == ret_va+8, and the
     * frame-pointer slot lives at ret_va-8 == rsp-16 (see sched.c) */
    u8 code[32];
    code[0] = 0x48; code[1] = 0x8B; code[2] = 0x7C; code[3] = 0x24; /* mov rdi,[rsp+disp8] */
    code[4] = 0xF0;                                                 /* -16 */
    code[5]  = 0x48; code[6]  = 0xC7; code[7]  = 0xC0;              /* mov rax, imm32 */
    code[8]  = SIGRETURN_TRAMP_NR; code[9] = 0; code[10] = 0; code[11] = 0;
    code[12] = 0x0F; code[13] = 0x05;                               /* syscall */
    code[14] = 0xEB; code[15] = 0xFE;                               /* jmp $  */
    if (vmm_reserve_in(pml4, rs, nrs, SIGRETURN_TRAMP_VA, 1,
                       PTE_US, 0) != 0)            /* RX (no NX, no RW) */
        return;
    paddr_t p = pmm_alloc_page();
    if (!p) return;
    memcpy(phys_to_virt(p), code, 16);
    vmm_map_in(pml4, SIGRETURN_TRAMP_VA, p, PTE_PRESENT | PTE_US);
}

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

static void seed_aslr(u64 extra) {
    u64 tsc;
    __asm__ volatile ("rdtsc" : "=A"(tsc) :: "memory");
    g_aslr_state = tsc ^ ((u64)pit_ticks() << 32) ^ extra;
}

/* A malicious or corrupt ELF must not map pages over kernel memory: every
 * segment's virtual range is checked to stay inside the user VA region and
 * every file range is checked against the on-disk size.  Each segment is
 * *reserved* as a demand-paged region; file bytes are mapped + copied
 * eagerly, the memsz-filesz tail (BSS) faults in later.  Permissions come
 * from the ELF p_flags: writable -> RW, non-executable -> NX.  Everything
 * is mapped into `pml4` (a fresh, not-yet-active table) via vmm_map_in. */
static u64 load_user_elf_into(vnode_t *v, u64 bias, u64 *pml4,
                              user_region_t *rs, int *nrs) {
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
    if (vmm_reserve_in(pml4, rs, nrs, span_first,
                       (span_last - span_first) / PAGE_SIZE,
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
        vmm_map_in(pml4, a, p, PTE_RW | PTE_US);   /* writable during copy */
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
        vmm_set_flags_in(pml4, a, flags);
    }
    return bias + eh->entry;                 /* PIE entry is an offset      */
}

/* Reserve the user stack (lazy region, NX) and write the SysV process
 * image: strings, then envp[]/argv[] pointer arrays, then argc.  Layout
 * (all inside the top pages of the stack):
 *
 *     higher address
 *       [argc:8]  [argv[0]..argv[argc-1]:8*argc]  [0:8]  [envp[]..]  [0:8]
 *       [string area ...]
 *     lower address
 *
 * _start reads argc at (%rsp), argv at 8(%rsp), envp at
 * 16(%rsp)+argc*8.  Returns the user RSP (pointing AT argc). */
static u64 build_user_stack_into(u64 *pml4, user_region_t *rs, int *nrs,
                                 u64 top, char *const kargv[], int argc,
                                 char *const kenvp[], int envc) {
    u64 base = top - USER_STACK_PAGES * PAGE_SIZE;
    /* EAGER reserve: the process image (argc/argv/envp + strings) is
     * written into the stack right now, so the pages must exist. */
    if (vmm_reserve_in(pml4, rs, nrs, base, USER_STACK_PAGES,
                       PTE_RW | PTE_NX, 0) != 0) {
        kprintf("user: stack region reserve failed\n");
        return 0;
    }
    /* total string bytes */
    u64 strbytes = 0;
    for (int i = 0; i < argc; i++) strbytes += strlen(kargv[i]) + 1;
    for (int i = 0; i < envc; i++) strbytes += strlen(kenvp[i]) + 1;
    /* argv strings byte count (envp strings follow them in memory) */
    u64 argv_bytes = 0;
    for (int i = 0; i < argc; i++) argv_bytes += strlen(kargv[i]) + 1;

    u64 strings = top - 8 - strbytes;        /* grows DOWN from top-8 */
    u64 envp_va = strings - (u64)(envc + 1) * 8;
    u64 argv_va = envp_va - (u64)(argc + 1) * 8;
    u64 argc_va = argv_va - 8;

    stac();
    /* strings */
    char *p = (char *)strings;
    for (int i = 0; i < argc; i++) {
        size_t n = strlen(kargv[i]) + 1;
        memcpy(p, kargv[i], n);
        p += n;
    }
    for (int i = 0; i < envc; i++) {
        size_t n = strlen(kenvp[i]) + 1;
        memcpy(p, kenvp[i], n);
        p += n;
    }
    /* envp pointers: walk AFTER the argv strings (a naive walk from the
     * top makes envp[0] point at argv[0]'s string - exec'd programs saw
     * env = "/bin/hello") */
    u64 *ep = (u64 *)envp_va;
    p = (char *)strings + argv_bytes;
    for (int i = 0; i < envc; i++) {
        ep[i] = (u64)p;
        p += strlen(kenvp[i]) + 1;
    }
    ep[envc] = 0;
    /* argv pointers */
    u64 *ap = (u64 *)argv_va;
    p = (char *)strings;
    for (int i = 0; i < argc; i++) {
        ap[i] = (u64)p;
        p += strlen(kargv[i]) + 1;
    }
    ap[argc] = 0;
    /* argc */
    *(u64 *)argc_va = (u64)argc;
    clac();
    return argc_va;                          /* _start sees argc at %rsp */
}

/* Prepare a fresh address space for the boot task (/bin/init, the wm).
 * Returns entry, rsp, the private PML4 and the region table. */
bool user_prepare_elf(vnode_t *v, u64 *entry_out, u64 *rsp_out,
                      u64 **pml4_out, user_region_t *regions_out,
                      int *nregions_out) {
    u64 *pml4 = vmm_new_pml4();
    if (!pml4) return false;
    user_region_t rs[MAX_USER_REGIONS];
    int nrs = 0;

    /* The loader and stack builder write file bytes / the process image
     * into the TARGET address space: CR3 must already point at it (kernel
     * half is shared, so this is safe even before the scheduler runs). */
    vmm_switch_pml4(pml4);

    seed_aslr((u64)(u64)v);
    u64 bias = pick_code_base();
    u64 top  = pick_stack_top();
    u64 entry = load_user_elf_into(v, bias, pml4, rs, &nrs);
    if (!entry) goto fail;
    apply_relocations(v, (const ehdr_t *)v->data, bias);
    user_map_trampoline(pml4, rs, &nrs);       /* sigreturn helper page */

    /* minimal process image: argc=1, argv=["init"] */
    char *argv0 = (char *)"init";
    char *kargv[2] = { argv0, NULL };
    char *kenvp[1] = { NULL };
    u64 user_rsp = build_user_stack_into(pml4, rs, &nrs, top,
                                         kargv, 1, kenvp, 0);
    if (!user_rsp) goto fail;

    *entry_out = entry;
    *rsp_out   = user_rsp;
    *pml4_out  = pml4;
    if (regions_out && nregions_out) {
        memcpy(regions_out, rs, sizeof rs);
        *nregions_out = nrs;
    }
    kprintf("user: prepared ring-3 (ASLR) entry=%p stack=%p pml4=%p\n",
            (void *)entry, (void *)user_rsp, pml4);
    return true;
fail:
    vmm_switch_pml4(vmm_kernel_pml4());
    vmm_free_pml4(pml4);
    return false;
}

/* exec(2): replace the CURRENT task's address space with a freshly loaded
 * image and rewrite its interrupt frame so it returns into the new program
 * with a clean stack image.  `kargv`/`kenvp` are already copied into
 * kernel memory by the syscall layer (the old user space is about to be
 * destroyed, so nothing may point into it).  On success the frame is
 * rewritten and 0 is returned; the task never sees the old code again. */
bool user_exec(vnode_t *v, char *const kargv[], int argc,
               char *const kenvp[], int envc, cpu_regs_t *frame) {
    task_t *t = sched_current();
    if (!t || !t->is_user) return false;

    u64 *new_pml4 = vmm_new_pml4();
    if (!new_pml4) return false;
    user_region_t rs[MAX_USER_REGIONS];
    int nrs = 0;

    /* CR3 must point at the new tables BEFORE any byte is copied into the
     * new image (the old address space is about to be destroyed anyway). */
    u64 *old_pml4 = t->pml4;
    vmm_switch_pml4(new_pml4);

    seed_aslr((u64)(u64)v ^ ((u64)t->pid << 32));
    u64 bias = pick_code_base();
    u64 top  = pick_stack_top();
    u64 entry = load_user_elf_into(v, bias, new_pml4, rs, &nrs);
    if (!entry) goto fail;
    apply_relocations(v, (const ehdr_t *)v->data, bias);
    user_map_trampoline(new_pml4, rs, &nrs);   /* sigreturn helper page */

    u64 user_rsp = build_user_stack_into(new_pml4, rs, &nrs, top,
                                         kargv, argc, kenvp, envc);
    if (!user_rsp) goto fail;

    /* drop the old address space */
    if (old_pml4 && old_pml4 != vmm_kernel_pml4())
        vmm_free_pml4(old_pml4);

    t->pml4 = new_pml4;
    memcpy(t->regions, rs, sizeof rs);
    t->region_count = nrs;
    /* fresh dynamic-memory arena + memory accounting for the new image */
    t->mmap_next = USER_MMAP_BASE;
    t->brk_base  = USER_MMAP_BASE;
    t->brk       = USER_MMAP_BASE;
    t->mem_pages = 0;
    for (int i = 0; i < nrs; i++) t->mem_pages += rs[i].npages;
    fpu_capture_clean(t->fpu_area);          /* fresh FPU state, like a real exec */
    /* exec resets signal state: handlers, pending and blocked signals all
     * die with the old image (POSIX: caught signals reset to default) */
    memset(t->sig_handlers, 0, sizeof t->sig_handlers);
    t->sig_pending = 0;
    t->sig_blocked = 0;

    /* Rewrite the frame: the syscall returns straight into the new
     * program (rax = 0 = exec success). */
    frame->rip = entry;
    frame->rsp = user_rsp;
    frame->rax = 0;
    frame->rcx = 0;
    frame->r11 = 0;

    kprintf("exec: pid %u '%s' -> %s entry=%p rsp=%p argc=%d env=%d\n",
            t->pid, t->name, v->name, (void *)entry, (void *)user_rsp,
            argc, envc);
    return true;
fail:
    /* restore the OLD tables and keep running the old image */
    vmm_switch_pml4(old_pml4 ? old_pml4 : vmm_kernel_pml4());
    vmm_free_pml4(new_pml4);
    return false;
}
