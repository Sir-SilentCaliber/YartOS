#pragma once
#include <yart/types.h>
#include <yart/fs.h>

#define USER_VBASE   0x40000000UL    /* matches userland/init.ld          */
#define USER_STACK_TOP 0x70000000UL
#define USER_STACK_PAGES 4

/* Dynamic-memory (mmap) arena: between the ASLR code span and the stack. */
#define USER_MMAP_BASE   0x50000000UL
#define USER_MMAP_END    0x60000000UL
#define USER_CS      0x1B            /* GDT entry 3 (user code) | RPL=3   */
#define USER_DS      0x23            /* GDT entry 4 (user data) | RPL=3   */

/* Load /bin/init's ELF and stack into the user address space and hand the
 * entry/stack back; the scheduler then creates the task (sched_create_user).
 * Returns false on any failure. */
bool user_prepare_elf(vnode_t *v, u64 *entry_out, u64 *rsp_out);
