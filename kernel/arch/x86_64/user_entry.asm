; ============================================================================
; leahOS - first entry of a user task into ring 3
;
; A freshly created user process (an exec target or a fork child) has a kernel
; stack fabricated so that the scheduler's context_switch "returns" here, with
; RSP pointing at a scheduler::TrapFrame. This restores that frame and drops to
; ring 3 with IRETQ - the one instruction that sets SS:RSP, RFLAGS and CS:RIP
; across a privilege change in one step.
;
; This runs only for a task's *first* trip to ring 3. After that the task is an
; ordinary preempted thread: it re-enters ring 3 through the IRETQ at the end of
; whatever interrupt or the SYSRET of whatever syscall the scheduler suspended
; it in.
; ============================================================================

BITS 64

SECTION .text

GLOBAL user_return
user_return:
    ; Ring 3 wants the user data selector in every segment register. IRETQ
    ; reloads SS from the frame but leaves DS/ES/FS/GS alone, so set them here.
    ; AX is scratch; RAX gets its real value from the frame a few lines down.
    mov ax, 0x18 | 3
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

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

    ; RSP now points at the IRETQ frame: RIP, CS, RFLAGS, RSP, SS.
    iretq

; ----------------------------------------------------------------------------
; sigreturn_to_user - leave a signal handler by the IRETQ path.
;
; A signal can interrupt user code at a hardware IRQ as well as at a syscall, so
; restoring the interrupted context has to put back every register - including
; RCX and R11, which the SYSRET the syscall path normally returns through would
; clobber. Switching RSP onto a fully populated TrapFrame and falling into
; user_return restores the lot with an IRETQ instead.
;
; RDI points at a scheduler::TrapFrame the caller has already validated. The
; kernel stack below it is abandoned, which is fine: the next entry from ring 3
; starts again from TSS.rsp0.
; ----------------------------------------------------------------------------
GLOBAL sigreturn_to_user
sigreturn_to_user:
    mov rsp, rdi
    jmp user_return
