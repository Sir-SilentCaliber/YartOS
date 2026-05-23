; Yart OS - tiny ring-3 "init" task.
;
; Built as a flat binary; the Yart kernel ELF loader will memcpy it to a
; user page mapped with PTE_US|PTE_RW.  Then iretq into user mode at the
; entry below.  Uses the int 0x80 syscall ABI defined in
; kernel/arch/x86_64/syscall.c:
;
;     rax = number      rdi = arg1     rsi = arg2     rdx = arg3
;       1 = write(buf, len)
;       2 = getpid()
;       3 = exit(status)
;       4 = time()
;
; Build (once toolchain is in PATH):
;     nasm -f bin userland/init.asm -o initrd_root/bin/init
;
bits 64
org  0x0000000040000000

_start:
    ; write("hello from ring 3\n", 18)
    mov rax, 1
    lea rdi, [rel msg]
    mov rsi, msglen
    int 0x80

    ; pid = getpid(); (just for show)
    mov rax, 2
    int 0x80

    ; exit(42)
    mov rax, 3
    mov rdi, 42
    int 0x80

.spin:  jmp .spin

msg:    db "hello from ring 3, this is yart-init", 10
msglen  equ $ - msg
