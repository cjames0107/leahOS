; ============================================================================
; leahOS - a minimal ring 3 program used to exercise the kernel's user support.
;
; Runs at CPL 3 in its own pages. It cannot touch hardware or kernel memory;
; the only way back into the kernel is the SYSCALL instruction. It writes a
; line to stdout and then exits with a code the kernel checks.
;
; The exit code is still .rodata + .bss, so a wrong value distinguishes "the
; loader did not zero .bss" from "the segment was never mapped" - and a program
; that reached ring 3 and made two syscalls at all proves the privilege round
; trip.
;
; Syscall ABI: number in RAX, args in RDI/RSI/RDX/R10/R8/R9, result in RAX.
;   1 = write(fd, buf, len)
;   0 = exit(code)
; ============================================================================

BITS 64

SECTION .text
GLOBAL _start

_start:
    ; write(1, message, message_len)
    mov rax, 1
    mov rdi, 1                      ; stdout
    lea rsi, [rel message]
    mov rdx, message_len
    syscall

    ; Prove .bss is writable and was zeroed: if the loader skipped the zero
    ; fill, this load picks up whatever the frame last held and the exit code
    ; comes out wrong.
    mov rax, [rel counter]
    test rax, rax
    jnz  .bss_dirty

    mov qword [rel counter], 0x11
    mov rax, [rel magic]           ; from .rodata
    add rax, [rel counter]         ; from .bss

    mov rdi, rax                   ; exit(code)
    xor rax, rax                   ; 0 = exit
    syscall

.bss_dirty:
    mov rdi, 0                     ; a zero exit code is the failure signal
    xor rax, rax
    syscall

.hang:                             ; exit never returns, but never fall off
    jmp .hang

SECTION .rodata
message:     db "hello from ring 3", 10
message_len equ $ - message
magic:       dq 0x1EA405C0DE

SECTION .bss
counter:     resq 1
