/* Yart OS - launch a ring-3 task from an ELF file in the VFS.
 *
 * Pattern:
 *   1) parse ELF64, allocate user pages with PTE_US|PTE_RW for each
 *      PT_LOAD segment, copy file bytes in.
 *   2) allocate a user stack with PTE_US|PTE_RW.
 *   3) save callee-saved kernel context (jmpbuf-style) so SYS_EXIT can
 *      longjmp back here.
 *   4) push the IRETQ frame and dive.
 *
 * No scheduler.  When the task calls SYS_EXIT we longjmp back and the
 * kernel's own loop resumes.
 */
#include <yart/user.h>
#include <yart/mm.h>
#include <yart/fs.h>
#include <yart/string.h>
#include <yart/console.h>
#include <yart/io.h>

/* setjmp/longjmp-style context save */
typedef struct { u64 rbx, rbp, r12, r13, r14, r15, rsp, rip; } yart_jmpbuf_t;
static yart_jmpbuf_t g_kctx;
static i64           g_user_status;
static bool          g_user_returned;

static int yart_setjmp(yart_jmpbuf_t *b) {
    int r;
    __asm__ volatile (
        "mov %%rbx,  0(%%rdi)\n\t"
        "mov %%rbp,  8(%%rdi)\n\t"
        "mov %%r12, 16(%%rdi)\n\t"
        "mov %%r13, 24(%%rdi)\n\t"
        "mov %%r14, 32(%%rdi)\n\t"
        "mov %%r15, 40(%%rdi)\n\t"
        "mov %%rsp, 48(%%rdi)\n\t"
        "lea 1f(%%rip), %%rax\n\t"
        "mov %%rax, 56(%%rdi)\n\t"
        "xor %%eax, %%eax\n\t"
        "jmp 2f\n"
        "1:\n\t"
        "mov $1, %%eax\n"
        "2:\n\t"
        : "=a"(r) : "D"(b) : "memory", "cc"
    );
    return r;
}

static NORETURN void yart_longjmp(yart_jmpbuf_t *b, int v) {
    __asm__ volatile (
        "mov  0(%%rdi), %%rbx\n\t"
        "mov  8(%%rdi), %%rbp\n\t"
        "mov 16(%%rdi), %%r12\n\t"
        "mov 24(%%rdi), %%r13\n\t"
        "mov 32(%%rdi), %%r14\n\t"
        "mov 40(%%rdi), %%r15\n\t"
        "mov 48(%%rdi), %%rsp\n\t"
        "mov %%esi, %%eax\n\t"
        "test %%eax, %%eax\n\t"
        "jnz 1f\n\t"
        "mov $1, %%eax\n"
        "1:\n\t"
        "jmp *56(%%rdi)\n\t"
        :: "D"(b), "S"(v) : "memory"
    );
    __builtin_unreachable();
}

void user_return(i64 status) {
    g_user_status = status;
    g_user_returned = true;
    yart_longjmp(&g_kctx, 1);
}

#define ELF_MAGIC 0x464C457FU
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

static u64 load_user_elf(vnode_t *v) {
    if (!v || v->type != VN_FILE || v->size < sizeof(ehdr_t)) return 0;
    ehdr_t *eh = v->data;
    if (eh->magic != ELF_MAGIC || eh->cls != 2) {
        kprintf("user: bad ELF\n");
        return 0;
    }
    phdr_t *ph = (phdr_t *)((u8 *)v->data + eh->phoff);
    for (int i = 0; i < eh->phnum; i++) {
        if (ph[i].type != PT_LOAD) continue;
        u64 va    = ph[i].vaddr;
        u64 mem   = ph[i].memsz;
        u64 file  = ph[i].filesz;
        u64 first = PAGE_ALIGN_DOWN(va);
        u64 last  = PAGE_ALIGN_UP(va + mem);
        for (u64 a = first; a < last; a += PAGE_SIZE) {
            paddr_t p = pmm_alloc_page();
            vmm_map(a, p, PTE_RW | PTE_US);
        }
        memcpy((void *)va, (u8 *)v->data + ph[i].offset, file);
        if (mem > file) memset((void *)(va + file), 0, mem - file);
    }
    return eh->entry;
}

static u64 alloc_user_stack(void) {
    for (int i = 0; i < USER_STACK_PAGES; i++) {
        u64 va = USER_STACK_TOP - (i + 1) * PAGE_SIZE;
        paddr_t p = pmm_alloc_page();
        vmm_map(va, p, PTE_RW | PTE_US);
    }
    return USER_STACK_TOP - 16;
}

void user_run_elf(vnode_t *v) {
    u64 entry = load_user_elf(v);
    if (!entry) {
        kprintf("user: load failed\n");
        return;
    }
    u64 user_rsp = alloc_user_stack();
    kprintf("user: launching ring-3 entry=%p stack=%p\n",
            (void *)entry, (void *)user_rsp);

    g_user_returned = false;
    if (yart_setjmp(&g_kctx) != 0) {
        kprintf("user: task exited with status %ld\n", g_user_status);
        return;
    }

    __asm__ volatile (
        "cli\n\t"
        "mov $0x23, %%rax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "push $0x23\n\t"
        "push %0\n\t"
        "push $0x202\n\t"
        "push $0x1B\n\t"
        "push %1\n\t"
        "iretq\n\t"
        :: "r"(user_rsp), "r"(entry)
        : "rax", "memory"
    );
    __builtin_unreachable();
}
