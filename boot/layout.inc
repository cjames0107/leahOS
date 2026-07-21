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
;   0x10000 - 0x17FFF   disk bounce buffer             (32 KiB)
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
; int 13h can only write through a real-mode segment:offset, so the BIOS can
; never place data above 1 MiB directly. Each chunk lands in a low bounce
; buffer and is then copied to its final home with unreal mode's 32-bit
; addressing, which is what lifts the old 64 KiB kernel ceiling.
KERNEL_LBA          equ 64

; Supplied by the Makefile as -DKERNEL_SECTORS=<actual>, computed from the size
; of the linked kernel so we never read more of the disk than exists.
%ifndef KERNEL_SECTORS
%define KERNEL_SECTORS 128
%endif

BOUNCE_BUFFER       equ 0x10000
BOUNCE_SEG          equ 0x1000
KERNEL_PHYS_BASE    equ 0x100000            ; 1 MiB

; Largest number of sectors we hand to a single int 13h call. Real BIOSes are
; only required to support 127, so stay well under it. Also sizes the bounce
; buffer: 64 sectors = 32 KiB, which fits between 0x10000 and the E820 map.
DISK_CHUNK_SECTORS  equ 64

; --- boot information block ------------------------------------------------
; Everything the kernel needs that only the BIOS could tell us. Field offsets
; are mirrored by boot::Info in kernel/include/leah/bootinfo.hpp; the two must
; be changed together.
BOOTINFO_BASE       equ 0x20000
BOOTINFO_SEG        equ 0x2000

BI_E820_COUNT       equ 0x00        ; u32
BI_FB_ADDRESS       equ 0x08        ; u64  linear framebuffer, 0 if text mode
BI_FB_PITCH         equ 0x10        ; u32  bytes per scanline
BI_FB_WIDTH         equ 0x14        ; u32
BI_FB_HEIGHT        equ 0x18        ; u32
BI_FB_BPP           equ 0x1C        ; u8
BI_FONT_ADDRESS     equ 0x20        ; u32  8x16 glyphs, 256 of them

; The E820 entries move out of the header into their own page so the block
; above can grow without colliding with them.
E820_ENTRIES        equ 0x21000
E820_ENTRIES_SEG    equ 0x2100
E820_ENTRY_SIZE     equ 24

; Copy of the video BIOS 8x16 font: 256 glyphs of 16 bytes.
FONT_BUFFER         equ 0x22000
FONT_SEG            equ 0x2200
FONT_BYTES          equ 4096

; Scratch for the VBE controller and mode information blocks.
VBE_INFO            equ 0x23000
VBE_INFO_SEG        equ 0x2300
VBE_MODE_INFO       equ 0x23400
VBE_MODE_INFO_SEG   equ 0x2340

; What we ask for, in order of preference.
FB_WANT_WIDTH       equ 1024
FB_WANT_HEIGHT      equ 768
FB_WANT_BPP         equ 32

; --- page tables -----------------------------------------------------------
; Four page directories, so the identity map reaches 4 GiB rather than 1. The
; framebuffer a VBE mode hands back typically sits just under 4 GiB, and the
; kernel has to be able to touch it before the VMM exists.
PML4_BASE           equ 0x70000
PDPT_BASE           equ 0x71000             ; identity, PML4[0]
PD_BASE             equ 0x72000             ; 4 directories, 0x72000-0x75FFF
PD_COUNT            equ 4
PDPT_HIGH_BASE      equ 0x76000             ; higher half, PML4[511]
PAGE_TABLES_END     equ 0x77000

; The kernel is linked for the top 2 GiB. Virtual 0xFFFFFFFF80000000 is
; physical 0, so the image loaded at 1 MiB appears here.
;   PML4 index 511, PDPT index 510, PD index 0
KERNEL_VIRT_BASE    equ 0xFFFFFFFF80100000
PML4_HIGH_INDEX     equ 511
PDPT_HIGH_INDEX     equ 510

PAGE_PRESENT        equ 1 << 0
PAGE_WRITE          equ 1 << 1
PAGE_HUGE           equ 1 << 7

; --- GDT selectors (see stage2.asm) ----------------------------------------
SEL_CODE32          equ 0x08
SEL_DATA32          equ 0x10
SEL_CODE64          equ 0x18
SEL_DATA64          equ 0x20
