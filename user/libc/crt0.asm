; ============================================================================
; leahOS - C runtime startup for user programs
;
; The kernel enters a fresh program at _start in ring 3, with a valid stack but
; nothing else set up - no arguments, no return address. crt0's job is to give
; main() a C environment and to make sure main returning turns into a clean
; exit() rather than a return into nothing.
; ============================================================================

BITS 64

; .text.startup is pulled to the front by user.ld, so _start lands at the
; program's entry address no matter how the objects are ordered on the link
; line.
SECTION .text.startup
GLOBAL _start
EXTERN main
EXTERN exit
EXTERN environ

_start:
    ; Terminate the call-frame chain so a debugger stops unwinding here.
    xor rbp, rbp

    ; The kernel left the stack as:
    ;   [argc][argv[0]]...[NULL][envp[0]]...[NULL][strings]
    ; Load all three before touching RSP.
    mov rdi, [rsp]              ; argc
    lea rsi, [rsp + 8]          ; argv
    lea rdx, [rsi + rdi*8 + 8]  ; envp = argv + (argc + 1) * 8

    ; libc needs the environment whether or not main asks for it - getenv is
    ; called by things that never see argv. This computation was already here,
    ; pointing at the strings, because there was no second vector to find.
    mov [rel environ], rdx

    ; Align the stack to 16 bytes; the following CALL pushes the return address,
    ; leaving main entered at the ABI-required 16-aligned-plus-8.
    and rsp, -16

    call main

    ; main's return value is the process exit status.
    mov edi, eax
    call exit

    ; exit does not return; if it somehow does, do not fall off the end.
.hang:
    jmp .hang
