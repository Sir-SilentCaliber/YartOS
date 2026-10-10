#pragma once
#include <yart/types.h>

static ALWAYS_INLINE void outb(u16 port, u8 val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static ALWAYS_INLINE u8 inb(u16 port) {
    u8 r; __asm__ volatile ("inb %1, %0" : "=a"(r) : "Nd"(port)); return r;
}
static ALWAYS_INLINE void outw(u16 port, u16 val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}
static ALWAYS_INLINE u16 inw(u16 port) {
    u16 r; __asm__ volatile ("inw %1, %0" : "=a"(r) : "Nd"(port)); return r;
}
static ALWAYS_INLINE void outl(u16 port, u32 val) {
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}
static ALWAYS_INLINE u32 inl(u16 port) {
    u32 r; __asm__ volatile ("inl %1, %0" : "=a"(r) : "Nd"(port)); return r;
}
static ALWAYS_INLINE void io_wait(void) { outb(0x80, 0); }

static ALWAYS_INLINE void cli(void) { __asm__ volatile ("cli"); }
static ALWAYS_INLINE void sti(void) { __asm__ volatile ("sti"); }
static ALWAYS_INLINE void hlt(void) { __asm__ volatile ("hlt"); }

static ALWAYS_INLINE u64 read_cr2(void) {
    u64 v; __asm__ volatile ("mov %%cr2, %0" : "=r"(v)); return v;
}
static ALWAYS_INLINE u64 read_cr3(void) {
    u64 v; __asm__ volatile ("mov %%cr3, %0" : "=r"(v)); return v;
}
static ALWAYS_INLINE void write_cr3(u64 v) {
    __asm__ volatile ("mov %0, %%cr3" : : "r"(v) : "memory");
}
static ALWAYS_INLINE void invlpg(vaddr_t v) {
    __asm__ volatile ("invlpg (%0)" : : "r"(v) : "memory");
}

/* Save then disable IRQs; pair with irq_restore() */
static ALWAYS_INLINE u64 irq_save(void) {
    u64 f; __asm__ volatile ("pushfq; pop %0; cli" : "=r"(f) :: "memory");
    return f;
}
static ALWAYS_INLINE void irq_restore(u64 f) {
    __asm__ volatile ("push %0; popfq" :: "rm"(f) : "memory","cc");
}
