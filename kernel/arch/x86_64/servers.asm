; The first three programs, built into the kernel image.
;
; The disk driver and the filesystem, because there is nothing to load them
; from: the machine has to be able to run something before it can read
; anything. And init, because the kernel no longer knows how to open a file at
; all - exec is handed an image now, and the only way to hand the *first*
; program an image is to already be carrying it.
;
; Every other program comes off the disk, read by whoever is exec-ing it.
;
; incbin rather than a generated array because the linker is already the thing
; that knows how to put bytes in a binary, and a C array of two hundred
; thousand entries is a compile nobody wants to wait for.

section .rodata

global g_server_blockd
global g_server_blockd_end
global g_server_vfsd
global g_server_vfsd_end
global g_server_init
global g_server_init_end

align 16
g_server_blockd:
    incbin "build/blockd.elf"
g_server_blockd_end:

align 16
g_server_vfsd:
    incbin "build/vfsd.elf"
g_server_vfsd_end:

align 16
g_server_init:
    incbin "build/init.elf"
g_server_init_end:
