/* Yart OS - kernel console (serial + framebuffer text) */
#include <yart/console.h>
#include <yart/io.h>
#include <yart/string.h>
#include <yart/gui.h>
#include <yart/theme.h>
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

void serial_putc(char c) {
    if (c == '\n') { while (!(inb(COM1 + 5) & 0x20)); outb(COM1, '\r'); }
    while (!(inb(COM1 + 5) & 0x20));
    outb(COM1, c);
}

void serial_puts(const char *s) { while (*s) serial_putc(*s++); }

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

void kputs(const char *s) { while (*s) kputc(*s++); }

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
