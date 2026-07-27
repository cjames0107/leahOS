; ============================================================================
; leahOS - application processor startup trampoline
;
; A SIPI starts an AP in 16-bit real mode at CS = vector << 8, IP = 0, with
; nothing else set up: no GDT, no paging, no long mode. This walks it the same
; road stage 2 walked the bootstrap processor - real -> protected -> long - and
; hands control to a 64-bit C function.
;
; It is copied to a fixed page in low memory (the SIPI vector is a page number,
; so it has to live below 1 MiB) and runs identity-mapped. The kernel's page
; tables have nothing in the low half, so smp::init temporarily identity-maps
; this page before starting anyone and removes the mapping afterwards.
;
; The parameter block at the end is filled in by the kernel before each AP is
; released, which is why APs are started one at a time.
; ============================================================================

BITS 16
ORG 0x8000

trampoline_start:
    cli
    cld

    ; A 32-bit GDT, reachable because everything here is identity-mapped.
    lgdt [gdt32_pointer]

    mov eax, cr0
    or  eax, 1                      ; CR0.PE
    mov cr0, eax
    jmp 0x08:protected_mode

; ----------------------------------------------------------------------------
BITS 32
protected_mode:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    mov eax, cr4
    or  eax, 1 << 5                 ; CR4.PAE, required for long mode
    mov cr4, eax

    ; The kernel's own page tables, so this CPU sees the same address space the
    ; bootstrap processor does the moment paging comes on.
    mov eax, [param_cr3]
    mov cr3, eax

    mov ecx, 0xC0000080             ; IA32_EFER
    rdmsr
    or  eax, (1 << 8) | (1 << 11)   ; LME (long mode) + NXE (honour the NX bit)
    wrmsr

    mov eax, cr0
    or  eax, 1 << 31                ; CR0.PG - paging on, long mode active
    mov cr0, eax

    lgdt [gdt64_pointer]
    jmp 0x08:long_mode

; ----------------------------------------------------------------------------
BITS 64
long_mode:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    ; Its own stack, allocated by the kernel; then into C, never to return.
    mov rsp, [param_stack]
    mov rax, [param_entry]
    jmp rax

; ----------------------------------------------------------------------------
ALIGN 8
gdt32:
    dq 0x0000000000000000           ; null
    dq 0x00CF9A000000FFFF           ; 32-bit code, base 0, limit 4 GiB
    dq 0x00CF92000000FFFF           ; 32-bit data
gdt32_end:

gdt32_pointer:
    dw gdt32_end - gdt32 - 1
    dd gdt32

ALIGN 8
gdt64:
    dq 0x0000000000000000           ; null
    dq 0x00AF9A000000FFFF           ; 64-bit code (L bit set)
    dq 0x00AF92000000FFFF           ; 64-bit data
gdt64_end:

gdt64_pointer:
    dw gdt64_end - gdt64 - 1
    dq gdt64

; ----------------------------------------------------------------------------
; Parameter block, at the end of the page so the kernel can write it at fixed
; offsets without caring how long the code above grew.
;   0x8FE8  entry point (64-bit kernel function)
;   0x8FF0  stack top for this AP
;   0x8FF8  CR3 to load
times 0xFE8 - ($ - $$) db 0
param_entry: dq 0
param_stack: dq 0
param_cr3:   dq 0
