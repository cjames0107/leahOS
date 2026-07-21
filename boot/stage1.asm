; ============================================================================
; leahOS - stage 1 bootloader (master boot record)
;
; The BIOS finds the 0xAA55 signature at the end of LBA 0, copies these 512
; bytes to 0x7C00, and jumps here in 16-bit real mode with DL = boot drive.
;
; We have 512 bytes total, so all this stage does is pull stage 2 off the
; disk and hand control to it.
; ============================================================================

BITS 16
ORG 0x7C00

%include "boot/layout.inc"

; ----------------------------------------------------------------------------
; entry
; ----------------------------------------------------------------------------
start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00              ; stack grows down, away from our own code
    sti

    mov [drive_number], dl      ; BIOS hands us the boot drive in DL

    mov si, msg_stage1
    call print

    ; --- require int 13h LBA extensions -------------------------------------
    ; CHS addressing is a historical curiosity; if the BIOS is old enough to
    ; lack extensions it is too old to run a 64-bit kernel anyway.
    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, [drive_number]
    int 0x13
    jc  .no_extensions
    cmp bx, 0xAA55
    jne .no_extensions

    ; --- read stage 2 -------------------------------------------------------
    mov si, dap
    mov ah, 0x42
    mov dl, [drive_number]
    int 0x13
    jc  .disk_error

    ; --- hand off -----------------------------------------------------------
    mov dl, [drive_number]      ; stage 2 needs to keep reading from this disk
    jmp STAGE2_SEG:STAGE2_OFF

.no_extensions:
    mov si, msg_no_ext
    jmp fail
.disk_error:
    mov si, msg_disk_err
    jmp fail

fail:
    call print
.hang:
    cli
    hlt
    jmp .hang

; ----------------------------------------------------------------------------
; print - write the NUL-terminated string at DS:SI via BIOS teletype
; ----------------------------------------------------------------------------
print:
    push ax
    push bx
    mov ah, 0x0E
    xor bx, bx
.loop:
    lodsb
    test al, al
    jz .done
    int 0x10
    jmp .loop
.done:
    pop bx
    pop ax
    ret

; ----------------------------------------------------------------------------
; data
; ----------------------------------------------------------------------------

; Disk Address Packet for int 13h AH=42h
ALIGN 4
dap:
    db 0x10                     ; packet size
    db 0                        ; reserved
    dw STAGE2_SECTORS           ; sectors to transfer
    dw STAGE2_OFF               ; destination offset
    dw STAGE2_SEG               ; destination segment
    dq STAGE2_LBA               ; starting LBA

drive_number: db 0

msg_stage1:   db "leahOS: stage1", 13, 10, 0
msg_no_ext:   db "stage1: no int13h ext", 13, 10, 0
msg_disk_err: db "stage1: disk error", 13, 10, 0

; ----------------------------------------------------------------------------
; partition table + boot signature
;
; The last 66 bytes of an MBR are not ours: four 16-byte partition entries and
; the signature. tools/mkfs_fat32.py fills the entries in after the image is
; assembled, so the filesystem's geometry has exactly one definition.
;
; If the code above ever outgrows 446 bytes this TIMES goes negative and the
; assembler stops us, which is the correct moment to find out.
; ----------------------------------------------------------------------------
TIMES 446 - ($ - $$) db 0

partition_table:
    TIMES 64 db 0

dw 0xAA55
