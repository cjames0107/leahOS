/* Linked well away from the low 4 GiB the kernel identity maps.
 *
 * A real userland program would link at 0x400000, which is conventional and
 * fine once each process has its own address space. Today there is only the
 * kernel's, and mapping over 0x400000 would replace the identity mapping of
 * that physical memory - which the PMM is still handing out. */

ENTRY(_start)

SECTIONS
{
    . = 0x0000600000000000;

    .text : ALIGN(4K) { *(.text .text.*) }
    .rodata : ALIGN(4K) { *(.rodata .rodata.*) }
    .data : ALIGN(4K) { *(.data .data.*) }
    .bss : ALIGN(4K) { *(.bss .bss.*) *(COMMON) }

    /DISCARD/ : { *(.comment) *(.note.*) *(.eh_frame*) }
}
