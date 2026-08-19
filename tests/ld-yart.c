/* ld-yart.so — a minimal but REAL x86_64 dynamic linker (the interpreter).
 *
 * This is the YartOS equivalent of ld.so / ld-musl: the kernel loads it as
 * the PT_INTERP interpreter of a dynamically-linked program, applies its
 * R_X86_64_RELATIVE relocations, and enters it.  The linker then:
 *   1. reads the auxv (AT_PHDR/AT_ENTRY/AT_BASE) to find the main program,
 *   2. walks the main program's PT_DYNAMIC for its string/symbol/reloc tables
 *      and its DT_NEEDED shared libraries,
 *   3. loads each needed .so (mmap RWX + copy its PT_LOAD segments) and
 *      applies its relocations,
 *   4. applies the main program's relocations (RELATIVE / GLOB_DAT /
 *      JUMP_SLOT / 64) by resolving symbols across all loaded objects,
 *   5. jumps to the program's entry (AT_ENTRY) with the original stack.
 *
 * HONEST SCOPE: this resolves RELATIVE/GLOB_DAT/JUMP_SLOT/64/COPY/IRELATIVE,
 * the full TLS set (DTPMOD64/DTPOFF64/DTPOFF32/TPOFF64/TPOFF32 + static TLS
 * + __tls_get_addr), and STT_GNU_IFUNC (the resolver is called).  It binds
 * eagerly (no lazy PLT) and does NOT yet handle GNU symbol versioning —
 * those are the remaining gaps versus a full ld.so.
 *
 * Built as a freestanding PIE shared object: no libc, raw Linux syscalls.
 */
typedef unsigned long u64;
typedef long i64;
typedef unsigned int u32;
typedef int i32;
typedef unsigned short u16;
typedef unsigned char u8;
typedef char bool;
#define true 1
#define false 0

/* ---- ELF64 ---- */
typedef struct {
    unsigned char ident[16];
    u16 type, machine; u32 version;
    u64 entry, phoff, shoff; u32 flags;
    u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} ElfEhdr;
typedef struct { u32 type, flags; u64 offset, vaddr, paddr, filesz, memsz, align; } ElfPhdr;
typedef struct { i64 tag; u64 val; } ElfDyn;
typedef struct { u32 name; unsigned char info, other; u16 shndx; u64 value, size; } ElfSym;
typedef struct { u64 off; i64 info, addend; } ElfRela;

#define PT_LOAD     1
#define PT_DYNAMIC  2
#define PT_TLS      7
#define DT_NULL     0
#define DT_NEEDED   1
#define DT_PLTRELSZ 2
#define DT_STRTAB   5
#define DT_SYMTAB   6
#define DT_RELA     7
#define DT_RELASZ   8
#define DT_RELAENT  9
#define DT_STRSZ    10
#define DT_SYMENT   11
#define DT_JMPREL   23
#define DT_PLTREL   20

#define R_X86_64_64        1
#define R_X86_64_COPY      5
#define R_X86_64_GLOB_DAT  6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE  8
#define R_X86_64_DTPMOD64  16
#define R_X86_64_DTPOFF64  17
#define R_X86_64_TPOFF64   18
#define R_X86_64_DTPOFF32  21
#define R_X86_64_TPOFF32   22
#define R_X86_64_IRELATIVE 37

#define STT_GNU_IFUNC      10
#define ELF64_ST_TYPE(i)   ((i) & 0xf)

#define AT_NULL  0
#define AT_PHDR  3
#define AT_PHENT 4
#define AT_PHNUM 5
#define AT_BASE  7
#define AT_ENTRY 9

#define ELF64_R_SYM(i)   ((i) >> 32)
#define ELF64_R_TYPE(i)  ((i) & 0xFFFFFFFF)

/* ---- raw Linux syscalls ---- */
static i64 sc(i64 n, i64 a, i64 b, i64 c, i64 d, i64 e, i64 f) {
    i64 r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(d), "r"(e), "r"(f)
                     : "rcx", "r11", "memory");
    return r;
}
static i64 S_write(i64 fd, const void *b, u64 n)  { return sc(1, fd, (i64)b, (i64)n, 0, 0, 0); }
static i64 S_open(const char *p)                  { return sc(2, (i64)p, 0, 0, 0, 0, 0); }
static i64 S_read(i64 fd, void *b, u64 n)         { return sc(0, fd, (i64)b, (i64)n, 0, 0, 0); }
static i64 S_close(i64 fd)                        { return sc(3, fd, 0, 0, 0, 0, 0); }
static i64 S_mmap(u64 len)                        { return sc(9, 0, (i64)len, 7, 0x22, -1, 0); } /* RWX anon */
static i64 S_munmap(void *a, u64 len)             { return sc(11, (i64)a, (i64)len, 0, 0, 0, 0); }
static void S_exit_group(i64 code)                { sc(231, code, 0, 0, 0, 0, 0); __builtin_unreachable(); }

