global gdt_flush
global tss_flush

section .text
bits 64

gdt_flush:
    lgdt [rdi]         ; Load GDT pointer passed in RDI

    ; Reload CS via far return
    push 0x08          ; Kernel Code Segment
    lea rax, [rel .reload_cs]
    push rax
    db 0x48, 0xcb      ; retfq (64-bit far return)

.reload_cs:
    ; Reload data segment registers
    mov ax, 0x10       ; Kernel Data Segment
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    ret

tss_flush:
    mov ax, 0x28       ; TSS Segment Selector (Offset 0x28 in GDT)
    ltr ax             ; Load Task Register
    ret
