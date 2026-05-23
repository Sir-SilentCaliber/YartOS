#pragma once
#include <yart/types.h>

void   *memset(void *dst, int c, size_t n);
void   *memcpy(void *dst, const void *src, size_t n);
void   *memmove(void *dst, const void *src, size_t n);
int     memcmp(const void *a, const void *b, size_t n);
size_t  strlen(const char *s);
int     strcmp(const char *a, const char *b);
int     strncmp(const char *a, const char *b, size_t n);
char   *strncpy(char *d, const char *s, size_t n);
char   *strchr(const char *s, int c);

/* tiny printf family */
int  vsnprintf(char *buf, size_t n, const char *fmt, __builtin_va_list ap);
int  snprintf(char *buf, size_t n, const char *fmt, ...);