/* ---- tiny libc ---- */
static u64 strlen(const char *s) { u64 n = 0; while (s[n]) n++; return n; }
static void *memcpy(void *d, const void *s, u64 n) { u8 *dd = d; const u8 *ss = s; for (u64 i = 0; i < n; i++) dd[i] = ss[i]; return d; }
static int strcmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (int)(unsigned char)*a - (int)(unsigned char)*b; }

static void emit(const char *s) { S_write(1, s, strlen(s)); }

/* ---- the loaded-object list ---- */
typedef struct {
    void     *base;      /* load address (add to vaddr-relative values) */
    ElfEhdr  *eh;
    ElfPhdr  *ph; u64 phnum;
    ElfDyn   *dyn;
    char     *strtab;
    ElfSym   *symtab;
    ElfRela  *rela;   u64 relasz, relaent;
    ElfRela  *jmprel; u64 pltrelsz;
    u64       nsyms;      /* dynsym entry count */
    u64       tls_memsz, tls_filesz, tls_off, tls_align, tls_base;
    char      name[64];
} obj_t;

static obj_t g_objs[16];
static int   g_nobj;

/* TLS runtime state (x86-64 variant-2 layout) */
static u64 g_tls_base[16];   /* module id -> runtime TLS block base */
static u64 g_tp;             /* the thread pointer (%fs base)      */

static u64 align_up(u64 v, u64 a) { if (a < 1) a = 1; return (v + a - 1) & ~(a - 1); }

/* read `size` bytes at file offset `off` from `path` into `dst` */
static void read_file_at(const char *path, u64 off, void *dst, u64 size) {
    i64 fd = S_open(path);
    if (fd < 0) return;
    u8 tmp[512]; u64 skipped = 0;
    while (skipped < off) {
        u64 want = off - skipped; if (want > 512) want = 512;
        i64 n = S_read(fd, tmp, want);
        if (n <= 0) break;
        skipped += (u64)n;
    }
    S_read(fd, dst, size);
    S_close(fd);
}

/* The general-dynamic TLS resolver: given a GOT entry holding
 * {DTPMOD, DTPOFF}, return the address of that thread-local variable. */
u64 __tls_get_addr(u64 *p) {
    return g_tls_base[p[0]] + p[1];
}

/* call an IFUNC resolver (returns the real function address) */
typedef u64 (*resolver_fn)(void);
static u64 call_resolver(u64 addr) { return ((resolver_fn)addr)(); }

/* find a dynamic entry by tag, or NULL */
static u64 *dyn_find(ElfDyn *d, i64 tag) {
    for (; d->tag != DT_NULL; d++) if (d->tag == tag) return &d->val;
    return 0;
}

/* resolve a symbol index in object `o`: search `o` first, then all objects,
 * for a DEFINED symbol with that name.  Returns the absolute value and (via
 * *mod_out, if non-NULL) the defining object's index. */
static u64 resolve_symbol_ex(obj_t *o, u64 symidx, int *mod_out) {
    ElfSym *s = &o->symtab[symidx];
    const char *name = o->strtab + s->name;
    for (int pass = 0; pass < 2; pass++) {
        for (int k = 0; k < g_nobj; k++) {
            obj_t *cand = &g_objs[k];
            if (pass == 0 && cand != o) continue;   /* pass 0: self only */
            if (pass == 1 && cand == o) continue;
            if (!cand->symtab || !cand->strtab) continue;
            for (u64 j = 1; j < cand->nsyms; j++) {
                ElfSym *cs = &cand->symtab[j];
                if ((cs->shndx != 0) &&
                    strcmp(cand->strtab + cs->name, name) == 0) {
                    u64 val = (u64)cand->base + cs->value;
                    /* IFUNC: st_value is the RESOLVER; call it to get the
                     * real function (glibc uses this for memcpy & friends) */
                    if (ELF64_ST_TYPE(cs->info) == STT_GNU_IFUNC)
                        val = call_resolver(val);
                    if (mod_out) *mod_out = k;
                    return val;
                }
            }
        }
    }
    if (mod_out) *mod_out = 0;
    return 0;
}
static u64 resolve_symbol(obj_t *o, u64 symidx) { return resolve_symbol_ex(o, symidx, 0); }

