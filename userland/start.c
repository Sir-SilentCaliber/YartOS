/* Yart userland crt0: the real process entry point.
 *
 * The kernel's exec path places a SysV process image on the stack:
 *   [rsp+0]  = argc
 *   [rsp+8]  = argv[0..argc-1], NULL
 *   [rsp+8+(argc+1)*8] = envp[0..], NULL
 * Exactly like a real OS, so a standard _start can pick them up and call
 * main_entry(argc, argv, envp), then exit with main's return value. */
__attribute__((naked, noreturn)) void _start(void) {
    __asm__ volatile(
        "movq (%%rsp), %%rdi\n\t"          /* argc                        */
        "leaq 8(%%rsp), %%rsi\n\t"         /* argv                        */
        "leaq 16(%%rsp,%%rdi,8), %%rdx\n\t"/* envp = argv + (argc+1)*8    */
        "andq $-16, %%rsp\n\t"             /* align per SysV ABI          */
        "call main_entry\n\t"              /* int main_entry(argc,argv,envp) */
        "movq %%rax, %%rdi\n\t"            /* exit(main's return value)   */
        "movq $0, %%rax\n\t"               /* SYS_EXIT                    */
        "syscall\n\t"
        "1: jmp 1b\n\t"
        :
        :
        : "memory");
}
