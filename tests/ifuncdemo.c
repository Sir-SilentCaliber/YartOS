static void write(long fd, const void *b, unsigned long n) {
    long r; __asm__ volatile("syscall" : "=a"(r) : "a"(1), "D"(fd), "S"(b), "d"(n) : "rcx","r11","memory"); (void)r;
}
static void exit_group(long c) { __asm__ volatile("syscall" : : "a"(231), "D"(c) : "rcx","r11","memory"); __builtin_unreachable(); }
extern void hello(void);
void _start(void) {
    hello();
    write(1, "ifunc: called\n", 14);
    exit_group(0);
}
