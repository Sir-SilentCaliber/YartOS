/* Yart OS - GDT (flat 64-bit, kernel + user code/data, TSS) */
#include <yart/types.h>
#include <yart/hal.h>
#include <yart/string.h>

typedef struct PACKED {
    u16 limit_lo;
    u16 base_lo;
    u8  base_mid;
    u8  access;
    u8  flags_limit_hi;
    u8  base_hi;
} gdt_entry_t;

typedef struct PACKED {
    u16 length;
    u16 base_lo;
    u8  base_mid1;
    u8  type;
    u8  flags;
    u8  base_mid2;
    u32 base_hi;
    u32 reserved;
} tss_desc_t;

typedef struct PACKED {
    u16 limit;
    u64 base;
} gdtr_t;

typedef struct PACKED {
    u32 reserved0;
    u64 rsp[3];
    u64 reserved1;
    u64 ist[7];
    u64 reserved2;
    u16 reserved3;
    u16 iopb_offset;
} tss_t;

static gdt_entry_t gdt[7];
static tss_t       tss;
static gdtr_t      gdtr;
static u8          ist1_stack[KB(16)] ALIGNED(16);

static void set_gate(int i, u8 access, u8 flags) {
    gdt[i].limit_lo = 0;
    gdt[i].base_lo = 0;
    gdt[i].base_mid = 0;
    gdt[i].access = access;
    gdt[i].flags_limit_hi = flags;
    gdt[i].base_hi = 0;
}

extern void gdt_flush(gdtr_t *p);   /* asm */
extern void tss_flush(u16 sel);     /* asm */

void gdt_init(void) {
    memset(&gdt, 0, sizeof gdt);
    /* 0: null */
    /* 1: kernel code  - 0x9A flags=0xA0 (L=1) */
    set_gate(1, 0x9A, 0xA0);
    /* 2: kernel data */
    set_gate(2, 0x92, 0xA0);
    /* 3: user code (DPL=3) - placed BEFORE data so STAR's user base works */
    set_gate(3, 0xFA, 0xA0);
    /* 4: user data */
    set_gate(4, 0xF2, 0xA0);
    /* 5+6: TSS (16 bytes) */
    memset(&tss, 0, sizeof tss);
    tss.rsp[0] = (u64)(ist1_stack + sizeof ist1_stack);
    tss.ist[0] = (u64)(ist1_stack + sizeof ist1_stack);
    tss.iopb_offset = sizeof tss;

    tss_desc_t *td = (tss_desc_t *)&gdt[5];
    u64 base = (u64)&tss;
    td->length    = sizeof tss - 1;
    td->base_lo   = base & 0xFFFF;
    td->base_mid1 = (base >> 16) & 0xFF;
    td->type      = 0x89;        /* present, type=available 64-bit TSS */
    td->flags     = 0x00;
    td->base_mid2 = (base >> 24) & 0xFF;
    td->base_hi   = (base >> 32) & 0xFFFFFFFF;
    td->reserved  = 0;

    gdtr.limit = sizeof gdt - 1;
    gdtr.base  = (u64)&gdt;
    gdt_flush(&gdtr);
    tss_flush(0x28);   /* selector for entry 5 */
}
