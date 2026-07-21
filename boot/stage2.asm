; ============================================================================
; leahOS - stage 2 bootloader
;
; Entered from stage 1 at 0x8000 in 16-bit real mode with DL = boot drive.
; This stage does everything the BIOS can help with while we still have it,
; then walks the CPU up through protected mode into long mode and jumps to
; the kernel.
;
;   16-bit real   : A20, E820 memory map, load kernel off disk
;   32-bit pmode  : relocate kernel to 1 MiB, build page tables, enable IA-32e
;   64-bit long   : hand off to the kernel at 1 MiB
; ============================================================================

BITS 16
ORG 0x8000

%include "boot/layout.inc"

; Drop a coloured character on the bottom row of the text screen. Bootloader
; debugging has no debugger, so cheap progress markers earn their keep.
%macro MARK 2
    mov byte [0xB8000 + (80 * 24 + %1) * 2], %2
    mov byte [0xB8000 + (80 * 24 + %1) * 2 + 1], 0x0A
%endmacro

; ----------------------------------------------------------------------------
; 16-bit real mode
; ----------------------------------------------------------------------------
stage2_start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti

    mov [drive_number], dl

    mov si, msg_stage2
    call print

    call enable_a20
    call detect_memory
    call load_kernel

    mov si, msg_pmode
    call print

    ; --- leave the BIOS behind ----------------------------------------------
    cli
    lgdt [gdtr]

    mov eax, cr0
    or  eax, 1                          ; CR0.PE
    mov cr0, eax

    jmp SEL_CODE32:pm_entry             ; far jump reloads CS, flushes pipeline

; ----------------------------------------------------------------------------
; enable_a20 - un-wrap the 21st address line
;
; Without A20 every odd megabyte aliases onto the one below it, which would
; quietly corrupt the kernel we are about to place at 1 MiB.
; ----------------------------------------------------------------------------
enable_a20:
    call check_a20
    jc  .done                           ; BIOS already did it for us

    in  al, 0x92                        ; fast A20 gate
    test al, 2
    jnz .recheck
    or  al, 2
    and al, 0xFE                        ; bit 0 is fast RESET - never set it
    out 0x92, al

.recheck:
    call check_a20
    jc  .done

    mov si, msg_a20_err
    jmp fail
.done:
    ret

; check_a20 - returns CF=1 if A20 is enabled
;
; Writes different bytes to 0x0500 and 0x100500. With A20 masked the second
; address wraps onto the first and the two reads agree.
check_a20:
    pushf
    push ds
    push es
    push di
    push si
    cli

    xor ax, ax
    mov es, ax
    mov di, 0x0500
    mov ax, 0xFFFF
    mov ds, ax
    mov si, 0x0510                      ; 0xFFFF:0x0510 = 0x100500

    mov al, [es:di]                     ; save both bytes
    push ax
    mov al, [ds:si]
    push ax

    mov byte [es:di], 0x00
    mov byte [ds:si], 0xFF
    cmp byte [es:di], 0xFF              ; did the high write alias down?

    pop ax                              ; restore, cmp already set ZF
    mov [ds:si], al
    pop ax
    mov [es:di], al

    mov ax, 0
    je  .disabled
    mov ax, 1
.disabled:
    pop si
    pop di
    pop es
    pop ds
    popf

    cmp ax, 1
    je  .enabled
    clc
    ret
.enabled:
    stc
    ret

; ----------------------------------------------------------------------------
; detect_memory - int 15h AX=E820 memory map
;
; This is the only reliable way to learn the machine's RAM layout, and it is
; only available while the BIOS is still around. Stash it for the kernel's
; physical memory manager.
; ----------------------------------------------------------------------------
detect_memory:
    mov ax, E820_SEG
    mov es, ax
    mov di, E820_ENTRIES_OFF

    xor ebx, ebx                        ; continuation value, 0 = start
    xor bp, bp                          ; accepted entry count

    mov edx, 0x534D4150                 ; 'SMAP'
    mov eax, 0xE820
    mov dword [es:di + 20], 1           ; ACPI 3.0: assume entry is valid
    mov ecx, E820_ENTRY_SIZE
    int 0x15
    jc  .failed
    cmp eax, 0x534D4150                 ; BIOS echoes the signature back
    jne .failed
    test ebx, ebx
    jz  .failed                         ; a one-entry map is not a map
    jmp .inspect

.next:
    mov eax, 0xE820
    mov edx, 0x534D4150
    mov dword [es:di + 20], 1
    mov ecx, E820_ENTRY_SIZE
    int 0x15
    jc  .done                           ; carry on a later call means "end"

.inspect:
    jcxz .skip                          ; BIOS returned nothing
    cmp cl, 20
    jbe .accept                         ; 20-byte entry, no ACPI flags
    test byte [es:di + 20], 1
    jz  .skip                           ; ACPI 3.0 "ignore this entry"
.accept:
    mov eax, [es:di + 8]                ; length low
    or  eax, [es:di + 12]               ; length high
    jz  .skip                           ; zero-length region
    inc bp
    add di, E820_ENTRY_SIZE
.skip:
    test ebx, ebx
    jnz .next

.done:
    mov [es:E820_COUNT_OFF], bp
    mov word [es:E820_COUNT_OFF + 2], 0
    xor ax, ax
    mov es, ax
    ret

.failed:
    mov si, msg_e820_err
    jmp fail

