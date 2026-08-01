/* Yart OS - PIT (channel 0, mode 3 square wave) */
#include <yart/hal.h>
#include <yart/io.h>
#include <yart/cpu.h>

#define PIT_CH0  0x40
#define PIT_CMD  0x43
#define PIT_HZ_BASE 1193182U

static volatile u64 ticks;
static u32 freq_hz;

/* Shared tick handler: driven by the PIT (vector 32) or, once apic.c has
 * switched delivery, by the LAPIC timer (vector 48). */
void yart_timer_irq(cpu_regs_t *r) {
    (void)r;
    ticks++;
    /* per-CPU tick counter (SMP: each AP's APIC timer bumps its own) */
    cpu_local_t *c = get_cpu_local();
    if (c && c->magic == CPU_LOCAL_MAGIC) c->ap_ticks++;
}

void pit_init(u32 hz) {
    freq_hz = hz;
    u32 div = PIT_HZ_BASE / hz;
    outb(PIT_CMD, 0x36);
    outb(PIT_CH0, div & 0xFF);
    outb(PIT_CH0, (div >> 8) & 0xFF);
    irq_register(32 + 0, yart_timer_irq);
    pic_unmask(0);
}

u64 pit_ticks(void) { return ticks; }

void sleep_ms(u32 ms) {
    u64 target = ticks + (u64)ms * freq_hz / 1000;
    while (ticks < target) __asm__ volatile ("hlt");
}
