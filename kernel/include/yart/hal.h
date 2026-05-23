#pragma once
#include <yart/types.h>

/* GDT */
void gdt_init(void);

/* IDT / interrupts */
typedef struct PACKED {
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rdi, rsi, rbp, rdx, rcx, rbx, rax;
    u64 vector, err;
    u64 rip, cs, rflags, rsp, ss;
} cpu_regs_t;

typedef void (*irq_handler_t)(cpu_regs_t *r);

void idt_init(void);
void irq_register(u8 irq, irq_handler_t h);
void isr_dispatch(cpu_regs_t *r);  /* called from asm */

/* PIC */
void pic_remap(int offset1, int offset2);
void pic_mask(u8 irq);
void pic_unmask(u8 irq);
void pic_eoi(u8 irq);

/* PIT */
void pit_init(u32 hz);
u64  pit_ticks(void);
void sleep_ms(u32 ms);

/* RTC */
typedef struct {
    u8 second, minute, hour;
    u8 day, month;
    u16 year;
} rtc_time_t;
void rtc_read(rtc_time_t *t);
