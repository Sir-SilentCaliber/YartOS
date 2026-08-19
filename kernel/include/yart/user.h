#pragma once
#include <yart/types.h>
#include <yart/fs.h>
#include <yart/hal.h>      /* cpu_regs_t */
#include <yart/mm.h>       /* user_region_t */

#define USER_VBASE   0x40000000UL    /* matches userland/init.ld          */
/* Lowest legal user VA.  YartOS's own code/stack/mmap all live at or above
 * USER_VBASE, but foreign LINUX ET_EXEC binaries load at their fixed low
 * address (classically 0x400000).  Everything below USER_VBASE is otherwise
 * unused, so the low gigabyte is safe for those binaries. */
#define USER_VFLOOR  0x1000UL
#define USER_STACK_TOP 0x80000000UL
#define USER_STACK_PAGES 32

/* Dynamic-memory (mmap) arena: between the ASLR code span and the stack. */
#define USER_MMAP_BASE   0x50000000UL
#define USER_MMAP_END    0x60000000UL
/* Framebuffer window (ring-3 wm maps it here, above stack - well below
 * the canonical-address hole and far away from the mmap/code regions). */
#define USER_FB_BASE     0x70000000UL
#define USER_CS      0x1B            /* GDT entry 3 (user code) | RPL=3   */
#define USER_DS      0x23            /* GDT entry 4 (user data) | RPL=3   */
#define SYS_USER_CS  0x2B            /* GDT entry 5 (sysret user code)    */

/* Load an ELF + stack into a FRESH private PML4 and hand entry/rsp/pml4/
 * regions back; the scheduler then creates the task (sched_create_user).
 * Returns false on any failure. */
bool user_prepare_elf(vnode_t *v, u64 *entry_out, u64 *rsp_out,
                      u64 **pml4_out, user_region_t *regions_out,
                      int *nregions_out);

/* exec(2): replace the current task's address space with `v` and rewrite
 * `frame` so the syscall returns into the new program with argv/envp on
 * its stack.  kargv/kenvp must live in KERNEL memory.  Returns false on
 * any failure (the task keeps running its old image). */
bool user_exec(vnode_t *v, char *const kargv[], int argc,
               char *const kenvp[], int envc, cpu_regs_t *frame);
