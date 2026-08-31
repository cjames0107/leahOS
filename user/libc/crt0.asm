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
%ifdef PIC
    ; Through the GOT, because `environ` lives in libc.so and this program does
    ; not know where that was placed - only the linker does, and the GOT slot
    ; is where it wrote the answer. A RIP-relative store would go to wherever
    ; this program's own code happens to be, which is not where environ is.
    mov rax, [rel environ wrt ..got]
    mov [rax], rdx
%else
    ; The four boot images link libc in, so environ is at a known distance.
    mov [rel environ], rdx
%endif

    ; Align the stack to 16 bytes; the following CALL pushes the return address,
    ; leaving main entered at the ABI-required 16-aligned-plus-8.
    and rsp, -16

%ifdef PIC
    call main wrt ..plt
%else
    call main
%endif

    ; main's return value is the process exit status.
    mov edi, eax
%ifdef PIC
    call exit wrt ..plt
%else
    call exit
%endif

    ; exit does not return; if it somehow does, do not fall off the end.
.hang:
    jmp .hang
