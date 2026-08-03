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
; Boot images rather than ELFs: the build reads the program headers and writes
; out what they meant (tools/mkbootimage.py), so the kernel maps segments from
; a fixed table and has no ELF parser at all. Every other program is exec'd by
; a process that can read the file and do its own parsing.
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
    incbin "build/blockd.img"
g_server_blockd_end:

align 16
g_server_vfsd:
    incbin "build/vfsd.img"
g_server_vfsd_end:

align 16
g_server_init:
    incbin "build/init.img"
g_server_init_end:
