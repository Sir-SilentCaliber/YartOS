# A genuine Linux x86_64 STATIC binary exercising the NEW Linux-ABI surface:
#   - clone (threads, shared memory) + futex (wait/wake rendezvous)
#   - UDP loopback (socket/bind/sendto/recvfrom)
#   - execve (replaces self with /bin/test_echo)
    .text
    .globl _start

# ---- print str (rdi=ptr, rsi=len) via write(1,ptr,len) ----
    .type emit, @function
emit:
    movl  $1, %eax
    movq  %rsi, %rdx            # count = len
    movq  %rdi, %rsi            # buf = ptr
    movl  $1, %edi              # fd = 1
    syscall
    ret

_start:
    # ============ PART 1: clone + futex ============
    # spawn thread A: clone(CLONE_VM|CLONE_FS|CLONE_FILES|CLONE_SIGHAND|CLONE_THREAD,
    #                        stackA_top, 0, 0, 0)
    movl  $56, %eax
    movl  $0x10D00, %edi        # flags
    leaq  stackA_top(%rip), %rsi
    xorl  %edx, %edx            # parent_tidptr
    xorl  %r10d, %r10d          # child_tidptr
    xorl  %r8d, %r8d            # tls
    syscall
    testq %rax, %rax
    jz    .LthreadA             # child: rax==0

    # parent: print "clone: spawned\n"
    leaq  m_spawn(%rip), %rdi
    movl  $m_spawn_len, %esi
    call  emit

    # parent: spin until shared == 42 (thread A wrote it)
.Lwait_shared:
    movl  shared(%rip), %ecx
    cmpl  $42, %ecx
    jne   .Lwait_shared
    leaq  m_shared(%rip), %rdi
    movl  $m_shared_len, %esi
    call  emit

    # parent: gate = 1 ; futex_wake(&gate, 1)
    movl  $1, gate(%rip)
    movl  $202, %eax
    leaq  gate(%rip), %rdi
    movl  $1, %esi              # FUTEX_WAKE
    movl  $1, %edx
    syscall

    # parent: spin until done == 1 (thread A finished)
.Lwait_done:
    movl  done(%rip), %ecx
    cmpl  $1, %ecx
    jne   .Lwait_done
    leaq  m_done(%rip), %rdi
    movl  $m_done_len, %esi
    call  emit

    # ============ PART 2: UDP loopback ============
    # fd = socket(AF_INET=2, SOCK_DGRAM=2, 0)
    movl  $41, %eax
    movl  $2, %edi
    movl  $2, %esi
    xorl  %edx, %edx
    syscall
    movq  %rax, %r12            # fd

    # bind(fd, &lo_addr, 8)  (127.0.0.1:9999)
    movl  $49, %eax
    movq  %r12, %rdi
    leaq  lo_addr(%rip), %rsi
    movl  $8, %edx
    syscall

    # sendto(fd, msg, len, 0, &lo_addr, 8)
    movl  $44, %eax
    movq  %r12, %rdi
    leaq  udpmsg(%rip), %rsi
    movl  $udpmsg_len, %edx
    xorl  %r10d, %r10d
    leaq  lo_addr(%rip), %r8
    movl  $8, %r9d
    syscall

    # recvfrom(fd, rbuf, 64, 0, NULL, NULL); then write it out
    movl  $45, %eax
    movq  %r12, %rdi
    leaq  rbuf(%rip), %rsi
    movl  $64, %edx
    xorl  %r10d, %r10d
    xorl  %r8d, %r8d
    xorl  %r9d, %r9d
    syscall
    # rax = bytes received; print them
    leaq  rbuf(%rip), %rdi
    movq  %rax, %rsi
    call  emit

    # close(fd)
    movl  $3, %eax
    movq  %r12, %rdi
    syscall

    # ============ PART 3: execve ============
    # execve("/bin/test_echo", ["test_echo", 0], [0])
    movl  $59, %eax
    leaq  echo_path(%rip), %rdi
    leaq  echo_argv(%rip), %rsi
    leaq  echo_envp(%rip), %rdx
    syscall

    # execve failed: print failure and exit 1
    leaq  m_execfail(%rip), %rdi
    movl  $m_execfail_len, %esi
    call  emit
    movl  $60, %eax
    movl  $1, %edi
    syscall

# ---- thread A: runs on stackA, writes shared, futex_waits, prints, exits ----
.LthreadA:
    movl  $42, shared(%rip)
    # futex_wait(&gate, 0) in a re-check loop
.Ltwait:
    movl  gate(%rip), %ecx
    cmpl  $0, %ecx
    jne   .Ltgothrough
    movl  $202, %eax
    leaq  gate(%rip), %rdi
    movl  $0, %esi              # FUTEX_WAIT
    movl  $0, %edx              # expect 0
    syscall
    jmp   .Ltwait
.Ltgothrough:
    leaq  m_thread(%rip), %rdi
    movl  $m_thread_len, %esi
    call  emit
    movl  $1, done(%rip)
    movl  $60, %eax
    xorl  %edi, %edi
    syscall

    .section .rodata
m_spawn:   .ascii "clone: spawned\n"
m_spawn_len = . - m_spawn
m_shared:  .ascii "clone: shared memory ok\n"
m_shared_len = . - m_shared
m_done:    .ascii "futex: rendezvous ok\n"
m_done_len = . - m_done
m_thread:  .ascii "thread: woken via futex\n"
m_thread_len = . - m_thread
udpmsg:    .ascii "udp loopback ok\n"
udpmsg_len = . - udpmsg
m_execfail:.ascii "execve FAILED\n"
m_execfail_len = . - m_execfail
echo_path: .ascii "/bin/test_echo\0"

    .section .data
lo_addr:   .byte 2,0, 0x27,0x0F, 0x7F,0,0,1     # 127.0.0.1:9999
shared:    .long 0
gate:      .long 0
done:      .long 0
echo_argv: .quad echo_path, 0
echo_envp: .quad 0

    .section .bss
    .align 16
rbuf:      .skip 64
    .align 16
stackA:    .skip 8192
stackA_top:
