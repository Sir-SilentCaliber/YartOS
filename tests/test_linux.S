# A genuine Linux x86_64 STATIC binary (ET_EXEC, Linux syscall ABI).
# Exercises the YartOS Linux-ABI layer: write, openat, stat, getdents64,
# uname, getuid, writev, mmap/munmap, clock_gettime, exit.
    .text
    .globl _start

# ---- helpers ----
# print_u64: print unsigned decimal of %rdi + newline to fd 1
    .type print_u64, @function
print_u64:
    subq  $40, %rsp
    movb  $10, 32(%rsp)     # newline at buffer end
    movq  %rdi, %rax
    movq  $10, %r10
    leaq  32(%rsp), %rbx    # write position (backwards)
.Ldig:
    xorq  %rdx, %rdx
    divq  %r10
    addb  $'0', %dl
    decq  %rbx
    movb  %dl, (%rbx)
    testq %rax, %rax
    jnz   .Ldig
    movl  $1, %eax          # sys_write
    movl  $1, %edi
    movq  %rbx, %rsi
    leaq  33(%rsp), %rdx
    subq  %rbx, %rdx
    syscall
    addq  $40, %rsp
    ret

_start:
    # write(1, msg1, len1)
    movl  $1, %eax
    movl  $1, %edi
    leaq  msg1(%rip), %rsi
    movl  $msg1_len, %edx
    syscall

    # openat(AT_FDCWD=-100, "/home/yart/linux_test.txt", O_WRONLY|O_CREAT|O_TRUNC=577, 0644)
    movl  $257, %eax
    movq  $-100, %rdi
    leaq  fname(%rip), %rsi
    movl  $577, %edx
    movl  $420, %r10d
    syscall
    movq  %rax, %rbx         # fd

    # write(fd, "file io via openat\n", 19)
    movl  $1, %eax
    movq  %rbx, %rdi
    leaq  fmsg(%rip), %rsi
    movl  $fmsg_len, %edx
    syscall

    # close(fd)
    movl  $3, %eax
    movq  %rbx, %rdi
    syscall

    # stat("/home/yart/linux_test.txt", &st); print st_size
    movl  $4, %eax
    leaq  fname(%rip), %rdi
    leaq  st(%rip), %rsi
    syscall
    movq  st+48(%rip), %rdi  # st_size
    call  print_u64

    # getdents64(open("/home/yart"), buf, 4096); print entry count
    movl  $2, %eax
    leaq  dirname(%rip), %rdi
    xorl  %esi, %esi
    syscall
    movq  %rax, %rbx         # dir fd
    movl  $217, %eax
    movq  %rbx, %rdi
    leaq  dbuf(%rip), %rsi
    movl  $4096, %edx
    syscall
    # count entries: walk reclen
    movq  %rax, %rcx         # bytes returned
    xorl  %r8d, %r8d         # count
    xorl  %r9d, %r9d         # offset
.Lcount:
    cmpq  %rcx, %r9
    jae   .Ldone
    movzwl 16+dbuf(%r9), %edx   # reclen (u16 at +16)
    addq  %rdx, %r9
    incl  %r8d
    jmp   .Lcount
.Ldone:
    movq  %r8, %rdi
    call  print_u64
    movl  $3, %eax
    movq  %rbx, %rdi
    syscall

    # uname(&uts); print sysname
    movl  $63, %eax
    leaq  uts(%rip), %rdi
    syscall
    movl  $1, %eax
    movl  $1, %edi
    leaq  uts(%rip), %rsi
    movl  $7, %edx
    syscall
    movl  $1, %eax
    movl  $1, %edi
    leaq  nl(%rip), %rsi
    movl  $1, %edx
    syscall

    # getuid -> print
    movl  $102, %eax
    syscall
    movq  %rax, %rdi
    call  print_u64

    # writev(fd=1, 2 iovecs) -> "WV:" + "ok\n"
    movl  $20, %eax
    movl  $1, %edi
    leaq  iov(%rip), %rsi
    movl  $2, %edx
    syscall

    # mmap(NULL, 4096, RW, ANON|PRIVATE, -1, 0) -> write to it -> write out -> munmap
    movl  $9, %eax
    xorl  %edi, %edi
    movl  $4096, %esi
    movl  $3, %edx           # PROT_READ|PROT_WRITE
    movl  $0x22, %r10d       # MAP_PRIVATE|MAP_ANONYMOUS
    movq  $-1, %r8
    xorl  %r9d, %r9d
    syscall
    movq  %rax, %r12         # mapping
    movq  $0x6d6d6170206f6b0a, %rcx  # "mmap ok\n"
    movq  %rcx, (%r12)
    movl  $1, %eax
    movl  $1, %edi
    movq  %r12, %rsi
    movl  $8, %edx
    syscall
    movl  $11, %eax
    movq  %r12, %rdi
    syscall

    # clock_gettime(CLOCK_REALTIME, &ts); print seconds
    movl  $228, %eax
    xorl  %edi, %edi
    leaq  ts(%rip), %rsi
    syscall
    movq  ts(%rip), %rdi
    call  print_u64

    # exit(0)
    movl  $60, %eax
    xorl  %edi, %edi
    syscall

    .section .rodata
msg1:     .ascii "LXTEST: linux abi suite\n"
msg1_len = . - msg1
fname:    .ascii "/home/yart/linux_test.txt\0"
fmsg:     .ascii "file io via openat\n"
fmsg_len = . - fmsg
dirname:  .ascii "/home/yart\0"
nl:       .ascii "\n"
wv1:      .ascii "WV:"
wv2:      .ascii "ok\n"

    .section .data
iov:      .quad wv1, 3, wv2, 3

    .section .bss
st:       .skip 144
dbuf:     .skip 4096
uts:      .skip 390
ts:       .skip 16
