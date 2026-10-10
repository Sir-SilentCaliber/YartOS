/* Yart OS - tiny freestanding string + printf */
#include <yart/string.h>
#include <stdarg.h>

void *memset(void *dst, int c, size_t n) {
    u8 *d = dst;
    while (n--) *d++ = (u8)c;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n) {
    u8 *d = dst; const u8 *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    u8 *d = dst; const u8 *s = src;
    if (d == s || n == 0) return dst;
    if (d < s) { while (n--) *d++ = *s++; }
    else { d += n; s += n; while (n--) *--d = *--s; }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const u8 *x = a, *y = b;
    while (n--) { if (*x != *y) return *x - *y; x++; y++; }
    return 0;
}

size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (u8)*a - (u8)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    while (n--) {
        if (*a != *b) return (u8)*a - (u8)*b;
        if (!*a) return 0;
        a++; b++;
    }
    return 0;
}

char *strncpy(char *d, const char *s, size_t n) {
    size_t i; for (i = 0; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}

char *strchr(const char *s, int c) {
    while (*s) { if (*s == (char)c) return (char *)s; s++; }
    return c == 0 ? (char *)s : 0;
}

/* ------------ tiny printf ------------ */

static int put_pad(char *buf, size_t cap, int *pos, char c, int n) {
    int w = 0;
    while (n-- > 0) {
        if ((size_t)*pos + 1 < cap) buf[(*pos)++] = c;
        else if (cap) buf[cap - 1] = 0;
        w++;
    }
    return w;
}

static int put_str(char *buf, size_t cap, int *pos, const char *s) {
    int w = 0;
    while (*s) {
        if ((size_t)*pos + 1 < cap) buf[(*pos)++] = *s;
        s++; w++;
    }
    return w;
}

static int put_num(char *buf, size_t cap, int *pos,
                   u64 val, int base, bool sign, bool upper,
                   int width, char pad) {
    char tmp[32]; int n = 0;
    const char *digs = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    bool neg = false;
    if (sign && (i64)val < 0) { neg = true; val = (u64)(-(i64)val); }
    if (val == 0) tmp[n++] = '0';
    while (val) { tmp[n++] = digs[val % base]; val /= base; }
    int len = n + (neg ? 1 : 0);
    int w = 0;
    if (pad == ' ') w += put_pad(buf, cap, pos, ' ', width - len);
    if (neg)        w += put_str(buf, cap, pos, "-");
    if (pad == '0') w += put_pad(buf, cap, pos, '0', width - len);
    while (n--) {
        if ((size_t)*pos + 1 < cap) buf[(*pos)++] = tmp[n];
        w++;
    }
    return w;
}

int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap) {
    int pos = 0;
    while (*fmt) {
        if (*fmt != '%') {
            if ((size_t)pos + 1 < cap) buf[pos++] = *fmt;
            fmt++; continue;
        }
        fmt++;
        char pad = ' ';
        int  width = 0;
        bool islong = false;
        if (*fmt == '0') { pad = '0'; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }
        if (*fmt == 'l') { islong = true; fmt++; if (*fmt == 'l') fmt++; }
        if (*fmt == 'z') { islong = true; fmt++; }

        switch (*fmt) {
        case 'c': {
            char c = (char)va_arg(ap, int);
            if ((size_t)pos + 1 < cap) buf[pos++] = c;
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            put_str(buf, cap, &pos, s);
            break;
        }
        case 'd': case 'i': {
            i64 v = islong ? va_arg(ap, i64) : va_arg(ap, int);
            put_num(buf, cap, &pos, (u64)v, 10, true, false, width, pad);
            break;
        }
        case 'u': {
            u64 v = islong ? va_arg(ap, u64) : va_arg(ap, unsigned);
            put_num(buf, cap, &pos, v, 10, false, false, width, pad);
            break;
        }
        case 'x': {
            u64 v = islong ? va_arg(ap, u64) : va_arg(ap, unsigned);
            put_num(buf, cap, &pos, v, 16, false, false, width, pad);
            break;
        }
        case 'X': {
            u64 v = islong ? va_arg(ap, u64) : va_arg(ap, unsigned);
            put_num(buf, cap, &pos, v, 16, false, true, width, pad);
            break;
        }
        case 'p': {
            u64 v = (u64)va_arg(ap, void *);
            put_str(buf, cap, &pos, "0x");
            put_num(buf, cap, &pos, v, 16, false, false, 16, '0');
            break;
        }
        case '%':
            if ((size_t)pos + 1 < cap) buf[pos++] = '%';
            break;
        default:
            if ((size_t)pos + 1 < cap) buf[pos++] = '%';
            if ((size_t)pos + 1 < cap) buf[pos++] = *fmt;
            break;
        }
        fmt++;
    }
    if (cap) buf[pos < (int)cap ? pos : (int)cap - 1] = 0;
    return pos;
}

int snprintf(char *buf, size_t cap, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return n;
}
