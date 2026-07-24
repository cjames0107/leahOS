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

_start:
    ; Terminate the call-frame chain so a debugger stops unwinding here.
    xor rbp, rbp

    ; No argc/argv/envp yet. Pass an empty argument vector so main can be
    ; written as int main(int, char**) without reading garbage.
    xor edi, edi                ; argc = 0
    xor esi, esi                ; argv = NULL
    xor edx, edx                ; envp = NULL

    call main

    ; main's return value is the process exit status.
    mov edi, eax
    call exit

    ; exit does not return; if it somehow does, do not fall off the end.
.hang:
    jmp .hang
