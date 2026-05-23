#pragma once
#include <yart/types.h>
#include <yart/fs.h>

#define USER_VBASE   0x40000000UL    /* matches userland/init.ld          */
#define USER_STACK_TOP 0x70000000UL
#define USER_STACK_PAGES 4
#define USER_CS      0x1B            /* GDT entry 3 (user code) | RPL=3   */
#define USER_DS      0x23            /* GDT entry 4 (user data) | RPL=3   */

void user_run_elf(vnode_t *v);       /* loads and iretqs into user mode   */
void user_return(i64 status);         /* invoked by SYS_EXIT to come back  */
