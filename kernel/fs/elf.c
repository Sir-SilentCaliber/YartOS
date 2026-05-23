/* Yart OS - minimal ELF64 loader.
 *
 * Loads an ET_EXEC or ET_DYN file out of the VFS into a fresh address
 * range.  Today we only validate + map; ring-3 entry happens later
 * (see userland/README.md).  This is enough to verify that the toolchain
 * can produce a Yart-targeted user binary.
 *
 * Usage:
 *     vnode_t *v = vfs_lookup("/bin/init");
 *     u64 entry  = elf_load(v);
 *     // jump_to_user(entry, ...);
 */
#include <yart/types.h>
#include <yart/fs.h>
#include <yart/mm.h>
#include <yart/string.h>
#include <yart/console.h>

#define ELF_MAGIC 0x464C457FU   /* "\177ELF" little-endian */

typedef struct PACKED {
    u32 magic;
    u8  cls, data, ver, osabi;
    u8  pad[8];
    u16 type, machine;
    u32 version;
    u64 entry, phoff, shoff;
    u32 flags;
    u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} elf64_ehdr_t;

typedef struct PACKED {
    u32 type, flags;
    u64 offset, vaddr, paddr, filesz, memsz, align;
} elf64_phdr_t;

#define PT_LOAD 1

u64 elf_load(vnode_t *v) {
    if (!v || v->type != VN_FILE || v->size < sizeof(elf64_ehdr_t)) return 0;
    elf64_ehdr_t *eh = v->data;
    if (eh->magic != ELF_MAGIC) {
        kprintf("elf: bad magic in %s\n", v->name);
        return 0;
    }
    if (eh->cls != 2 /* 64-bit */) {
        kprintf("elf: not 64-bit\n");
        return 0;
    }
    elf64_phdr_t *ph = (elf64_phdr_t *)((u8 *)v->data + eh->phoff);
    int loaded = 0;
    for (int i = 0; i < eh->phnum; i++) {
        if (ph[i].type != PT_LOAD) continue;
        u64 va    = ph[i].vaddr;
        u64 mem   = ph[i].memsz;
        u64 file  = ph[i].filesz;
        u64 first = PAGE_ALIGN_DOWN(va);
        u64 last  = PAGE_ALIGN_UP(va + mem);
        u64 flags = PTE_RW | PTE_US;
        for (u64 a = first; a < last; a += PAGE_SIZE) {
            paddr_t p = pmm_alloc_page();
            vmm_map(a, p, flags);
        }
        memcpy((void *)va, (u8 *)v->data + ph[i].offset, file);
        if (mem > file) memset((void *)(va + file), 0, mem - file);
        loaded++;
    }
    kprintf("elf: %s loaded, %d segs, entry=%p\n",
            v->name, loaded, (void *)eh->entry);
    return eh->entry;
}
