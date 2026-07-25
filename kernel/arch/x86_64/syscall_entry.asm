; ============================================================================
; leahOS - the SYSCALL landing pad
;
; A user process reaches the kernel through SYSCALL. This saves the user
; registers as a syscall::Frame on the current task's kernel stack, calls the
; C++ dispatcher, then restores and SYSRETs back to ring 3.
;
; The kernel stack it switches to is the running task's own - the scheduler
; keeps kernel_syscall_rsp pointing at it. That matters because a syscall that
; blocks (fork, wait, exit) is suspended mid-handler on this stack; a single
; shared stack would be clobbered the moment another task made a syscall.
; ============================================================================

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
    mov [user_rsp_scratch], rsp
    mov rsp, [kernel_syscall_rsp]

    ; Build a syscall::Frame. Push order is the reverse of the struct, so the
    ; lowest field (r15) is pushed last and ends up at the lowest address.
    push qword [user_rsp_scratch]   ; user_rsp
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
    call syscall_dispatch

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

    ; SYSRETQ loads RIP from RCX, RFLAGS from R11, and CS/SS from STAR at RPL 3.
    o64 sysret

; ----------------------------------------------------------------------------
; set_syscall_stack - the scheduler points this at the running task's stack on
; every switch, so SYSCALL always lands on the current task's own kernel stack.
; ----------------------------------------------------------------------------
GLOBAL set_syscall_stack
set_syscall_stack:
    mov [kernel_syscall_rsp], rdi
    ret

SECTION .bss
ALIGN 8
kernel_syscall_rsp: RESQ 1
user_rsp_scratch:   RESQ 1
