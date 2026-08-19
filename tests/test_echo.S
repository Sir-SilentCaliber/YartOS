# execve target: prints "EXECVE OK\n" and exits.  Proves Linux execve()
# replaced the process image (this is a SEPARATE static binary).
    .text
    .globl _start
_start:
    movl  $1, %eax
    movl  $1, %edi
    leaq  msg(%rip), %rsi
    movl  $msg_len, %edx
    syscall
    movl  $60, %eax
    xorl  %edi, %edi
    syscall

    .section .rodata
msg:     .ascii "EXECVE OK\n"
msg_len = . - msg
