global enter_usermode

section .text
bits 64

; void enter_usermode(uint64_t entry_point, uint64_t user_rsp)
; RDI = entry_point
; RSI = user_rsp
enter_usermode:
    cli                         ; Disable interrupts during transition

    mov ax, 0x23                ; User Data Segment (Selector 0x20 | RPL 3)
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; Build 64-bit iretq frame: SS, RSP, RFLAGS, CS, RIP
    push 0x23                   ; User SS (0x20 | 3)
    push rsi                    ; User RSP
    push 0x202                  ; RFLAGS (IF=1, IOPL=0)
    push 0x1B                   ; User CS (0x18 | 3)
    push rdi                    ; User RIP

    iretq                       ; Transition to Ring 3
