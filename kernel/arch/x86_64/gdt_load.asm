; Yart OS - load GDTR + reload all segment selectors, then load TR.
bits 64
section .text
global gdt_flush
global tss_flush

gdt_flush:
    lgdt [rdi]
    mov  ax, 0x10        ; kernel data
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax
    mov  ss, ax
    pop  rdi             ; return address
    mov  rax, 0x08       ; kernel code
    push rax
    push rdi
    o64 retf

tss_flush:
    mov ax, di
    ltr ax
    ret
