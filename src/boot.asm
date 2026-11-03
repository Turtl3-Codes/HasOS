[BITS 64]

global _start
extern kernel_main

section .text
_start:
    ; Set up 64-bit stack
    mov rsp, stack_top

    ; Call kernel main C entry point
    call kernel_main

.hang:
    hlt
    jmp .hang

section .bss
align 16
stack_bottom:
    resb 16384 ; 16 KB Kernel Stack
stack_top:
