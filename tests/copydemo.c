/* copydemo — dynamically-linked; defines main_global that libcopy.so copies. */
static void write(long fd, const void *b, unsigned long n) {
    long r; __asm__ volatile("syscall" : "=a"(r) : "a"(1), "D"(fd), "S"(b), "d"(n) : "rcx","r11","memory"); (void)r;
}
static void exit_group(long c) { __asm__ volatile("syscall" : : "a"(231), "D"(c) : "rcx","r11","memory"); __builtin_unreachable(); }
extern long bump_global(void);
int main_global = 5;
void _start(void) {
    long a = bump_global();   /* 6 */
    long b = bump_global();   /* 7 */
    char d1 = (char)('0' + (a % 10));
    char d2 = (char)('0' + (b % 10));
    write(1, "copy global bump=", 17); write(1, &d1, 1);
    write(1, " bump=", 6); write(1, &d2, 1); write(1, "\n", 1);
    exit_group(0);
}
