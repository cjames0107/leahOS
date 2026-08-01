; The first two programs, built into the kernel image.
;
; They are the disk driver and the filesystem, so there is nothing to load them
; from: the machine has to be able to run something before it can read
; anything. Every other program comes off the disk, through these two.
;
; incbin rather than a generated array because the linker is already the thing
; that knows how to put bytes in a binary, and a C array of two hundred
; thousand entries is a compile nobody wants to wait for.

section .rodata

global g_server_blockd
global g_server_blockd_end
global g_server_vfsd
global g_server_vfsd_end

align 16
g_server_blockd:
    incbin "build/blockd.elf"
g_server_blockd_end:

align 16
g_server_vfsd:
    incbin "build/vfsd.elf"
g_server_vfsd_end:
