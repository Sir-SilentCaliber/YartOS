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
#define ET_EXEC   2   /* a foreign (Linux) static binary: fixed VA, not PIE */
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
#define PT_DYNAMIC 2
#define PT_INTERP 3
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

static u64 rdtsc_now(void) {
    u64 lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return (hi << 32) | lo;
}

/* Find a PT_INTERP program header's path string (the dynamic-linker path,
 * e.g. "/lib/ld-musl-x86_64.so.1").  Returns 0 on success, -1 if none. */
static int find_interp_path(vnode_t *v, char *out, int cap) {
    if (!v || v->type != VN_FILE || v->size < sizeof(ehdr_t)) return -1;
    ehdr_t *eh = (ehdr_t *)v->data;
    if (eh->magic != ELF_MAGIC || eh->phoff + (u64)eh->phnum * sizeof(phdr_t) > v->size)
        return -1;
    phdr_t *ph = (phdr_t *)((u8 *)v->data + eh->phoff);
    for (int i = 0; i < eh->phnum; i++) {
        if (ph[i].type != PT_INTERP) continue;
        u64 off = ph[i].offset, sz = ph[i].filesz;
        if (off + sz > v->size || sz == 0 || sz > (u64)cap - 1) return -1;
        memcpy(out, (u8 *)v->data + off, sz);
        out[sz] = 0;
        return 0;
    }
    return -1;
}

/* Is this vnode a foreign (Linux) binary?  YartOS builds everything as PIE
 * (ET_DYN) with NO interpreter.  A Linux binary is EITHER a static ET_EXEC,
 * OR an ET_DYN (PIE) with a PT_INTERP (dynamically-linked) — the PT_INTERP
 * presence is the marker that distinguishes a Linux PIE from a YartOS one. */
static bool elf_is_linux(vnode_t *v) {
    if (!v || v->type != VN_FILE || v->size < sizeof(ehdr_t)) return false;
    const ehdr_t *eh = (const ehdr_t *)v->data;
    if (eh->magic != ELF_MAGIC || eh->cls != 2 || eh->machine != EM_X86_64)
        return false;
    if (eh->type == ET_EXEC) return true;
    if (eh->type != ET_DYN) return false;
    /* ET_DYN: Linux PIE iff it names an interpreter */
    char tmp[VFS_MAX_PATH];
    return find_interp_path(v, tmp, sizeof tmp) == 0;
}

/* A malicious or corrupt ELF must not map pages over kernel memory: every
 * segment's virtual range is checked to stay inside the user VA region and
 * every file range is checked against the on-disk size.  Each segment is
 * *reserved* as a demand-paged region; file bytes are mapped + copied
 * eagerly, the memsz-filesz tail (BSS) faults in later.  Permissions come
 * from the ELF p_flags: writable -> RW, non-executable -> NX.  Everything
 * is mapped into `pml4` (a fresh, not-yet-active table) via vmm_map_in. */