/* find the defining module + raw TLS offset (st_value) for symbol `symidx` */
static int sym_tls_info(obj_t *o, u64 symidx, int *mod_out, u64 *val_out) {
    ElfSym *s = &o->symtab[symidx];
    const char *name = o->strtab + s->name;
    for (int pass = 0; pass < 2; pass++) {
        for (int k = 0; k < g_nobj; k++) {
            obj_t *cand = &g_objs[k];
            if (pass == 0 && cand != o) continue;
            if (pass == 1 && cand == o) continue;
            if (!cand->symtab || !cand->strtab) continue;
            for (u64 j = 1; j < cand->nsyms; j++) {
                ElfSym *cs = &cand->symtab[j];
                if ((cs->shndx != 0) &&
                    strcmp(cand->strtab + cs->name, name) == 0) {
                    *mod_out = k;
                    *val_out = cs->value;   /* the TLS offset within the module */
                    return 1;
                }
            }
        }
    }
    return 0;
}

/* apply an object's relocation table */
static void apply_relocs(obj_t *o, ElfRela *rela, u64 count) {
    for (u64 i = 0; i < count; i++) {
        ElfRela *r = &rela[i];
        u64 type = ELF64_R_TYPE((u64)r->info);
        u64 sym  = ELF64_R_SYM((u64)r->info);
        u64 *P   = (u64 *)((u8 *)o->base + r->off);
        switch (type) {
        case R_X86_64_RELATIVE:
            *P = (u64)o->base + r->addend; break;
        case R_X86_64_DTPMOD64: {               /* general-dynamic module id */
            int m; u64 v;
            if (sym_tls_info(o, sym, &m, &v)) *P = (u64)m;
            break; }
        case R_X86_64_DTPOFF64: {               /* offset within the module  */
            int m; u64 v;
            if (sym_tls_info(o, sym, &m, &v)) *P = v;
            break; }
        case R_X86_64_DTPOFF32: {
            int m; u64 v;
            if (sym_tls_info(o, sym, &m, &v)) *(u32 *)P = (u32)v;
            break; }
        case R_X86_64_TPOFF64: {                /* static TLS: addr - TP     */
            int m; u64 v;
            if (sym_tls_info(o, sym, &m, &v))
                *P = (g_tls_base[m] + v) - g_tp;
            break; }
        case R_X86_64_TPOFF32: {
            int m; u64 v;
            if (sym_tls_info(o, sym, &m, &v))
                *(u32 *)P = (u32)((g_tls_base[m] + v) - g_tp);
            break; }
        case R_X86_64_64:
        case R_X86_64_GLOB_DAT:
        case R_X86_64_JUMP_SLOT:
            *P = resolve_symbol(o, sym) + (u64)r->addend; break;
        case R_X86_64_COPY: {               /* copy a global from the main exec */
            int m; u64 src = resolve_symbol_ex(o, sym, &m);
            ElfSym *s = &o->symtab[sym];
            if (src && s->size) memcpy(P, (void *)src, s->size);
            break; }
        case R_X86_64_IRELATIVE:            /* slot holds a resolver: call it */
            *P = call_resolver((u64)o->base + r->addend);
            break;
        default: break;   /* unhandled reloc: leave as-is (best effort) */
        }
    }
}

