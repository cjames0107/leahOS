; ============================================================================
; leahOS - the SYSCALL landing pad
;
; A user process reaches the kernel through SYSCALL. This saves the user
; registers as a syscall::Frame on the current task's kernel stack, calls the
; C++ dispatcher, then restores and SYSRETs back to ring 3.
;
; The kernel stack it switches to is the running task's own - the scheduler
; keeps this CPU's per-CPU block pointing at it. That matters because a syscall
; that blocks (fork, wait, exit) is suspended mid-handler on this stack; a
; single shared stack would be clobbered the moment another task made a
; syscall.
;
; Finding that stack is the awkward part. On entry there is no free register -
; RSP still points into user memory and every general register holds user data -
; so the address has to come from somewhere the instruction stream can reach
; without one. That is what GS is for: SWAPGS exchanges GS_BASE with
; IA32_KERNEL_GS_BASE, so one instruction turns GS into this processor's own
; block, and gs:[8] is its syscall stack. A global would name a single stack for
; the whole machine, which is only ever right on a uniprocessor.
;
; The invariant, which every entry from ring 3 and every exit to it maintains:
; while kernel code runs, GS_BASE is this CPU's percpu::Cpu and
; IA32_KERNEL_GS_BASE holds the user's GS; in ring 3 the two are swapped.
; ============================================================================

; percpu::Cpu field offsets - an ABI with percpu.hpp.
%define CPU_SELF            0
%define CPU_SYSCALL_STACK   8
%define CPU_USER_RSP        16

BITS 64
DEFAULT REL

EXTERN syscall_dispatch

SECTION .text

; ----------------------------------------------------------------------------
; syscall_entry
;
; On entry the CPU has put the user RIP in RCX and RFLAGS in R11, loaded CS/SS
; from STAR, and masked RFLAGS with FMASK (so IF is already clear). It has NOT
; switched stacks: RSP still points into user memory, which we must not trust.
; ----------------------------------------------------------------------------
GLOBAL syscall_entry
syscall_entry:
    swapgs                          ; GS now this CPU's block, not the user's
    mov [gs:CPU_USER_RSP], rsp      ; park the untrusted user stack
    mov rsp, [gs:CPU_SYSCALL_STACK] ; and stand on the running task's own

    ; Build a syscall::Frame. Push order is the reverse of the struct, so the
    ; lowest field (r15) is pushed last and ends up at the lowest address.
    push qword [gs:CPU_USER_RSP]    ; user_rsp
    push r11                        ; user_flags
    push rcx                        ; user_rip
    push rax                        ; number / return slot
    push rdi
    push rsi
    push rdx
    push r8
    push r9
    push r10
    push r11
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    cld                             ; SysV requires DF clear on entry to C
    mov rdi, rsp                    ; syscall::Frame* - first argument

    ; Re-align the stack before entering C. A syscall::Frame is seventeen
    ; words - an odd number - so building it flipped the 16-byte alignment the
    ; SysV ABI promises a function, and the compiler places 16-aligned locals
    ; on that promise. Nothing noticed while the kernel was integer-only;
    ; FXRSTOR notices immediately, with a #GP. RDI already points at the frame,
    ; so the gap costs nothing but eight bytes of stack.
    sub rsp, 8
    call syscall_dispatch
    add rsp, 8

    ; RAX in the frame now holds the return value. A handler may also have
    ; rewritten the whole frame (execve does, to jump into the new image), so
    ; reload all of it.
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdx
    pop rsi
    pop rdi
    pop rax
    pop rcx                         ; user_rip -> RCX for SYSRET
    pop r11                         ; user_flags -> R11 for SYSRET
    pop rsp                         ; back to the user stack

    ; Hand GS back to the user and park this CPU's block for the next entry.
    swapgs

    ; SYSRETQ loads RIP from RCX, RFLAGS from R11, and CS/SS from STAR at RPL 3.
    o64 sysret

; ----------------------------------------------------------------------------
; set_syscall_stack - the scheduler points this at the running task's stack on
; every switch, so SYSCALL always lands on the current task's own kernel stack.
;
; Writing it through GS rather than by CPU slot means it always lands on the
; block of the processor actually executing, with no way for the two to
; disagree.
; ----------------------------------------------------------------------------
GLOBAL set_syscall_stack
set_syscall_stack:
    mov [gs:CPU_SYSCALL_STACK], rdi
    ret
