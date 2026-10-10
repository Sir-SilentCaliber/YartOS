/* libifunc.so — exports an IFUNC (indirect function) symbol. */
static long write(long fd, const void *b, unsigned long n) {
    long r; __asm__ volatile("syscall" : "=a"(r) : "a"(1), "D"(fd), "S"(b), "d"(n) : "rcx","r11","memory"); return r;
}
static void hello_fast(void) { write(1, "ifunc: fast path\n", 17); }
static void *resolve_hello(void) { return hello_fast; }
void hello(void) __attribute__((ifunc("resolve_hello")));
