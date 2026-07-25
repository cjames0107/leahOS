/* Link map for leahOS user programs.
 *
 * .text.startup is forced first so _start (from crt0) is at the program's
 * entry address regardless of link order. Programs link at the conventional
 * 0x400000: the kernel keeps all of its own mappings in the higher half now, so
 * the entire low canonical half belongs to user space and the small code model
 * reaches everything.
 */

ENTRY(_start)

SECTIONS
{
    . = 0x400000;

    .text : ALIGN(4K) {
        *(.text.startup)
        *(.text .text.*)
    }
    .rodata : ALIGN(4K) { *(.rodata .rodata.*) }
    .data   : ALIGN(4K) { *(.data .data.*) }
    .bss    : ALIGN(4K) { *(.bss .bss.*) *(COMMON) }

    /DISCARD/ : { *(.comment) *(.note.*) *(.eh_frame*) }
}
