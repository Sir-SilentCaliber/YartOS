/* libtls.so — exports a function that reads/writes a __thread variable. */
static long write(long fd, const void *b, unsigned long n) {
    long r; __asm__ volatile("syscall" : "=a"(r) : "a"(1), "D"(fd), "S"(b), "d"(n) : "rcx","r11","memory"); return r;
}
__thread int tls_counter = 100;

long tls_bump(void) { return ++tls_counter; }
long tls_peek(void)  { return tls_counter; }
