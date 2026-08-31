; ============================================================================
; leahOS - entry stub for the dynamic linker.
;
; The kernel enters the interpreter, not the program, and hands it the stack
; the program is expecting: argc, argv, envp, and the strings. So this must do
; its work without disturbing any of it, and then jump - not call - into the
; program with the stack exactly as it was found.
;
; That is the whole reason this is assembly. Everything else ld.so does is C.
; ============================================================================

BITS 64

SECTION .text.startup
GLOBAL _start
EXTERN ld_main

_start:
    ; Terminate the call-frame chain: there is nothing above this.
    xor rbp, rbp

    ; The program's stack pointer, kept in a callee-saved register so that
    ; ld_main cannot lose it.
    mov rbx, rsp

    ; The ABI wants RSP 16-aligned at the call site. The stack the kernel
    ; built is aligned for the program's crt0, which does its own aligning,
    ; so it is not necessarily aligned for this one.
    and rsp, -16
    call ld_main                ; returns the program's entry point in rax

    ; Back to the stack the program is owed, and in we go.
    mov rsp, rbx
    jmp rax
