/* Link map for leahOS user programs.
 *
 * .text.startup is forced first so _start (from crt0) is at the program's
 * entry address regardless of link order. Programs link high, at 96 TiB, for
 * the same reason the old assembly test did: there is still only one address
 * space, and the low canonical half up to 4 GiB is taken by the kernel's
 * identity map. Per-process address spaces will let this move to the
 * conventional 0x400000.
 */

ENTRY(_start)

SECTIONS
{
    . = 0x0000600000000000;

    .text : ALIGN(4K) {
        *(.text.startup)
        *(.text .text.*)
    }
    .rodata : ALIGN(4K) { *(.rodata .rodata.*) }
    .data   : ALIGN(4K) { *(.data .data.*) }
    .bss    : ALIGN(4K) { *(.bss .bss.*) *(COMMON) }

    /DISCARD/ : { *(.comment) *(.note.*) *(.eh_frame*) }
}
