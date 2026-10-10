; Yart OS - 256 ISR stubs that all converge on isr_common -> isr_dispatch(C)
;
; Privilege handling: if the interrupted context was ring-3 (CS.RPL == 3),
; swapgs so GS.base points at the kernel per-CPU area.  The reverse swapgs
; happens on exit, with interrupts masked for the single-instruction window.
;
; Stack layout at isr_common entry (same for NOERR and ERR stubs):
;   [rsp+0]=vector  [rsp+8]=err  [rsp+16]=RIP  [rsp+24]=CS
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
    test byte [rsp+24], 3       ; CS.RPL: 3 => came from ring 3
    jz   .from_kernel
    swapgs
.from_kernel:
    PUSHALL
    cld
    mov rdi, rsp                ; cpu_regs_t *
    call isr_dispatch
    mov rsp, rax                ; scheduler may have switched stacks: the
                                ; returned rsp is the frame to resume from
    POPALL
    add rsp, 16                 ; vector + err
    cli                         ; close the swapgs/iretq race for IRQs
    test byte [rsp+8], 3        ; CS.RPL after popping vector+err
    jz   .to_kernel
    ; Resuming to ring 3: FORCE the frame's SS to USER_DS (0x23).  A frame
    ; whose SS.RPL is 0 (0x20) iretq's to user with SS.RPL < CPL and #GPs
    ; with err = the SS selector (0x20) - a real crash observed on some
    ; CPUs/environments where SYSRET leaves the user SS as 0x20.  The
    ; frame layout here is [rip+0, cs+8, rflags+16, rsp+24, ss+32].
    mov qword [rsp+32], 0x23
    swapgs
.to_kernel:
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
