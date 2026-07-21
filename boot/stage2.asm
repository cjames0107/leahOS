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
    call enter_unreal
    call clear_bootinfo
    call detect_memory
    call copy_rom_font
    call load_kernel
    call set_video_mode

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
    mov ax, E820_ENTRIES_SEG
    mov es, ax
    xor di, di

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
    mov ax, BOOTINFO_SEG
    mov es, ax
    mov [es:BI_E820_COUNT], bp
    mov word [es:BI_E820_COUNT + 2], 0
    xor ax, ax
    mov es, ax
    ret

.failed:
    mov si, msg_e820_err
    jmp fail

; ----------------------------------------------------------------------------
; enter_unreal - keep real mode's BIOS, borrow protected mode's reach
;
; A segment register's base and limit are cached in a hidden register that is
; only reloaded when the selector is written. So: enter protected mode, load FS
; from a descriptor with a 4 GiB limit, and leave again without touching FS.
; The cached limit survives, and real-mode code can now address all of memory
; through FS while INT 13h and the rest of the BIOS still work normally.
;
; FS rather than DS or ES on purpose - those get reloaded constantly for BIOS
; calls, and any write to them would drop the big limit on the floor.
; ----------------------------------------------------------------------------
enter_unreal:
    cli
    push eax
    push bx

    lgdt [gdtr]

    mov eax, cr0
    or  al, 1                           ; CR0.PE - protected mode
    mov cr0, eax
    jmp $+2                             ; flush the prefetch queue

    mov bx, SEL_DATA32
    mov fs, bx                          ; FS cache: base 0, limit 4 GiB

    and al, 0xFE                        ; back to real mode
    mov cr0, eax
    jmp $+2

    pop bx
    pop eax
    sti
    ret

; ----------------------------------------------------------------------------
; load_kernel - read the kernel to 1 MiB, one bounce-buffer chunk at a time
;
; The BIOS can only write below 1 MiB, so each chunk is read into the bounce
; buffer and then copied up through FS. The kernel's size in sectors comes from
; the build, so this reads exactly as much of the disk as there is kernel.
; ----------------------------------------------------------------------------
load_kernel:
    mov dword [dap_lba_lo], KERNEL_LBA
    mov dword [dap_lba_hi], 0
    mov dword [dest_linear], KERNEL_PHYS_BASE
    mov word  [sectors_left], KERNEL_SECTORS

.next_chunk:
    mov ax, [sectors_left]
    test ax, ax
    jz  .done

    cmp ax, DISK_CHUNK_SECTORS          ; last chunk is usually short
    jbe .have_count
    mov ax, DISK_CHUNK_SECTORS
.have_count:
    mov [chunk_sectors], ax

    mov [dap_count], ax
    mov word [dap_off], 0
    mov word [dap_seg], BOUNCE_SEG

    mov si, dap
    mov ah, 0x42
    mov dl, [drive_number]
    int 0x13
    jc  .error

    call copy_chunk_to_high

    movzx eax, word [chunk_sectors]
    add [dap_lba_lo], eax
    sub [sectors_left], ax
    jmp .next_chunk

.done:
    ret

.error:
    mov si, msg_disk_err
    jmp fail

; copy_chunk_to_high - bounce buffer -> [dest_linear], chunk_sectors * 512 bytes
;
; Moves dwords through FS, whose 4 GiB limit is what makes a destination above
; 1 MiB legal from real mode at all.
copy_chunk_to_high:
    push ds
    push si
    push ecx
    push eax

    mov ax, BOUNCE_SEG
    mov ds, ax
    xor si, si

    movzx ecx, word [cs:chunk_sectors]
    shl ecx, 7                          ; sectors * 512 / 4 = dwords

    mov edi, [cs:dest_linear]
.loop:
    lodsd                               ; eax = [ds:si], si += 4
    mov [fs:edi], eax
    add edi, 4
    dec ecx
    jnz .loop

    mov [cs:dest_linear], edi

    pop eax
    pop ecx
    pop si
    pop ds
    ret

; ----------------------------------------------------------------------------
; clear_bootinfo - the kernel reads every field, so none may be stale
; ----------------------------------------------------------------------------
clear_bootinfo:
    push es
    push di
    push cx
    mov ax, BOOTINFO_SEG
    mov es, ax
    xor di, di
    xor ax, ax
    mov cx, 128                         ; 256 bytes
    rep stosw
    pop cx
    pop di
    pop es
    ret

