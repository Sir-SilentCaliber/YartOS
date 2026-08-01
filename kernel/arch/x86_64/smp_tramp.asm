; Yart OS - SMP AP trampoline (position-independent binary blob).
;
; Copied to a page below 1 MiB (0x8000) by smp_start_aps().  The AP starts
; in 16-bit real mode at offset 0 (the entry vector), switches to protected
; mode, PAE + long mode with the kernel's PML4, loads a per-AP kernel stack,
; and calls ap_c_entry(cpu_id) in the high half.
;
; The patchable fields live at FIXED offsets (data after the code):
;     +0x80 gdt_ptr   (6 bytes: limit + 32-bit base)
;     +0x88 pml4_phys (u64)
;     +0x90 stack_top (u64)
;     +0x98 c_entry   (u64)
;     +0xA0 cpu_id    (u32)
;
bits 16
section .text
global smp_trampoline_start
global smp_trampoline_end

smp_trampoline_start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax

    ; marker: write 0xAA to 0x5000 (BSP polls this to confirm AP started)
    mov ax, 0x5000
    mov es, ax
    mov byte [es:0], 0xAA

    lgdt [cs:0x100]

    mov eax, cr0
    or eax, 1
    mov cr0, eax            ; protected mode

    jmp 0x08:.pmode

bits 32
.pmode:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax

    mov eax, cr4
    or eax, 1 << 5          ; PAE
    mov cr4, eax

    mov eax, [0x8108]        ; pml4 phys (low 32 bits; < 4G in QEMU)
    mov cr3, eax

    mov ecx, 0xC0000080     ; EFER
    rdmsr
    or eax, 1 << 8          ; LME
    wrmsr

    mov eax, cr0
    or eax, 0x80000001      ; PG + PE
    mov cr0, eax

    lgdt [0x8100]

    jmp 0x08:.lmode

bits 64
.lmode:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax

    mov rsp, [0x8110]        ; stack top (u64, high-half)
    mov rdi, [0x8120]        ; cpu_id (u32)

    mov rax, [0x8118]        ; c_entry
    call rax

.halt:
    cli
    hlt
    jmp .halt

times 0x100 - ($ - smp_trampoline_start) db 0

; ---------- patchable data block (fixed at +0x100) ----------
smp_gdt_ptr:    dq 0        ; +0x100 (limit + base low32)
smp_pml4_ptr:   dq 0        ; +0x108
smp_stack_ptr:  dq 0        ; +0x110
smp_c_entry_ptr: dq 0       ; +0x118
smp_cpu_id_ptr: dd 0        ; +0x120
                dd 0

smp_trampoline_end:
