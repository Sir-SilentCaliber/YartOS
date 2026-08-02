; =============================================================================
;  Yart OS - fast syscall/sysret entry (the `syscall` instruction).
;
;  Entered via MSR_LSTAR whenever a ring-3 task executes `syscall`.
;  The CPU (with EFER.SCE set):
;     * saves the return address in RCX
;     * saves the old RFLAGS in R11
;     * clears IF/DF/etc. according to MSR_SFMASK (so we run with IF=0)
;     * loads CS/SS from STAR (kernel code/data)
;     * jumps here in ring 0
;  It does NOT switch stacks and does NOT save the user RSP, so this entry
;  must do what the int 0x80 hardware does for us:
;     1) swapgs             -> GS.base = kernel per-CPU area
;     2) capture user RSP, then load THIS task's kernel stack top (RSP0)
;        from cpu_local_t::ap_krsp0 (the syscall instruction leaves RSP
;        pointing at the user stack)
;     3) build the exact same cpu_regs_t frame the int 0x80 ISR stub would
;     4) call syscall_fast_dispatch(), which runs syscall_handler() and then
;        sched_after_isr() - the scheduler may switch tasks and returns the
;        frame pointer to resume from (a different task's frame is fine)
;     5) pop that frame and sysretq back to the (possibly switched) task.
;
;  The frame layout must match cpu_regs_t in kernel/include/yart/hal.h:
;    offset 0..112 : r15..rax (register save, lowest address first)
;    120/128       : vector / err
;    136/144/152/160/168 : rip / cs / rflags / rsp / ss
;  Because push decrements RSP, the FIRST value pushed lands at the HIGHEST
;  address, so we push in the order: ss, user_rsp, rflags, cs, rip, err,
;  vector, then the PUSHALL register block (rax first .. r15 last).
; =============================================================================
bits 64
section .text

extern syscall_fast_dispatch

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

%define USER_CS     0x1B    ; slot 3 user code (matches user.h USER_CS)
%define USER_DS     0x23    ; slot 4 user data (matches user.h USER_DS)
%define RSP0_OFF    152     ; offsetof(cpu_local_t, ap_krsp0) - pinned in cpu.h
%define SCRATCH_OFF 160     ; offsetof(cpu_local_t, ap_syscall_rsp)

global syscall_entry
syscall_entry:
    cli
    swapgs                    ; GS.base -> kernel per-CPU area
    ; The syscall instruction does NOT switch stacks and every GPR belongs
    ; to the caller, so stash the user RSP in a per-CPU memory slot (no
    ; register clobbered) before moving RSP to the current task's kernel
    ; stack top (ap_krsp0).
    mov  [gs:SCRATCH_OFF], rsp
    mov  rsp, [gs:RSP0_OFF]
    ; ---- build cpu_regs_t frame ----
    push qword USER_DS        ; ss        @168
    push qword [gs:SCRATCH_OFF] ; user rsp @160
    push r11                  ; rflags    @152
    push qword USER_CS        ; cs        @144
    push rcx                  ; rip       @136
    push qword 0              ; err       @128
    push qword 0x80           ; vector    @120 (marker: fast syscall entry)
    PUSHALL                   ; rax..r15  @112..0  (rax = syscall number)
    cld
    mov  rdi, rsp             ; cpu_regs_t *
    call syscall_fast_dispatch
    ; rax = frame pointer to resume from (may be a different task's frame)
    mov  rsp, rax
    POPALL
    add  rsp, 16              ; skip vector + err; rsp -> rip
    test byte [rsp+8], 3      ; target frame CS.RPL: 3 => user, else kernel
    jnz  .user_resume
    ; ---- kernel target (e.g. the desktop / pid 0) ----
    ; sysret is only valid for a user task.  When a fast syscall switches to
    ; a kernel task we must return exactly like the ISR stub: iretq, and do
    ; NOT swapgs (kernel mode keeps GS.base = per-CPU area).  The stack is
    ; already [rip, cs, rflags(, rsp, ss)] so iretq works directly.
    cli
    iretq
.user_resume:
    mov  rcx, [rsp]           ; rip      @0   (sysret return address)
    mov  r11, rsp             ; r11 = frame ptr (temp)
    mov  rsp, [r11+24]        ; user rsp @24  (switch to the user stack)
    mov  r11, [r11+16]        ; rflags   @16  (sysret restores RFLAGS)
    cli
    swapgs                    ; GS.base -> user (0)
    o64 sysret                ; REX.W -> SYSRETQ (return to 64-bit mode).
                              ; Plain `sysret` is SYSRETL (0F 07) which would
                              ; re-enter 32-bit compat mode and hang.
