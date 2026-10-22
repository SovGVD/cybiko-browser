/* One image for a CyOS .e module: code, read-only data and data from 0,
 * then the zeroed data (bss), whose size goes in the module header.
 * cybiko_elink.py links this twice at different bases to find the words
 * the loader must relocate. */
OUTPUT_FORMAT("coff-h8300")
OUTPUT_ARCH(h8300s)
SECTIONS
{
    .text : {
        *(.text)
        *(.rodata)
        *(.rdata)
        *(.data)
        . = ALIGN(4);
        __image_end = .;
    }
    .bss : {
        __bss_start = .;
        *(.bss)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    }
    /DISCARD/ : { *(.comment) *(.stab) *(.stabstr) }
}
