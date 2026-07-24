; ============================================================================
; leahOS - ring 3 entry/exit and the SYSCALL trampoline
;
; Three pieces that together let a program run at CPL 3 and call back into the
; kernel:
;
;   enter_user_mode  drops into ring 3 via IRETQ, saving a kernel context to
;                    return to
;   syscall_entry    the SYSCALL landing pad: switch stacks, call C++, SYSRET
;   exit_to_kernel   unwinds back to enter_user_mode's caller with an exit code
;
; There is no scheduler yet. enter_user_mode runs one program to completion and
; the exit syscall longjmps back out of it, which is exactly the primitive a
; scheduler will later generalise into a context switch.
; ============================================================================

BITS 64
DEFAULT REL                     ; RIP-relative access to our own .bss globals

EXTERN syscall_dispatch

; GDT selectors, RPL 3. Must match gdt.hpp.
SEL_USER_DATA   equ 0x18 | 3
SEL_USER_CODE   equ 0x20 | 3

SECTION .text

; ----------------------------------------------------------------------------
; u64 enter_user_mode(u64 entry /*rdi*/, u64 user_stack /*rsi*/)
;
; Saves the callee-saved registers and RSP so exit_to_kernel can return here,
; then builds an interrupt frame and IRETQs into ring 3. IRETQ is the only way
; into a lower privilege level that also sets SS:RSP and RFLAGS atomically.
; ----------------------------------------------------------------------------
GLOBAL enter_user_mode
enter_user_mode:
    ; Preserve the SysV callee-saved set at a fixed spot, plus the return
    ; address already on the stack, so exit_to_kernel can reconstruct this
    ; frame and RET as though enter_user_mode returned normally.
    mov [return_rsp], rsp
    mov [return_rbx], rbx
    mov [return_rbp], rbp
    mov [return_r12], r12
    mov [return_r13], r13
    mov [return_r14], r14
    mov [return_r15], r15

    ; Ring 3 runs with the user data selector in every segment register. The
    ; CPU reloads SS from the IRETQ frame but leaves DS/ES/FS/GS alone.
    mov ax, SEL_USER_DATA
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; IRETQ pops, from the top: RIP, CS, RFLAGS, RSP, SS.
    push SEL_USER_DATA          ; SS
    push rsi                    ; RSP - the user stack
    push 0x202                  ; RFLAGS: IF set, everything else clear
    push SEL_USER_CODE          ; CS
    push rdi                    ; RIP - the program entry point

    ; Do not leak kernel register contents into the new ring-3 program.
    xor rax, rax
    xor rbx, rbx
    xor rcx, rcx
    xor rdx, rdx
    xor rbp, rbp
    xor r8, r8
    xor r9, r9
    xor r10, r10
    xor r11, r11
    xor r12, r12
    xor r13, r13
    xor r14, r14
    xor r15, r15

    iretq

; ----------------------------------------------------------------------------
; exit_to_kernel(u64 code /*rdi*/) - never returns to the caller
;
; Restores the context enter_user_mode saved and returns from *it*, carrying
; the exit code back in RAX. This is a longjmp in all but name.
; ----------------------------------------------------------------------------
GLOBAL exit_to_kernel
exit_to_kernel:
    mov rax, rdi                ; the exit code becomes enter_user_mode's return

    mov rsp, [return_rsp]
    mov rbx, [return_rbx]
    mov rbp, [return_rbp]
    mov r12, [return_r12]
    mov r13, [return_r13]
    mov r14, [return_r14]
    mov r15, [return_r15]

    ret                         ; returns out of enter_user_mode

; ----------------------------------------------------------------------------
; syscall_entry - where the SYSCALL instruction lands
;
; On entry the CPU has put the user RIP in RCX and RFLAGS in R11, loaded CS/SS
; from STAR, and masked RFLAGS with FMASK (so IF is already clear). It has NOT
; switched stacks: RSP still points into user memory, which we must not trust
; or keep using.
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

    ; RAX in the frame now holds the return value; reload the whole set so a
    ; handler that changed a register (a future context switch will) is honoured.
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

SECTION .bss
ALIGN 8
; Single-CPU scratch. When SMP arrives these move into a per-CPU block reached
; through the GS base, and syscall_entry gains a swapgs.
kernel_syscall_rsp: RESQ 1
user_rsp_scratch:   RESQ 1

return_rsp: RESQ 1
return_rbx: RESQ 1
return_rbp: RESQ 1
return_r12: RESQ 1
return_r13: RESQ 1
return_r14: RESQ 1
return_r15: RESQ 1

SECTION .text
; Let C++ hand us the kernel stack SYSCALL should switch to.
GLOBAL set_syscall_stack
set_syscall_stack:
    mov [kernel_syscall_rsp], rdi
    ret
