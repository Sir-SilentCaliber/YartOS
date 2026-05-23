#pragma once
#include <yart/types.h>

void serial_init(void);
void serial_putc(char c);
void serial_puts(const char *s);

void kputc(char c);
void kputs(const char *s);
int  kprintf(const char *fmt, ...);

NORETURN void kpanic(const char *fmt, ...);
