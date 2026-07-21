; ============================================================================
; leahOS - a minimal ELF64 executable used to exercise the kernel's loader.
;
; Deliberately spread across .text, .rodata and .bss so the loader has to
; handle more than one PT_LOAD segment, and has to zero the part of a segment
; that exists in memory but not in the file.
;
; Called as an ordinary function for now, in ring 0 and in the kernel's own
; address space. Once there is a userland this becomes an execve() target
; instead, and the ret goes away in favour of an exit syscall.
; ============================================================================

BITS 64

SECTION .text
GLOBAL _start

_start:
    ; Proves .bss was mapped writable and pre-zeroed: if the loader skipped
    ; the zero fill, this add would pick up whatever was in the frame.
    mov rax, [rel counter]
    test rax, rax
    jnz  .bss_not_zeroed

    mov qword [rel counter], 0x11

    mov rax, [rel magic]            ; from .rodata
    add rax, [rel counter]          ; from .bss
    ret

.bss_not_zeroed:
    xor rax, rax                    ; a zero return is the failure signal
    ret

SECTION .rodata
magic:   dq 0x1EA405C0DE

SECTION .bss
counter: resq 1
