/* Link map for the dynamic linker.
 *
 * At a fixed address, as an ordinary executable, which is the one thing in the
 * system that is deliberately not position-independent: an interpreter that
 * needed relocating would have to relocate itself before it could relocate
 * anything else, and that bootstrap costs a page of careful assembly to buy
 * something no address space here is short of. 0x08000000 is between the
 * libraries and the heap - see <loader.h> for the map.
 */

ENTRY(_start)

SECTIONS
{
    . = 0x08000000;

    .text : ALIGN(4K) {
        *(.text.startup)
        *(.text .text.*)
    }
    .rodata : ALIGN(4K) { *(.rodata .rodata.*) }
    .data   : ALIGN(4K) { *(.data .data.*) }
    .bss    : ALIGN(4K) { *(.bss .bss.*) *(COMMON) }

    /DISCARD/ : { *(.comment) *(.note.*) *(.eh_frame*) }
}
