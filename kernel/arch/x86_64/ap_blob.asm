; The assembled AP trampoline, carried inside the kernel image so smp::init can
; copy it into low memory. It is a flat binary built for a fixed load address -
; see boot/ap_trampoline.asm - not linkable code, which is why it arrives as an
; opaque blob rather than a section the linker places.

SECTION .rodata
ALIGN 16

GLOBAL ap_trampoline_blob
GLOBAL ap_trampoline_blob_end

ap_trampoline_blob:
    incbin "build/ap_trampoline.bin"
ap_trampoline_blob_end:
