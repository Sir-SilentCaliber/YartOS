# test_tls — verifies TLS (arch_prctl ARCH_SET_FS -> %fs base) end-to-end.
#   1. mmap a page (RWX), write 0xDEADBEEF at offset 0.
#   2. arch_prctl(ARCH_SET_FS=0x1002, page)  -> sets the thread's %fs base.
#   3. read %fs:0 and compare against 0xDEADBEEF, print TLS OK / TLS FAIL.
    .text
    .globl _start
_start:
    movl  $9, %eax
    xorl  %edi, %edi
    movl  $4096, %esi
    movl  $7, %edx
    movl  $0x22, %r10d
    movq  $-1, %r8
    xorl  %r9d, %r9d
    syscall
    movq  %rax, %rbx            # page

    movq  $0xDEADBEEF, %rcx
    movq  %rcx, (%rbx)          # page[0] = 0xDEADBEEF

    movl  $158, %eax
    movl  $0x1002, %edi         # ARCH_SET_FS
    movq  %rbx, %rsi
    syscall

    movq  %fs:0, %rax           # read back through %fs
    movq  $0xDEADBEEF, %rcx     # re-load: `syscall` clobbered %rcx
    cmpq  %rcx, %rax
    je    .Lok
    leaq  m_fail(%rip), %rsi
    movl  $m_fail_len, %edx
    jmp   .Lout
.Lok:
    leaq  m_ok(%rip), %rsi
    movl  $m_ok_len, %edx
.Lout:
    movl  $1, %eax
    movl  $1, %edi
    syscall
    movl  $60, %eax
    xorl  %edi, %edi
    syscall

    .section .rodata
m_ok:    .ascii "TLS OK: fs base applied\n"
m_ok_len = . - m_ok
m_fail:  .ascii "TLS FAIL\n"
m_fail_len = . - m_fail
