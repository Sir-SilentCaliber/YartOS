/* dynhello — a DYNAMICALLY-LINKED Linux program (the demo).
 * References greet() from libgreet.so through the PLT/GOT; the dynamic
 * linker (ld-yart.so) resolves it at runtime.  This proves the whole
 * PT_INTERP -> ld-yart -> DT_NEEDED -> relocation -> entry chain works. */
extern long greet(const char *who);

static void write(long fd, const void *b, unsigned long n) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(1), "D"(fd), "S"(b), "d"(n) : "rcx", "r11", "memory");
    (void)r;
}
static void exit_group(long code) {
    __asm__ volatile("syscall" : : "a"(231), "D"(code) : "rcx", "r11", "memory");
    __builtin_unreachable();
}

void _start(void) {
    write(1, "dynhello: calling greet() from a .so\n", 38);
    long r = greet("world");
    /* print the return value's digit (42 -> '4') to prove the call returned */
    char d = (char)('0' + (r % 10));
    write(1, "dynhello: greet returned ", 26);
    write(1, &d, 1);
    write(1, "\n", 1);
    exit_group(0);
}