static u64 load_user_elf_into(vnode_t *v, u64 bias, u64 *pml4,
                              user_region_t *rs, int *nrs, u64 *span_end_out) {
    if (span_end_out) *span_end_out = 0;
    if (!v || v->type != VN_FILE || v->size < sizeof(ehdr_t)) return 0;
    ehdr_t *eh = v->data;
    if (eh->magic != ELF_MAGIC || eh->cls != 2 || eh->machine != EM_X86_64) {
        kprintf("user: bad ELF header (magic/cls/machine)\n");
        return 0;
    }
    if (eh->type != ET_DYN && eh->type != ET_EXEC) {
        kprintf("user: ELF type %u unsupported (want PIE, or Linux ET_EXEC)\n",
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
     * zero-filled data.  A Linux ET_EXEC binary loads at its FIXED low VA
     * (classically 0x400000); YartOS PIE binaries land in [USER_VBASE, ...).
     * The low 1 GB below USER_VBASE is otherwise unused by YartOS, so it is
     * safe to place foreign binaries there. */
    u64 min_va = (eh->type == ET_EXEC) ? 0x1000 : USER_VBASE;
    u64 span_first = ~0ULL, span_last = 0;
    for (int i = 0; i < eh->phnum; i++) {
        if (ph[i].type != PT_LOAD) continue;
        u64 va  = bias + ph[i].vaddr;
        u64 mem = ph[i].memsz, file = ph[i].filesz;
        if (va + mem < va) {                       /* overflow              */
            kprintf("user: ELF segment overflow\n");
            return 0;
        }
        if (va < min_va || va + mem > USER_STACK_TOP) {
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
    if (span_end_out) *span_end_out = span_last;

    /* Pass 2: map each page of the span ONCE (pages can be shared between
     * segments - the boundary page holds the tail of the text segment and
     * the head of the data segment).  For every page:
     *   - allocate + zero it,
     *   - copy in the file bytes of every segment that intersects it,
     *   - set the UNION of permissions: executable if any covering segment
     *     is executable, writable if any is writable, NX otherwise.
     *
     * BSS-TAIL FIX: permissions come from the segment's MEMORY range
     * (memsz), not just its file bytes.  A page fully past filesz (e.g.
     * the first pure .bss page after a huge .data blob) intersects no
     * FILE bytes, so the old code left want_write=false and mapped it
     * read-only - the app SIGSEGV'd on its first write to .bss (settings
     * crashed in cursors_init writing G_count).  Pages covered by no
     * segment memory range at all default to RW|NX (safe data). */
    for (u64 a = span_first; a < span_last; a += PAGE_SIZE) {
        paddr_t p = pmm_alloc_page();              /* zeroed by the PMM    */
        vmm_map_in(pml4, a, p, PTE_RW | PTE_US);   /* writable during copy */
        bool want_exec = false, want_write = false;
        bool covered = false;
        for (int i = 0; i < eh->phnum; i++) {
            if (ph[i].type != PT_LOAD) continue;
            u64 sva = bias + ph[i].vaddr;
            u64 smem = ph[i].memsz;
            if (a + PAGE_SIZE <= sva || a >= sva + smem) continue;
            covered = true;                        /* memory-range coverage */
            if (ph[i].flags & PF_X) want_exec = true;
            if (ph[i].flags & PF_W) want_write = true;
            /* copy the file-byte intersection, if any */
            u64 sfile = ph[i].filesz;
            if (a + PAGE_SIZE <= sva || a >= sva + sfile) continue;
            u64 src_off = (a > sva ? a : sva) - sva;
            u64 dst = (a > sva ? a : sva);
            u64 n = (a + PAGE_SIZE < sva + sfile ? a + PAGE_SIZE : sva + sfile) - dst;
            stac();   /* writing into user pages while SMAP is on */
            memcpy((void *)dst, (u8 *)v->data + ph[i].offset + src_off, n);
            clac();
        }
        if (!covered) want_write = true;           /* gap page: RW data  */
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
/* Linux auxiliary vector entries (x86_64 ABI).  A Linux static binary's
 * _start (musl/glibc) reads the auxv right after envp to find AT_PHDR,
 * AT_ENTRY, AT_PAGESZ, AT_RANDOM, etc.  Without it, real binaries fault
 * immediately; our own YartOS binaries don't use it (auxv==NULL). */
#define AT_NULL     0
#define AT_PHDR     3
#define AT_PHENT    4
#define AT_PHNUM    5
#define AT_PAGESZ   6
#define AT_BASE     7
#define AT_ENTRY    9
#define AT_CLKTCK   17
#define AT_RANDOM   25
#define AT_SYSINFO_EHDR 33

static u64 build_user_stack_into(u64 *pml4, user_region_t *rs, int *nrs,
                                 u64 top, char *const kargv[], int argc,
                                 char *const kenvp[], int envc,
                                 const u64 *auxv, int auxc) {
    u64 base = top - USER_STACK_PAGES * PAGE_SIZE;
    /* EAGER reserve: the process image (argc/argv/envp + strings) is
     * written into the stack right now, so the pages must exist. */
    if (vmm_reserve_in(pml4, rs, nrs, base, USER_STACK_PAGES,
                       PTE_RW | PTE_NX, 0) != 0) {
        kprintf("user: stack region reserve failed\n");
        return 0;
    }
    /* total string bytes (plus a 16-byte AT_RANDOM blob when auxv present) */
    u64 strbytes = (auxv ? 16 : 0);
    for (int i = 0; i < argc; i++) strbytes += strlen(kargv[i]) + 1;
    for (int i = 0; i < envc; i++) strbytes += strlen(kenvp[i]) + 1;
    /* argv strings byte count (envp strings follow them in memory) */
    u64 argv_bytes = 0;
    for (int i = 0; i < argc; i++) argv_bytes += strlen(kargv[i]) + 1;

    /* SysV layout (low -> high address): argc, argv[], NULL, envp[], NULL,
     * auxv pairs, AT_NULL, then the string area at the very top.  The auxv
     * MUST sit ABOVE envp so a standard _start / dynamic linker that walks
     * UP from %rsp (argc) reaches it — the old code placed it BELOW argc,
     * which made any auxv reader walk off the top of the stack. */
    u64 strings = top - 8 - strbytes;        /* grows DOWN from top-8 */
    u64 auxv_va = auxv ? (strings - (u64)(auxc + 1) * 16) : strings;
    u64 envp_va = auxv_va - (u64)(envc + 1) * 8;
    u64 argv_va = envp_va - (u64)(argc + 1) * 8;
    u64 argc_va = argv_va - 8;

    stac();
    /* strings */
    char *p = (char *)strings;
    /* 16-byte AT_RANDOM source (weak PRNG from TSC; good enough for a canary) */
    if (auxv) {
        u64 x = rdtsc_now() ^ 0x9E3779B97F4A7C15ULL;
        for (int i = 0; i < 16; i++) { p[i] = (char)(x & 0xFF); x = x * 6364136223846793005ULL + 1442695040888963407ULL; }
        p += 16;
    }
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
    p = (char *)strings + (auxv ? 16 : 0) + argv_bytes;
    for (int i = 0; i < envc; i++) {
        ep[i] = (u64)p;
        p += strlen(kenvp[i]) + 1;
    }
    ep[envc] = 0;
    /* argv pointers */
    u64 *ap = (u64 *)argv_va;
    p = (char *)strings + (auxv ? 16 : 0);
    for (int i = 0; i < argc; i++) {
        ap[i] = (u64)p;
        p += strlen(kargv[i]) + 1;
    }
    ap[argc] = 0;
    /* argc */
    *(u64 *)argc_va = (u64)argc;
    /* auxv (rewrite AT_RANDOM to point at the blob on the stack) */
    if (auxv) {
        u64 *av = (u64 *)auxv_va;
        for (int i = 0; i < auxc; i++) {
            av[2 * i]     = auxv[2 * i];
            av[2 * i + 1] = (auxv[2 * i] == AT_RANDOM) ? (u64)strings
                                                       : auxv[2 * i + 1];
        }
        av[2 * auxc] = AT_NULL;
        av[2 * auxc + 1] = 0;
    }
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
    u64 entry = load_user_elf_into(v, bias, pml4, rs, &nrs, NULL);
    if (!entry) goto fail;
    apply_relocations(v, (const ehdr_t *)v->data, bias);
    user_map_trampoline(pml4, rs, &nrs);       /* sigreturn helper page */

    /* minimal process image: argc=1, argv=["init"] */
    char *argv0 = (char *)"init";
    char *kargv[2] = { argv0, NULL };
    char *kenvp[1] = { NULL };
    u64 user_rsp = build_user_stack_into(pml4, rs, &nrs, top,
                                         kargv, 1, kenvp, 0, NULL, 0);
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
     * new image (the old address space is about to be destroyed anyway).
     * Record the NEW tables on the task BEFORE the switch, so there is no
     * window where t->pml4 (what the scheduler and other CPUs read) is
     * stale while CR3 already points at the new tables. */
    u64 *old_pml4 = t->pml4;
    t->pml4 = new_pml4;
    vmm_switch_pml4(new_pml4);

    seed_aslr((u64)(u64)v ^ ((u64)t->pid << 32));
    bool is_linux = elf_is_linux(v);
    /* bias: 0 for a Linux STATIC ET_EXEC (fixed VA); otherwise a random PIE
     * base (YartOS binaries AND Linux dynamically-linked PIEs load anywhere) */
    u64 bias = ((const ehdr_t *)v->data)->type == ET_EXEC ? 0 : pick_code_base();
    u64 top  = pick_stack_top();
    u64 main_end = 0;
    u64 entry = load_user_elf_into(v, bias, new_pml4, rs, &nrs, &main_end);
    if (!entry) goto fail;
    apply_relocations(v, (const ehdr_t *)v->data, bias);

    /* ---- DYNAMIC LINKING: PT_INTERP ----
     * A dynamically-linked ELF names an interpreter (the dynamic linker,
     * e.g. /lib/ld-musl-x86_64.so.1).  Load the interpreter's PT_LOAD
     * segments (it is a PIE .so) at a base placed AFTER the main image, and
     * hand control to the interpreter's entry.  The interpreter finds the
     * main program through the auxv (AT_PHDR/AT_ENTRY) and itself through
     * AT_BASE, relocates, loads DT_NEEDED libs, and jumps to the program. */
    u64 interp_base = 0, interp_entry = 0;
    char ipath[VFS_MAX_PATH];
    if (is_linux && find_interp_path(v, ipath, sizeof ipath) == 0) {
        vnode_t *interp = vfs_lookup(ipath);
        if (interp) {
            interp_base = PAGE_ALIGN_UP(main_end + PAGE_SIZE);
            if (interp_base < USER_VBASE) interp_base = USER_VBASE;
            interp_entry = load_user_elf_into(interp, interp_base, new_pml4,
                                              rs, &nrs, NULL);
            if (!interp_entry) {
                kprintf("user: failed to load interpreter %s\n", ipath);
                interp_base = 0;
            } else {
                /* the interpreter is itself a relocatable PIE .so: apply its
                 * R_X86_64_RELATIVE relocations so its own global pointers
                 * are correct before it runs */
                apply_relocations(interp, (const ehdr_t *)interp->data,
                                  interp_base);
                kprintf("user: dynamic: interpreter %s @%p entry=%p\n",
                        ipath, (void *)interp_base, (void *)interp_entry);
            }
        } else {
            kprintf("user: interpreter %s not found\n", ipath);
        }
    }
    user_map_trampoline(new_pml4, rs, &nrs);   /* sigreturn helper page */

    /* Linux binaries get a real auxiliary vector (musl/glibc _start reads it
     * for AT_PHDR/AT_ENTRY/AT_RANDOM; a dynamic program's interpreter reads
     * AT_BASE for itself); YartOS binaries don't. */
    u64 auxv[2 * 12]; int auxc = 0;
    if (is_linux) {
        const ehdr_t *eh = (const ehdr_t *)v->data;
        u64 phdr = bias + eh->phoff;
        auxv[auxc++] = AT_SYSINFO_EHDR; auxv[auxc++] = 0;
        auxv[auxc++] = AT_PHDR;   auxv[auxc++] = phdr;
        auxv[auxc++] = AT_PHENT;  auxv[auxc++] = eh->phentsize;
        auxv[auxc++] = AT_PHNUM;  auxv[auxc++] = eh->phnum;
        auxv[auxc++] = AT_PAGESZ; auxv[auxc++] = PAGE_SIZE;
        auxv[auxc++] = AT_BASE;   auxv[auxc++] = interp_base;
        auxv[auxc++] = AT_ENTRY;  auxv[auxc++] = entry;
        auxv[auxc++] = AT_CLKTCK; auxv[auxc++] = 100;
        auxv[auxc++] = AT_RANDOM; auxv[auxc++] = 0;   /* rewritten by the builder */
    }
    /* A dynamically-linked program starts in the INTERPRETER; it later jumps
     * to AT_ENTRY (the main program's entry, still recorded above). */
    if (interp_entry) entry = interp_entry;
    u64 user_rsp = build_user_stack_into(new_pml4, rs, &nrs, top,
                                         kargv, argc, kenvp, envc,
                                         auxv, auxc);
    if (!user_rsp) goto fail;

    /* drop the old address space (t->pml4 already points at the new one).
     * If it is a SHARED thread PML4, unref (the last user frees it); else
     * free it outright. */
    if (old_pml4 && old_pml4 != vmm_kernel_pml4() &&
        sched_pml4_unref(old_pml4))
        vmm_free_pml4(old_pml4);

    memcpy(t->regions, rs, sizeof rs);
    t->region_count = nrs;
    t->linux_abi = is_linux;                 /* foreign binary: translate syscalls */
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

    /* exec(2) replaces the program, so the task NAME becomes the new
     * program's (POSIX comm).  Skipping this kept every app named after
     * its parent ("wm"), which made `ps` and the watchdog report every
     * application as the compositor. */
    char old_name[TASK_NAME_LEN];
    strncpy(old_name, t->name, TASK_NAME_LEN - 1);
    old_name[TASK_NAME_LEN - 1] = 0;
    strncpy(t->name, v->name, TASK_NAME_LEN - 1);
    t->name[TASK_NAME_LEN - 1] = 0;

    kprintf("exec: pid %u '%s' -> %s entry=%p rsp=%p argc=%d env=%d\n",
            t->pid, old_name, v->name, (void *)entry, (void *)user_rsp,
            argc, envc);
    return true;
fail:
    /* restore the OLD tables and keep running the old image */
    t->pml4 = old_pml4;
    vmm_switch_pml4(old_pml4 ? old_pml4 : vmm_kernel_pml4());
    vmm_free_pml4(new_pml4);
    return false;
}
