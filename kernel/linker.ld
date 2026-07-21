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

/* The kernel runs at KERNEL_VMA but is loaded at KERNEL_LMA. Every section
 * carries an AT() so its load address is physical while its symbols resolve
 * to the higher half - objcopy -O binary lays the image out by load address,
 * which is exactly what the bootloader copies to 1 MiB.
 *
 * Must agree with boot/layout.inc and kernel/include/leah/memory.hpp. */
KERNEL_VMA = 0xFFFFFFFF80100000;
KERNEL_LMA = 0x100000;

SECTIONS
{
    . = KERNEL_VMA;

    __kernel_start = .;

    .text : AT(ADDR(.text) - KERNEL_VMA + KERNEL_LMA) ALIGN(4K)
    {
        *(.text.entry)
        *(.text .text.*)
    }

    .rodata : AT(ADDR(.rodata) - KERNEL_VMA + KERNEL_LMA) ALIGN(4K)
    {
        *(.rodata .rodata.*)
    }

    .data : AT(ADDR(.data) - KERNEL_VMA + KERNEL_LMA) ALIGN(4K)
    {
        *(.data .data.*)
    }

    /* Constructors for objects with static storage duration. KEEP stops
     * --gc-sections from discarding them: nothing references these pointers,
     * they are found by walking the section. Without this the constructors are
     * emitted and then silently never run. */
    .init_array : AT(ADDR(.init_array) - KERNEL_VMA + KERNEL_LMA) ALIGN(8)
    {
        __init_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array))
        KEEP(*(.ctors))
        __init_array_end = .;
    }

    /* NOBITS, so objcopy leaves it out of the flat image entirely - the entry
     * stub is responsible for zeroing it before any C++ runs. */
    .bss : AT(ADDR(.bss) - KERNEL_VMA + KERNEL_LMA) ALIGN(4K)
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
