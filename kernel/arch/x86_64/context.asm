; ============================================================================
; leahOS - kernel-thread context switch
;
; The entire saved state of a suspended thread lives on its own kernel stack:
; this pushes the SysV callee-saved registers, parks RSP in the outgoing
; thread's control block, loads the incoming thread's RSP, and pops its
; registers back. The RET at the end returns into wherever the incoming thread
; last called context_switch from - which, for a thread that has never run, is
; a fabricated frame that lands in the trampoline (see scheduler.cpp).
;
; Caller-saved registers are not touched: the C++ caller already expects them
; to be clobbered across a call, so the compiler has spilled anything live.
; RFLAGS is likewise not saved here - a preempted thread's flags ride back on
; the IRETQ that ends its interrupt, and a cooperatively-yielding thread does
; not care about IF across the call because the scheduler restores it.
; ============================================================================

BITS 64

SECTION .text

; void context_switch(u64* save_rsp /*rdi*/, u64 load_rsp /*rsi*/)
GLOBAL context_switch
context_switch:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    mov [rdi], rsp          ; park the outgoing thread's stack pointer
    mov rsp, rsi            ; adopt the incoming thread's stack

    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    ret
