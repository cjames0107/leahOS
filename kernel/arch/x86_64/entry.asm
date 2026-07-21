; ============================================================================
; leahOS - kernel entry stub
;
; Entered from stage 2 in 64-bit long mode with paging on, the first 1 GiB
; identity mapped, and RDI pointing at the E820 memory map.
;
; Everything C++ assumes about its environment - a stack, zeroed .bss - has to
; be true before we make the call.
; ============================================================================

BITS 64

SECTION .text.entry

GLOBAL _start
EXTERN kernel_main
EXTERN __bss_start
EXTERN __bss_end

_start:
    cli

    ; The bootloader's stack lives at 0x7C00 and is about to be reused for
    ; other things, so stash the argument somewhere that does not depend on it.
    mov r15, rdi

    ; Zero .bss. It is NOBITS, so it never existed in the flat image and holds
    ; whatever the BIOS happened to leave at that address.
    mov rdi, __bss_start
    mov rcx, __bss_end
    sub rcx, rdi
    xor eax, eax
    rep stosb

    mov rsp, stack_top
    xor rbp, rbp                        ; terminate the call-frame chain

    mov rdi, r15
    call kernel_main

    ; kernel_main is not supposed to return.
.halt:
    cli
    hlt
    jmp .halt

SECTION .bss
ALIGN 16
stack_bottom:
    RESB 64 * 1024
stack_top:
