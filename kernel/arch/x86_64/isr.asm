; Yart OS - 256 ISR stubs that all converge on isr_common -> isr_dispatch(C)
bits 64
section .text
extern isr_dispatch

%macro PUSHALL 0
    push rax
    push rbx
    push rcx
    push rdx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
%endmacro

%macro POPALL 0
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rdx
    pop rcx
    pop rbx
    pop rax
%endmacro

isr_common:
    PUSHALL
    cld
    mov rdi, rsp        ; cpu_regs_t *
    call isr_dispatch
    POPALL
    add rsp, 16         ; vector + err
    iretq

; vectors that push their own error code on the CPU stack
%define HAS_ERR(v) (v == 8 || (v >= 10 && v <= 14) || v == 17 || v == 21 || v == 29 || v == 30)

%macro ISR_NOERR 1
isr%1:
    push qword 0
    push qword %1
    jmp  isr_common
%endmacro

%macro ISR_ERR 1
isr%1:
    push qword %1
    jmp  isr_common
%endmacro

; ----- generate 256 stubs -----
%assign i 0
%rep 256
    %if HAS_ERR(i)
        ISR_ERR i
    %else
        ISR_NOERR i
    %endif
    %assign i i+1
%endrep

; ----- table of pointers -----
section .data
global isr_stub_table
isr_stub_table:
%assign i 0
%rep 256
    dq isr %+ i
    %assign i i+1
%endrep