/* load a shared library from /lib/<name> and append it to the object list */
static void load_lib(const char *name) {
    char path[96];
    const char *pre = "/lib/";
    u64 k = 0;
    while (*pre) path[k++] = *pre++;
    const char *np = name;               /* don't clobber `name` (used below) */
    while (*np && k < 90) path[k++] = *np++;
    path[k] = 0;

    i64 fd = S_open(path);
    if (fd < 0) { emit("ld: open failed: "); emit(path); emit("\n"); return; }

    /* read the whole file into a scratch buffer (libraries are tiny) */
    static u8 scratch[65536];
    u64 fsz = 0;
    for (;;) {
        i64 n = S_read(fd, scratch + fsz, 4096);
        if (n <= 0) break;
        fsz += (u64)n;
    }
    S_close(fd);
    if (fsz < 64) return;

    ElfEhdr *eh = (ElfEhdr *)scratch;
    if (eh->ident[0] != 0x7f || eh->ident[1] != 'E') return;
    ElfPhdr *ph = (ElfPhdr *)(scratch + eh->phoff);
    if (eh->phnum > 32) return;

    /* compute the total memory span (from the lowest page-aligned vaddr) */
    u64 lo = ~0ULL, hi = 0;
    for (u64 i = 0; i < eh->phnum; i++)
        if (ph[i].type == PT_LOAD) {
            u64 a = ph[i].vaddr & ~0xFFFULL;
            u64 b = (ph[i].vaddr + ph[i].memsz + 0xFFF) & ~0xFFFULL;
            if (a < lo) lo = a;
            if (b > hi) hi = b;
        }
    u64 span = hi - lo;
    void *base = (void *)S_mmap(span);
    if ((i64)base < 0) return;
    /* copy each PT_LOAD's file bytes (at p_offset) into the mapping */
    for (u64 i = 0; i < eh->phnum; i++)
        if (ph[i].type == PT_LOAD)
            memcpy((u8 *)base + (ph[i].vaddr - lo),
                   scratch + ph[i].offset, ph[i].filesz);

    /* record the object */
    obj_t *o = &g_objs[g_nobj++];
    o->base = base; o->eh = (ElfEhdr *)base; o->ph = (ElfPhdr *)((u8 *)base + eh->phoff); o->phnum = eh->phnum;
    u64 nlen = 0; while (name[nlen] && nlen < 63) nlen++;
    for (u64 i = 0; i < nlen; i++) o->name[i] = name[i];
    o->name[nlen] = 0;
    o->dyn = 0; o->strtab = 0; o->symtab = 0; o->rela = 0; o->relasz = 0; o->relaent = 0; o->jmprel = 0; o->pltrelsz = 0;
    o->tls_memsz = 0; o->tls_filesz = 0; o->tls_off = 0; o->tls_align = 1; o->tls_base = 0;

    /* capture PT_TLS (initialized TLS data lives in the file at p_offset) */
    for (u64 i = 0; i < eh->phnum; i++)
        if (ph[i].type == PT_TLS) {
            o->tls_memsz  = ph[i].memsz;
            o->tls_filesz = ph[i].filesz;
            o->tls_off    = ph[i].offset;
            o->tls_align  = ph[i].align ? ph[i].align : 1;
            break;
        }

    /* find PT_DYNAMIC (file-relative vaddr; rebase against base-lo) */
    for (u64 i = 0; i < eh->phnum; i++)
        if (ph[i].type == PT_DYNAMIC)
            o->dyn = (ElfDyn *)((u8 *)base + (ph[i].vaddr - lo));

    if (o->dyn) {
        u64 *p;
        if ((p = dyn_find(o->dyn, DT_STRTAB))) o->strtab = (char *)((u8 *)base + (*p - lo));
        if ((p = dyn_find(o->dyn, DT_SYMTAB))) o->symtab = (ElfSym *)((u8 *)base + (*p - lo));
        if ((p = dyn_find(o->dyn, DT_RELA)))  o->rela  = (ElfRela *)((u8 *)base + (*p - lo));
        if ((p = dyn_find(o->dyn, DT_RELASZ)))o->relasz = *p;
        if ((p = dyn_find(o->dyn, DT_RELAENT)))o->relaent = *p ? *p : 24;
        if ((p = dyn_find(o->dyn, DT_JMPREL)))o->jmprel = (ElfRela *)((u8 *)base + (*p - lo));
        if ((p = dyn_find(o->dyn, DT_PLTRELSZ)))o->pltrelsz = *p;
        o->nsyms = (o->strtab && o->symtab)
                 ? (u64)((char *)o->strtab - (char *)o->symtab) / 24 : 0;
    }
    emit("ld: loaded "); emit(path); emit("\n");
}

