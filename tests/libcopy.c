/* libcopy.so — references a global defined in the MAIN program, forcing a
 * R_X86_64_COPY relocation. */
static long write(long fd, const void *b, unsigned long n) {
    long r; __asm__ volatile("syscall" : "=a"(r) : "a"(1), "D"(fd), "S"(b), "d"(n) : "rcx","r11","memory"); return r;
}
extern int main_global;      /* defined in the executable */
int read_global(void) { return main_global; }
long bump_global(void) { main_global += 1; return main_global; }
