/* libgreet.so — a shared library for the dynamic-linking demo.
 * Exports greet() which writes a string to fd 1 (raw Linux syscall).
 * Deliberately relocation-free (pure PIC code, no globals) so the demo
 * exercises exactly the JUMP_SLOT resolution in the main program. */
static long write(long fd, const void *b, unsigned long n) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(1), "D"(fd), "S"(b), "d"(n) : "rcx", "r11", "memory");
    return r;
}
static unsigned long strlen(const char *s) { unsigned long n = 0; while (s[n]) n++; return n; }

long greet(const char *who) {
    write(1, "libgreet: hello, ", 17);
    write(1, who, strlen(who));
    write(1, "\n", 1);
    return 42;
}
