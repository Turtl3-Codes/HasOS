global kernel_ctx_save
global kernel_ctx_restore

section .bss
align 16
kernel_ctx_rsp:    resq 1
kernel_ctx_rbp:    resq 1
kernel_ctx_rbx:    resq 1
kernel_ctx_r12:    resq 1
kernel_ctx_r13:    resq 1
kernel_ctx_r14:    resq 1
kernel_ctx_r15:    resq 1
kernel_ctx_rip:    resq 1
kernel_ctx_rflags: resq 1

section .text
bits 64

; int kernel_ctx_save(void)
kernel_ctx_save:
    mov rax, [rsp]              ; caller's return address (do not pop)
    mov [kernel_ctx_rip], rax
    mov [kernel_ctx_rsp], rsp
    mov [kernel_ctx_rbp], rbp
    mov [kernel_ctx_rbx], rbx
    mov [kernel_ctx_r12], r12
    mov [kernel_ctx_r13], r13
    mov [kernel_ctx_r14], r14
    mov [kernel_ctx_r15], r15

    pushfq
    pop rax
    mov [kernel_ctx_rflags], rax

    xor eax, eax
    ret                          ; normal return, caller sees 0

; void kernel_ctx_restore(void)
kernel_ctx_restore:
    mov rsp, [kernel_ctx_rsp]
    add rsp, 8
    mov rbp, [kernel_ctx_rbp]
    mov rbx, [kernel_ctx_rbx]
    mov r12, [kernel_ctx_r12]
    mov r13, [kernel_ctx_r13]
    mov r14, [kernel_ctx_r14]
    mov r15, [kernel_ctx_r15]

    mov rax, [kernel_ctx_rflags]
    push rax
    popfq                        ; restores IF (interrupts re-enabled)

    mov eax, 1                   ; kernel_ctx_save()'s caller sees 1
    mov rcx, [kernel_ctx_rip]
    jmp rcx
