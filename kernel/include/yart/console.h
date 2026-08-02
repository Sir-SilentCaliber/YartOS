#pragma once
#include <yart/types.h>

void serial_init(void);
void serial_putc(char c);
void serial_puts(const char *s);

void kputc(char c);
void kputs(const char *s);
int  kprintf(const char *fmt, ...);

NORETURN void kpanic(const char *fmt, ...);

/* Kernel audit / dmesg log ring buffer (row 19).  Every line printed to the
 * kernel console (via kprintf/serial_puts, which carries the security events
 * like doas/setuid too) is captured into a bounded ring of lines.  A dmesg
 * syscall lets a ring-3 task read them back. */
int  klog_lines_total(void);      /* how many lines have ever been logged */
int  klog_read(char *dst, int start_line, int max_lines); /* -> lines copied */