; ----------------------------------------------------------------------------
; copy_rom_font - take the video BIOS 8x16 font before we leave text mode
;
; Every VGA BIOS carries the font it uses for text mode. Borrowing it saves
; embedding one in the kernel, and it is the same typeface the firmware and
; the boot messages already used, so the switch to a framebuffer does not
; change what the console looks like.
;
; INT 10h AX=1130h returns the pointer in ES:BP, which is why both are saved.
; ----------------------------------------------------------------------------
copy_rom_font:
    push es
    push bp
    push ds
    push si
    push di
    push cx

    mov ax, 0x1130
    mov bh, 0x06                        ; 8x16 ROM font
    int 0x10                            ; -> ES:BP

    push es
    pop ds
    mov si, bp                          ; DS:SI = font

    mov ax, FONT_SEG
    mov es, ax
    xor di, di

    mov cx, FONT_BYTES / 2
    rep movsw

    xor ax, ax
    mov ds, ax
    mov es, ax

    mov ax, BOOTINFO_SEG
    mov es, ax
    mov dword [es:BI_FONT_ADDRESS], FONT_BUFFER
    xor ax, ax
    mov es, ax

    pop cx
    pop di
    pop si
    pop ds
    pop bp
    pop es
    ret

; ----------------------------------------------------------------------------
; set_video_mode - find a linear-framebuffer VBE mode and switch to it
;
; Done last, after everything that wants to print: once this succeeds there is
; no text console until the kernel builds one.
;
; A mode is only usable if it reports a linear framebuffer. The banked
; alternative needs a bank switch every 64 KiB, which is not worth supporting
; when every card made since the 90s offers LFB.
; ----------------------------------------------------------------------------
set_video_mode:
    push es
    push di

    ; --- controller info, for the mode list ---------------------------------
    mov ax, VBE_INFO_SEG
    mov es, ax
    xor di, di
    mov dword [es:di], 'VBE2'           ; ask for VBE 2.0+ information
    mov ax, 0x4F00
    int 0x10
    cmp ax, 0x004F
    jne .give_up

    ; VideoModePtr is a far pointer at offset 14 of the info block.
    mov ax, [es:14]                     ; offset
    mov dx, [es:16]                     ; segment
    mov [mode_list_off], ax
    mov [mode_list_seg], dx

    mov word [best_mode], 0xFFFF
    mov dword [best_score], 0

.next_mode:
    mov es, [mode_list_seg]
    mov di, [mode_list_off]
    mov cx, [es:di]
    cmp cx, 0xFFFF
    je  .choose
    add word [mode_list_off], 2
    mov [current_mode], cx

    mov ax, VBE_MODE_INFO_SEG
    mov es, ax
    xor di, di
    mov ax, 0x4F01
    int 0x10
    cmp ax, 0x004F
    jne .next_mode

    ; attributes bit 4 = graphics, bit 7 = linear framebuffer
    mov ax, [es:0]
    test ax, 1 << 4
    jz  .next_mode
    test ax, 1 << 7
    jz  .next_mode

    cmp byte [es:0x19], FB_WANT_BPP     ; bits per pixel
    jne .next_mode

    ; Score by pixel count, capped at what we asked for, so the largest mode
    ; that is no bigger than the target wins.
    mov ax, [es:0x12]                   ; width
    cmp ax, FB_WANT_WIDTH
    ja  .next_mode
    mov bx, [es:0x14]                   ; height
    cmp bx, FB_WANT_HEIGHT
    ja  .next_mode

    mul bx                              ; dx:ax = width * height
    mov cx, dx
    shl ecx, 16
    mov cx, ax                          ; ecx = pixel count

    cmp ecx, [best_score]
    jbe .next_mode
    mov [best_score], ecx
    mov ax, [current_mode]
    mov [best_mode], ax
    jmp .next_mode

