/* tlsdemo — dynamically-linked program with its OWN __thread + a lib's. */
static void write(long fd, const void *b, unsigned long n) {
    long r; __asm__ volatile("syscall" : "=a"(r) : "a"(1), "D"(fd), "S"(b), "d"(n) : "rcx","r11","memory"); (void)r;
}
static void exit_group(long c) { __asm__ volatile("syscall" : : "a"(231), "D"(c) : "rcx","r11","memory"); __builtin_unreachable(); }
extern long tls_bump(void);
extern long tls_peek(void);

__thread int mine = 7;

static void putdec(long v) {
    char b[24]; int i = 0;
    if (v == 0) b[i++] = '0';
    while (v > 0 && i < 22) { b[i++] = (char)('0' + v % 10); v /= 10; }
    char o[24]; int k = 0; while (i > 0) o[k++] = b[--i]; o[k] = 0;
    write(1, o, k);
}

void _start(void) {
    long a = tls_peek();      /* 100 */
    long b = tls_bump();      /* 101 */
    long c = tls_bump();      /* 102 */
    write(1, "tls mine=", 9); putdec(mine);   /* 7 */
    write(1, " peek=", 6); putdec(a);          /* 100 */
    write(1, " bump=", 6); putdec(b);          /* 101 */
    write(1, " bump=", 6); putdec(c);          /* 102 */
    write(1, "\n", 1);
    exit_group(0);
}
