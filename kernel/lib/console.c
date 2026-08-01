/* Yart OS - kernel console (serial + framebuffer text) */
#include <yart/console.h>
#include <yart/io.h>
#include <yart/string.h>
#include <yart/gui.h>
#include <yart/theme.h>
#include <yart/spinlock.h>
#include <stdarg.h>

#define COM1 0x3f8

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
    while (*s) serial_putc(*s++);
    console_release(fl);
}

static int tcx = 8, tcy = 8;
static bool fb_ready = false;

void kputc(char c) {
    serial_putc(c);
    if (!fb_ready && g_fb.fb) fb_ready = true;
    if (!fb_ready) return;
    if (c == '\n') { tcx = 8; tcy += FONT_H; }
    else if (c == '\r') { tcx = 8; }
    else { draw_char(tcx, tcy, c, TH_TEXT, TH_DESKTOP_BOT); tcx += FONT_W; }
    if (tcx + FONT_W >= (int)g_fb.width)  { tcx = 8; tcy += FONT_H; }
    if (tcy + FONT_H >= (int)g_fb.height) {
        fb_clear(TH_DESKTOP_BOT); tcx = 8; tcy = 8;
    }
}

void kputs(const char *s) {
    u64 fl = console_acquire();
    while (*s) kputc(*s++);
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
        fb_clear(TH_DESKTOP_BOT);
        draw_rect(0, 0, g_fb.width, 80, TH_PANEL);
        draw_text(20, 24, "YART KERNEL PANIC", TH_ERR, 0);
        draw_text(20, 100, buf, TH_TEXT, TH_DESKTOP_BOT);
        draw_text(20, 130, "System halted. See serial log for trace.",
                  TH_TEXT_DIM, TH_DESKTOP_BOT);
        fb_present();
    }
    for (;;) hlt();
}
