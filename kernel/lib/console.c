/* Yart OS - kernel console (serial + framebuffer text) */
#include <yart/console.h>
#include <yart/io.h>
#include <yart/string.h>
#include <yart/gui.h>
#include <yart/theme.h>
#include <yart/spinlock.h>
#include <stdarg.h>

#define COM1 0x3f8

static void klog_capture(char c);   /* forward decl (audit/dmesg ring) */

void serial_init(void) {
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x01);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
}

unsigned int g_cpu_count_hint = 1;   /* set by smp.c once APs are up */
static spinlock_t g_console_lock;
static u64 console_acquire(void) {
    u64 iflags = irq_save();               /* no IRQ while we hold it      */
    if (g_cpu_count_hint > 1) spin_lock(&g_console_lock);
    return iflags;
}
static void console_release(u64 iflags) {
    if (g_cpu_count_hint > 1) spin_unlock(&g_console_lock);
    irq_restore(iflags);
}

/* Bounded wait for the 16550 transmitter: under QEMU/TCG with many CPUs
 * hammering the console, the chardev backend can stall and THRE never sets.
 * Waiting forever would hold the console spinlock (IRQs off) and freeze the
 * whole system, so after a bounded spin we drop the character instead. */
static void serial_tx_wait(void) {
    u32 tries = 0;
    while (!(inb(COM1 + 5) & 0x20)) {
        if (++tries > 200000u) return;   /* give up: don't hang the OS */
        __asm__ volatile("pause");
    }
}

void serial_putc(char c) {
    if (c == '\n') { serial_tx_wait(); if (inb(COM1 + 5) & 0x20) outb(COM1, '\r'); }
    serial_tx_wait();
    if (inb(COM1 + 5) & 0x20) outb(COM1, c);
}

void serial_puts(const char *s) {
    u64 fl = console_acquire();
    for (const char *p = s; *p; p++) {
        serial_putc(*p);
        klog_capture(*p);          /* also capture into the audit/dmesg ring */
    }
    console_release(fl);
}

/* ---- kernel audit / dmesg log ring (row 19) ---- */
#define KLOG_LINES     256
#define KLOG_LINE_MAX  256
static struct {
    char text[KLOG_LINE_MAX];
    u16  len;
} g_klog[KLOG_LINES];
static u32    g_klog_next;     /* next slot to write                        */
static u64    g_klog_total;    /* total lines ever logged (monotonic)       */
static char   g_klog_cur[KLOG_LINE_MAX];
static u16    g_klog_cur_len;
static spinlock_t g_klog_lock;

/* Append a fully-rendered line to the ring (called on '\n').  `len` may be 0
 * for an empty line.  Runs with the klog lock + IRQs off. */
static void klog_flush(void) {
    u64 fl = irq_save();
    spin_lock(&g_klog_lock);
    u16 n = g_klog_cur_len < KLOG_LINE_MAX ? g_klog_cur_len : KLOG_LINE_MAX;
    g_klog[g_klog_next].len = n;
    memcpy(g_klog[g_klog_next].text, g_klog_cur, n);
    g_klog_next = (g_klog_next + 1) % KLOG_LINES;
    g_klog_total++;
    g_klog_cur_len = 0;
    spin_unlock(&g_klog_lock);
    irq_restore(fl);
}

/* Capture one character into the current line, flushing to the ring on
 * newline.  Cheap (a couple of stores); only the ring flush takes the lock. */
static void klog_capture(char c) {
    if (c == '\n') { klog_flush(); return; }
    if (c == '\r') return;
    if (g_klog_cur_len < KLOG_LINE_MAX - 1) g_klog_cur[g_klog_cur_len++] = c;
}

int klog_lines_total(void) {
    u64 fl = irq_save();
    spin_lock(&g_klog_lock);
    u64 t = g_klog_total;
    spin_unlock(&g_klog_lock);
    irq_restore(fl);
    return (int)t;
}

/* Copy up to max_lines lines starting at logical line `start` into a kernel
 * buffer.  Each line is written as NUL-terminated text (with its content; a
 * trailing newline is the caller's choice).  Returns the number of lines
 * copied.  Used by the SYS_DMESG syscall; the caller is responsible for the
 * user-range being large enough (it passes max_lines * KLOG_LINE_MAX). */
int klog_read(char *dst, int start, int max_lines) {
    if (max_lines <= 0) return 0;
    const int stride = KLOG_LINE_MAX + 1;   /* fixed per-line stride so the
                                               caller can index lines directly */
    int copied = 0;
    u64 fl = irq_save();
    spin_lock(&g_klog_lock);
    u64 total = g_klog_total;
    u64 oldest = (total > KLOG_LINES) ? total - KLOG_LINES : 0;
    for (int i = 0; i < max_lines; i++) {
        u64 L = (u64)start + (u64)i;
        if (L >= total) break;
        if (L < oldest) continue;              /* too old, wrapped out */
        u32 slot = (u32)(L % KLOG_LINES);
        u16 n = g_klog[slot].len;
        char *out = dst + (u64)i * stride;
        memcpy(out, g_klog[slot].text, n);
        out[n] = 0;
        copied++;
    }
    spin_unlock(&g_klog_lock);
    irq_restore(fl);
    return copied;
}

/* Legacy FB text cursor state — kept for future emergency kconsole; the ring-3
 * compositor owns the screen during normal operation so kputc is serial-only. */
static int tcx = 8, tcy = 8;
static bool fb_ready = false;
static void mark_fb_text_vars_used(void) { (void)tcx; (void)tcy; (void)fb_ready; }

void kputc(char c) {
    serial_putc(c);
    mark_fb_text_vars_used();
    /* Framebuffer text output disabled: ring-3 compositor owns the screen. */
    return;
}

void kputs(const char *s) {
    u64 fl = console_acquire();
    for (const char *p = s; *p; p++) {
        kputc(*p);
        klog_capture(*p);          /* capture into the audit/dmesg ring too */
    }
    console_release(fl);
}

int kprintf(const char *fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    kputs(buf);
    return n;
}

NORETURN void kpanic(const char *fmt, ...) {
    cli();
    char buf[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

    serial_puts("\n[YART PANIC] ");
    serial_puts(buf);
    serial_puts("\n");

    if (g_fb.fb) {
        fb_clear(0xFF200000);
        fb_present();
    }
    for (;;) hlt();
}