static void ld_entry(u64 rsp) {
    /* walk to the auxv: rsp -> argc, argv[], NULL, envp[], NULL, auxv */
    u64 *sp = (u64 *)rsp;
    u64 argc = sp[0];
    u64 *argv = sp + 1;
    u64 *p = argv + argc + 1;        /* skip argv + its NULL */
    while (*p) p++;                  /* skip envp */
    p++;                             /* past envp NULL */
    /* now p = auxv pairs */
    u64 phdr = 0, phent = 0, phnum = 0, entry = 0, base = 0;
    for (; p[0] != AT_NULL; p += 2) {
        if (p[0] == AT_PHDR)  phdr  = p[1];
        if (p[0] == AT_PHENT) phent = p[1];
        if (p[0] == AT_PHNUM) phnum = p[1];
        if (p[0] == AT_ENTRY) entry = p[1];
        if (p[0] == AT_BASE)  base  = p[1];
    }

    emit("ld-yart: dynamic linker up\n");

    /* register the MAIN program as object 0.  The kernel already mapped it,
     * but the auxv only carries AT_PHDR (= load_base + e_phoff) and AT_ENTRY.
     * Recover the load base by scanning DOWN from AT_PHDR for the ELF magic
     * (e_phoff is small, so the header is on the same or an adjacent page). */
    obj_t *main = &g_objs[g_nobj++];
    main->ph = (ElfPhdr *)phdr;
    main->phnum = phnum;
    main->dyn = 0; main->strtab = 0; main->symtab = 0;
    main->rela = 0; main->relasz = 0; main->relaent = 0; main->jmprel = 0; main->pltrelsz = 0;
    main->tls_memsz = 0; main->tls_filesz = 0; main->tls_off = 0; main->tls_align = 1; main->tls_base = 0;
    main->name[0] = 0;
    /* the executable path is argv[0] (needed to re-read its .tdata) */
    const char *exe_path = (const char *)argv[0];
    {
        u64 cand = phdr & ~0xFFFULL, B = 0;
        for (int tries = 0; tries < 64 && !B; tries++, cand -= 0x1000) {
            unsigned char *m = (unsigned char *)cand;
            if (m[0] == 0x7f && m[1] == 'E' && m[2] == 'L' && m[3] == 'F') B = cand;
        }
        main->base = (void *)B;
        main->eh  = (ElfEhdr *)B;
    }

    /* register the INTERPRETER (this .so, at AT_BASE) so its exported
     * symbols — __tls_get_addr — are resolvable by the programs' relocs. */
    {
        obj_t *self = &g_objs[g_nobj++];
        self->base = (void *)base;
        self->eh = (ElfEhdr *)base;
        self->ph = (ElfPhdr *)((u8 *)base + self->eh->phoff);
        self->phnum = self->eh->phnum;
        self->dyn = 0; self->strtab = 0; self->symtab = 0;
        self->rela = 0; self->relasz = 0; self->relaent = 0; self->jmprel = 0; self->pltrelsz = 0;
        self->tls_memsz = 0; self->tls_filesz = 0; self->tls_off = 0; self->tls_align = 1; self->tls_base = 0;
        self->name[0] = 0;
        for (u64 i = 0; i < self->phnum; i++)
            if (self->ph[i].type == PT_DYNAMIC) { self->dyn = (ElfDyn *)((u8 *)base + self->ph[i].vaddr); break; }
        if (self->dyn) {
            u64 *q;
            if ((q = dyn_find(self->dyn, DT_STRTAB))) self->strtab = (char *)((u8 *)base + *q);
            if ((q = dyn_find(self->dyn, DT_SYMTAB))) self->symtab = (ElfSym *)((u8 *)base + *q);
            self->nsyms = (self->strtab && self->symtab)
                        ? (u64)((char *)self->strtab - (char *)self->symtab) / 24 : 0;
        }
    }

    /* parse main's PT_DYNAMIC */
    for (u64 i = 0; i < phnum; i++) {
        if (main->ph[i].type != PT_DYNAMIC) continue;
        main->dyn = (ElfDyn *)((u8 *)main->base + main->ph[i].vaddr);
        break;
    }
    u64 *dp;
    if (main->dyn) {
        if ((dp = dyn_find(main->dyn, DT_STRTAB))) main->strtab = (char *)((u8 *)main->base + *dp);
        if ((dp = dyn_find(main->dyn, DT_SYMTAB))) main->symtab = (ElfSym *)((u8 *)main->base + *dp);
        if ((dp = dyn_find(main->dyn, DT_RELA)))  main->rela  = (ElfRela *)((u8 *)main->base + *dp);
        if ((dp = dyn_find(main->dyn, DT_RELASZ)))main->relasz = *dp;
        if ((dp = dyn_find(main->dyn, DT_RELAENT)))main->relaent = *dp ? *dp : 24;
        if ((dp = dyn_find(main->dyn, DT_JMPREL)))main->jmprel = (ElfRela *)((u8 *)main->base + *dp);
        if ((dp = dyn_find(main->dyn, DT_PLTRELSZ)))main->pltrelsz = *dp;
        main->nsyms = (main->strtab && main->symtab)
                    ? (u64)((char *)main->strtab - (char *)main->symtab) / 24 : 0;

        /* load DT_NEEDED libraries (the d_val is a DT_STRTAB offset) */
        for (ElfDyn *d = main->dyn; d->tag != DT_NULL; d++)
            if (d->tag == DT_NEEDED)
                load_lib(main->strtab + (u64)d->val);
    }

    /* ---- static TLS block (variant 2): lay out, init, set %fs ---- */
    {
        /* the executable's TLS block sits directly below the thread pointer
         * (local-exec: __thread vars are at TP - offset), so its size is the
         * RAW PT_TLS memsz — NOT aligned up (aligning shifted TP and broke
         * the baked-in offsets). */
        u64 exe_size = 0, exe_fsz = 0, exe_off = 0;
        for (u64 i = 0; i < main->phnum; i++)
            if (main->ph[i].type == PT_TLS) {
                exe_size = main->ph[i].memsz;
                exe_fsz  = main->ph[i].filesz;
                exe_off  = main->ph[i].offset;
                break;
            }
        u64 so_total = 0;
        for (int k = 1; k < g_nobj; k++) {
            obj_t *o = &g_objs[k];
            if (!o->tls_memsz) continue;
            so_total = align_up(so_total, o->tls_align);
            so_total += o->tls_memsz;
        }
        u64 total = 16 + so_total + exe_size;
        void *block = (void *)S_mmap(total);
        if ((i64)block > 0) {
            /* .so TLS blocks from block+0 upward; exe block above; TP on top */
            u64 off = 0;
            for (int k = 1; k < g_nobj; k++) {
                obj_t *o = &g_objs[k];
                if (!o->tls_memsz) continue;
                off = align_up(off, o->tls_align);
                o->tls_base = (u64)block + off;
                g_tls_base[k] = o->tls_base;
                char lp[96]; const char *pr = "/lib/"; u64 lk = 0;
                while (*pr) lp[lk++] = *pr++;
                const char *nm = o->name;
                while (*nm && lk < 90) lp[lk++] = *nm++;
                lp[lk] = 0;
                read_file_at(lp, o->tls_off, (void *)o->tls_base, o->tls_filesz);
                off += o->tls_memsz;
            }
            g_tls_base[0] = (u64)block + off;
            g_tp = (u64)block + off + exe_size;
            if (exe_fsz)
                read_file_at(exe_path, exe_off, (void *)g_tls_base[0], exe_fsz);
            *(u64 *)g_tp = g_tp;                       /* TCB self-pointer */
            { u64 r; __asm__ volatile("syscall" : "=a"(r)   /* set %fs */
                  : "a"(158), "D"(0x1002), "S"(g_tp) : "rcx", "r11", "memory"); }
            emit("ld: static TLS up\n");
        }
    }

    /* apply relocations: libraries first, then the main program */
    for (int k = 1; k < g_nobj; k++) {
        obj_t *o = &g_objs[k];
        if (o->rela)   apply_relocs(o, o->rela,   o->relasz / (o->relaent ? o->relaent : 24));
        if (o->jmprel) apply_relocs(o, o->jmprel, o->pltrelsz / 24);
    }
    if (main->rela)   apply_relocs(main, main->rela,   main->relasz / (main->relaent ? main->relaent : 24));
    if (main->jmprel) apply_relocs(main, main->jmprel, main->pltrelsz / 24);

    emit("ld-yart: relocations done, jumping to main\n");

    /* jump to the program entry with the ORIGINAL stack (argc/argv/envp) */
    void (*go)(void) = (void (*)(void))entry;
    __asm__ volatile("mov %0, %%rsp" : : "r"(rsp) : "memory");
    go();
    /* if main returns, exit the whole thread group */
    S_exit_group(0);
}

__attribute__((naked)) void _start(void) {
    __asm__ volatile(
        "mov %rsp, %rdi\n\t"
        "andq $-16, %rsp\n\t"
        "call ld_entry\n\t"
        "ud2\n\t");
}
