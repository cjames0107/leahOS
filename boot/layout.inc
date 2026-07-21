; ============================================================================
; leahOS - physical memory and disk layout shared by the boot stages
;
; Real mode conventional memory (the low 640 KiB) is the only RAM we can touch
; before entering protected mode, so everything the bootloader needs has to be
; carved out of it. Nothing here may overlap.
;
;   0x00000 - 0x004FF   BIOS IVT + BIOS data area      (untouchable)
;   0x00500 - 0x07BFF   free                           (unused for now)
;   0x07C00 - 0x07DFF   stage 1                        (loaded by BIOS)
;   0x07E00 - 0x07FFF   free
;   0x08000 - 0x0BFFF   stage 2                        (16 KiB)
;   0x0C000 - 0x0FFFF   free
;   0x10000 - 0x1FFFF   kernel staging buffer          (64 KiB)
;   0x20000 - 0x20FFF   E820 memory map
;   0x70000 - 0x72FFF   PML4 / PDPT / PD page tables
;   0xA0000 - 0xFFFFF   video memory + BIOS ROM        (untouchable)
;   0x100000+           kernel, copied up in protected mode
; ============================================================================

; --- stage 2 ---------------------------------------------------------------
STAGE2_LBA          equ 1
STAGE2_SECTORS      equ 32                  ; 16 KiB
STAGE2_SEG          equ 0x0000
STAGE2_OFF          equ 0x8000

; --- kernel ----------------------------------------------------------------
; The kernel is read into a real-mode-addressable buffer and then copied to
; its final home at 1 MiB once we are in 32-bit protected mode. That caps the
; kernel at 64 KiB for now; unreal mode will lift the limit when we need it.
KERNEL_LBA          equ 64
KERNEL_SECTORS      equ 128                 ; 64 KiB
KERNEL_BUFFER       equ 0x10000
KERNEL_BUFFER_SEG   equ 0x1000
KERNEL_PHYS_BASE    equ 0x100000            ; 1 MiB

; Largest number of sectors we hand to a single int 13h call. Real BIOSes are
; only required to support 127, so stay well under it.
DISK_CHUNK_SECTORS  equ 64

; --- E820 memory map -------------------------------------------------------
; u32 entry count at E820_COUNT, then a packed array of 24-byte entries.
; Real mode can only reach this through a segment, hence the split constants.
E820_COUNT          equ 0x20000
E820_ENTRIES        equ 0x20010
E820_SEG            equ 0x2000
E820_COUNT_OFF      equ 0x0000
E820_ENTRIES_OFF    equ 0x0010
E820_ENTRY_SIZE     equ 24

; --- page tables -----------------------------------------------------------
PML4_BASE           equ 0x70000
PDPT_BASE           equ 0x71000
PD_BASE             equ 0x72000
PAGE_TABLES_END     equ 0x73000

PAGE_PRESENT        equ 1 << 0
PAGE_WRITE          equ 1 << 1
PAGE_HUGE           equ 1 << 7

; --- GDT selectors (see stage2.asm) ----------------------------------------
SEL_CODE32          equ 0x08
SEL_DATA32          equ 0x10
SEL_CODE64          equ 0x18
SEL_DATA64          equ 0x20
