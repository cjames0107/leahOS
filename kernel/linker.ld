/* leahOS kernel image layout.
 *
 * The bootloader copies a flat binary to 1 MiB and jumps to offset 0, so
 * .text.entry must come first and the load address must match
 * KERNEL_PHYS_BASE in boot/layout.inc.
 *
 * This is deliberately an identity-mapped, low-half kernel for now. Moving to
 * the canonical higher half at 0xFFFFFFFF80000000 comes once the VMM exists.
 */

ENTRY(_start)

SECTIONS
{
    . = 0x100000;

    __kernel_start = .;

    .text : ALIGN(4K)
    {
        *(.text.entry)
        *(.text .text.*)
    }

    .rodata : ALIGN(4K)
    {
        *(.rodata .rodata.*)
    }

    .data : ALIGN(4K)
    {
        *(.data .data.*)
    }

    /* NOBITS, so objcopy leaves it out of the flat image entirely - the entry
     * stub is responsible for zeroing it before any C++ runs. */
    .bss : ALIGN(4K)
    {
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        . = ALIGN(8);
        __bss_end = .;
    }

    __kernel_end = .;

    /DISCARD/ :
    {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame_hdr)
    }
}
