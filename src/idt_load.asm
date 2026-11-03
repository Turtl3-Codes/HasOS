; Cleared to prevent duplicate definition of idt_load with isr_stubs.asm
global idt_load_dummy

section .text
bits 64

idt_load_dummy:
    ret
