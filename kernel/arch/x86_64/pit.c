/* Yart OS - PIT (channel 0, mode 3 square wave) */
#include <yart/hal.h>
#include <yart/io.h>
#include <yart/cpu.h>
#include <yart/console.h>

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

/* ---- monotonic millisecond clock (TSC-backed) ----
 * The system tick is TICK_HZ (1 kHz), so a tick-derived ms clock has 1 ms
 * resolution - the baseline Skift parity for 60 Hz pacing.  The TSC gives
 * sub-millisecond resolution on top of that, which the compositor's
 * animations interpolate against for buttery motion.  Calibrated once
 * against the tick; falls back to the tick clock if the TSC is broken. */
static u64 g_tsc_per_ms;    /* TSC counts per millisecond (0 = uncalibrated) */
static u64 g_tsc_epoch;     /* TSC value at the calibration reference point  */
static u64 g_tsc_epoch_ms;  /* pit_ticks()-derived ms at that same point     */

static ALWAYS_INLINE u64 rdtsc_u64(void) {
    u32 lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

void tsc_calibrate(void) {
    /* Measures the TSC against the system tick counter (TICK_HZ rate) and
     * derives TSC counts-per-ms.  REQUIRES the tick to be live (interrupts
     * enabled) - call it after sti().  Calibrates over ~100 ms for a stable
     * rate; falls back to the coarse tick clock if the TSC looks broken. */
    u64 t0 = rdtsc_u64();
    u64 tick0 = pit_ticks();
    const u64 CAL_TICKS = TICK_HZ / 10;     /* 100 ms */
    while (pit_ticks() - tick0 < CAL_TICKS)
        __asm__ volatile ("pause");
    u64 t1 = rdtsc_u64();
    u64 tick1 = pit_ticks();
    u64 dt_ms = (tick1 - tick0) * 1000ULL / TICK_HZ;
    if (dt_ms && t1 > t0) {
        g_tsc_per_ms   = (t1 - t0) / dt_ms;
        g_tsc_epoch    = t1;
        g_tsc_epoch_ms = tick1 * (1000ULL / TICK_HZ);
    }
    kprintf("tsc: %llu counts/ms (calibrated over %llu ms)\n",
            (unsigned long long)g_tsc_per_ms,
            (unsigned long long)dt_ms);
}

u64 time_ms(void) {
    if (!g_tsc_per_ms) return pit_ticks() * (1000ULL / TICK_HZ);
    u64 t = rdtsc_u64();
    if (t < g_tsc_epoch) return g_tsc_epoch_ms;   /* TSC wrapped: clamp */
    return g_tsc_epoch_ms + (t - g_tsc_epoch) / g_tsc_per_ms;
}

void sleep_ms(u32 ms) {
    u64 target = ticks + (u64)ms * freq_hz / 1000;
    while (ticks < target) __asm__ volatile ("hlt");
}
