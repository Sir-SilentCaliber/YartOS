#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;
typedef int64_t  i64;

typedef uint64_t paddr_t;
typedef uint64_t vaddr_t;

#define PAGE_SIZE       0x1000UL
#define PAGE_MASK       (PAGE_SIZE - 1)
#define PAGE_ALIGN_DOWN(x) ((x) & ~PAGE_MASK)
#define PAGE_ALIGN_UP(x)   (((x) + PAGE_MASK) & ~PAGE_MASK)

#define KB(x) ((x) * 1024UL)
#define MB(x) ((x) * 1024UL * 1024UL)
#define GB(x) ((x) * 1024UL * 1024UL * 1024UL)

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define UNUSED(x)     ((void)(x))
#define ALWAYS_INLINE __attribute__((always_inline)) inline
#define PACKED        __attribute__((packed))
#define NORETURN      __attribute__((noreturn))
#define ALIGNED(n)    __attribute__((aligned(n)))
#define SECTION(s)    __attribute__((section(s)))

#define MIN(a,b) ({ typeof(a) _a=(a); typeof(b) _b=(b); _a<_b?_a:_b; })
#define MAX(a,b) ({ typeof(a) _a=(a); typeof(b) _b=(b); _a>_b?_a:_b; })

#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))