.choose:
    mov cx, [best_mode]
    cmp cx, 0xFFFF
    je  .give_up

    ; Re-read the winner's details; the scratch block holds the last mode we
    ; looked at, not the one we picked.
    mov ax, VBE_MODE_INFO_SEG
    mov es, ax
    xor di, di
    mov ax, 0x4F01
    int 0x10
    cmp ax, 0x004F
    jne .give_up

    mov ax, [es:0x10]                   ; bytes per scanline
    mov [saved_pitch], ax
    mov ax, [es:0x12]
    mov [saved_width], ax
    mov ax, [es:0x14]
    mov [saved_height], ax
    mov al, [es:0x19]
    mov [saved_bpp], al
    mov eax, [es:0x28]                  ; physical framebuffer base
    mov [saved_fb], eax

    mov bx, [best_mode]
    or  bx, 0x4000                      ; use the linear framebuffer
    mov ax, 0x4F02
    int 0x10
    cmp ax, 0x004F
    jne .give_up

    mov ax, BOOTINFO_SEG
    mov es, ax
    mov eax, [saved_fb]
    mov [es:BI_FB_ADDRESS], eax
    mov dword [es:BI_FB_ADDRESS + 4], 0
    movzx eax, word [saved_pitch]
    mov [es:BI_FB_PITCH], eax
    movzx eax, word [saved_width]
    mov [es:BI_FB_WIDTH], eax
    movzx eax, word [saved_height]
    mov [es:BI_FB_HEIGHT], eax
    mov al, [saved_bpp]
    mov [es:BI_FB_BPP], al

.give_up:
    ; A zero framebuffer address in the boot info tells the kernel to fall
    ; back to the VGA text buffer, so failing here is not fatal.
    xor ax, ax
    mov es, ax
    pop di
    pop es
    ret

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

    ; The kernel is already at 1 MiB - unreal mode put it there directly, so
    ; there is nothing left to relocate.

    ; --- build the initial page tables --------------------------------------
    ; Identity map the first 4 GiB with 2 MiB pages. Crude, but it keeps every
    ; address the bootloader already handed out valid across the CR0.PG write,
    ; and it reaches the MMIO window where a VBE framebuffer lives.
    mov edi, PML4_BASE
    mov ecx, (PAGE_TABLES_END - PML4_BASE) / 4
    xor eax, eax
    rep stosd

    mov dword [PML4_BASE], PDPT_BASE | PAGE_PRESENT | PAGE_WRITE

    ; One PDPT entry per gigabyte, each pointing at its own page directory.
    mov edi, PDPT_BASE
    mov eax, PD_BASE | PAGE_PRESENT | PAGE_WRITE
    mov ecx, PD_COUNT
.map_pdpt:
    mov [edi], eax
    add eax, 0x1000
    add edi, 8
    loop .map_pdpt

    ; The same page directory is mapped a second time high up, so the kernel
    ; is reachable at both its physical and its linked address. The identity
    ; window is what keeps the jump into long mode - and every address the
    ; bootloader already handed out - valid across the switch.
    mov dword [PML4_BASE + PML4_HIGH_INDEX * 8], PDPT_HIGH_BASE | PAGE_PRESENT | PAGE_WRITE
    mov dword [PDPT_HIGH_BASE + PDPT_HIGH_INDEX * 8], PD_BASE | PAGE_PRESENT | PAGE_WRITE

    mov edi, PD_BASE
    mov eax, PAGE_PRESENT | PAGE_WRITE | PAGE_HUGE
    mov ecx, 512 * PD_COUNT
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
    ; The jump goes to the linked higher-half address, not the physical one,
    ; so every symbol the kernel resolves from here on is already correct.
    mov rdi, BOOTINFO_BASE
    mov rax, KERNEL_VIRT_BASE
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

drive_number:  db 0

ALIGN 4
dest_linear:   dd 0                     ; next 32-bit destination address
mode_list_off: dw 0
mode_list_seg: dw 0
current_mode:  dw 0
best_mode:     dw 0xFFFF
best_score:    dd 0
saved_fb:      dd 0
saved_pitch:   dw 0
saved_width:   dw 0
saved_height:  dw 0
saved_bpp:     db 0
sectors_left:  dw 0
chunk_sectors: dw 0

msg_stage2:   db "leahOS: stage2", 13, 10, 0
msg_pmode:    db "leahOS: entering long mode", 13, 10, 0
msg_a20_err:  db "stage2: A20 failed", 13, 10, 0
msg_e820_err: db "stage2: E820 failed", 13, 10, 0
msg_disk_err: db "stage2: disk error", 13, 10, 0

; Pad to the sector count stage 1 was told to read, so the image layout and
; the loader can never disagree.
TIMES (STAGE2_SECTORS * 512) - ($ - $$) db 0