; ----------------------------------------------------------------------------
; load_kernel - pull the kernel image into the staging buffer
;
; int 13h writes through a real-mode segment:offset, so the highest address we
; can name is just under 1 MiB. The kernel lands at 0x10000 for now and gets
; relocated to its real home once we have 32-bit addressing.
; ----------------------------------------------------------------------------
load_kernel:
    mov dword [dap_lba_lo], KERNEL_LBA
    mov dword [dap_lba_hi], 0
    mov word  [dap_off], 0
    mov word  [dap_seg], KERNEL_BUFFER_SEG

    mov cx, KERNEL_SECTORS / DISK_CHUNK_SECTORS
.chunk:
    mov word [dap_count], DISK_CHUNK_SECTORS
    mov si, dap
    mov ah, 0x42
    mov dl, [drive_number]
    int 0x13
    jc  .error

    add dword [dap_lba_lo], DISK_CHUNK_SECTORS
    add word  [dap_seg], (DISK_CHUNK_SECTORS * 512) / 16
    loop .chunk
    ret

.error:
    mov si, msg_disk_err
    jmp fail

; ----------------------------------------------------------------------------
; print / fail - 16-bit only
; ----------------------------------------------------------------------------
print:
    push ax
    push bx
    mov ah, 0x0E
    xor bx, bx
.loop:
    lodsb
    test al, al
    jz  .done
    int 0x10
    jmp .loop
.done:
    pop bx
    pop ax
    ret

fail:
    call print
.hang:
    cli
    hlt
    jmp .hang

; ============================================================================
; 32-bit protected mode
; ============================================================================
BITS 32
pm_entry:
    mov ax, SEL_DATA32
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x7C00

    MARK 0, 'P'

    ; --- relocate the kernel to 1 MiB ---------------------------------------
    mov esi, KERNEL_BUFFER
    mov edi, KERNEL_PHYS_BASE
    mov ecx, (KERNEL_SECTORS * 512) / 4
    rep movsd

    MARK 1, 'M'

    ; --- build the initial page tables --------------------------------------
    ; Identity map the first 1 GiB with 2 MiB pages. Crude, but it keeps every
    ; address the bootloader already handed out valid across the CR0.PG write.
    mov edi, PML4_BASE
    mov ecx, (PAGE_TABLES_END - PML4_BASE) / 4
    xor eax, eax
    rep stosd

    mov dword [PML4_BASE], PDPT_BASE | PAGE_PRESENT | PAGE_WRITE
    mov dword [PDPT_BASE], PD_BASE   | PAGE_PRESENT | PAGE_WRITE

    mov edi, PD_BASE
    mov eax, PAGE_PRESENT | PAGE_WRITE | PAGE_HUGE
    mov ecx, 512
.map_pd:
    mov [edi], eax                      ; high dword is already zero
    add eax, 0x200000                   ; next 2 MiB frame
    add edi, 8
    loop .map_pd

    MARK 2, 'T'

    ; --- switch on IA-32e ---------------------------------------------------
    mov eax, cr4
    or  eax, 1 << 5                     ; CR4.PAE - mandatory for long mode
    mov cr4, eax

    mov eax, PML4_BASE
    mov cr3, eax

    mov ecx, 0xC0000080                 ; IA32_EFER
    rdmsr
    or  eax, 1 << 8                     ; EFER.LME
    wrmsr

    mov eax, cr0
    or  eax, 1 << 31                    ; CR0.PG - long mode activates here
    mov cr0, eax

    jmp SEL_CODE64:lm_entry

; ============================================================================
; 64-bit long mode
; ============================================================================
BITS 64
DEFAULT ABS                             ; no RIP-relative addressing in a flat binary

lm_entry:
    mov ax, SEL_DATA64
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov rsp, 0x7C00

    MARK 3, 'L'

    ; System V AMD64: first argument in RDI. Hand the kernel its memory map.
    mov rdi, E820_COUNT
    mov rax, KERNEL_PHYS_BASE
    jmp rax

; ============================================================================
; data
; ============================================================================

; --- Global Descriptor Table ------------------------------------------------
; Flat segments; on x86-64 segmentation survives only as a privilege and mode
; switch, so the base/limit fields in the 64-bit entries are ignored.
ALIGN 8
gdt:
    dq 0x0000000000000000               ; 0x00 null
    dq 0x00CF9A000000FFFF               ; 0x08 code32  4 GiB, exec/read
    dq 0x00CF92000000FFFF               ; 0x10 data32  4 GiB, read/write
    dq 0x00AF9A000000FFFF               ; 0x18 code64  L=1
    dq 0x00CF92000000FFFF               ; 0x20 data64
gdt_end:

ALIGN 8
gdtr:
    dw gdt_end - gdt - 1
    dd gdt

; --- Disk Address Packet ----------------------------------------------------
ALIGN 4
dap:
    db 0x10
    db 0
dap_count:  dw 0
dap_off:    dw 0
dap_seg:    dw 0
dap_lba_lo: dd 0
dap_lba_hi: dd 0

drive_number: db 0

msg_stage2:   db "leahOS: stage2", 13, 10, 0
msg_pmode:    db "leahOS: entering long mode", 13, 10, 0
msg_a20_err:  db "stage2: A20 failed", 13, 10, 0
msg_e820_err: db "stage2: E820 failed", 13, 10, 0
msg_disk_err: db "stage2: disk error", 13, 10, 0

; Pad to the sector count stage 1 was told to read, so the image layout and
; the loader can never disagree.
TIMES (STAGE2_SECTORS * 512) - ($ - $$) db 0
