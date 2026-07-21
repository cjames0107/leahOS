; ============================================================================
; leahOS - interrupt service routine stubs
;
; The CPU gives every vector the same entry protocol but not the same stack:
; some exceptions push a 32-bit error code and most do not. Handling that
; difference in C++ would mean the frame layout depends on the vector, so we
; normalise it here - vectors without an error code push a zero in its place.
;
; What every handler therefore sees, from low address to high:
;
;   r15 r14 r13 r12 r11 r10 r9 r8 rbp rdi rsi rdx rcx rbx rax   <- isr_common
;   vector                                                      <- stub
;   error_code                                                  <- stub or CPU
;   rip cs rflags rsp ss                                        <- CPU
;
; This must stay in step with interrupts::Frame in interrupts.hpp.
; ============================================================================

BITS 64

EXTERN interrupt_dispatch

SECTION .text

; Vectors that the CPU pushes an error code for. Everything else gets a zero
; from us so the frame is one shape.
;   8 #DF  10 #TS  11 #NP  12 #SS  13 #GP  14 #PF  17 #AC  21 #CP  29 #VC  30 #SX
%define HAS_ERROR_CODE(v) ((v) == 8 || (v) == 10 || (v) == 11 || (v) == 12 || \
                           (v) == 13 || (v) == 14 || (v) == 17 || (v) == 21 || \
                           (v) == 29 || (v) == 30)

%macro ISR_STUB 1
isr_stub_%1:
    %if HAS_ERROR_CODE(%1)
        ; the CPU already pushed the error code
    %else
        push qword 0
    %endif
    push qword %1
    jmp isr_common
%endmacro

%assign vector 0
%rep 256
    ISR_STUB vector
%assign vector vector + 1
%endrep

; ----------------------------------------------------------------------------
; isr_common - save state, call into C++, restore, return
; ----------------------------------------------------------------------------
isr_common:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; The CPU aligned RSP to 16 before pushing its 5-qword frame; our 2 + 15
    ; pushes bring the total to 176 bytes, so RSP is 16-byte aligned here and
    ; the call leaves the callee correctly aligned per the SysV ABI.
    cld                                 ; SysV requires DF clear on entry
    mov rdi, rsp                        ; Frame& - first argument
    call interrupt_dispatch

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    add rsp, 16                         ; discard vector and error code
    iretq

; ----------------------------------------------------------------------------
; isr_stub_table - so idt.cpp can install all 256 without naming each one
; ----------------------------------------------------------------------------
SECTION .rodata
GLOBAL isr_stub_table
isr_stub_table:
%assign vector 0
%rep 256
    dq isr_stub_ %+ vector
%assign vector vector + 1
%endrep
