/* GNU-EFI linker script for x86_64 UEFI application (PE32+).
 * Standard GNU-EFI layout: .text, .sdata, .data, .dynamic, .rela, .reloc.
 * The .reloc section is required for a relocatable UEFI image.
 */

PHDRS
{
    text    PT_LOAD FLAGS(5);   /* R E */
    data    PT_LOAD FLAGS(6);   /* R W */
    dynamic PT_DYNAMIC;
}

SECTIONS
{
    . = 0;
    ImageBase = .;

    .text : {
        *(.text .text.*)
        *(.got .got.*)
        *(.rodata .rodata.*)
        *(.eh_frame)
    } :text

    . = ALIGN(4096);

    .sdata : {
        *(.sdata .sdata.*)
    } :data

    . = ALIGN(4096);

    .data : {
        *(.data .data.*)
        *(.sdata2 .sdata2.*)
    } :data

    . = ALIGN(4096);
    _DYNAMIC = .;
    .dynamic : {
        *(.dynamic)
    } :data

    . = ALIGN(4096);

    .rela : {
        *(.rela .rela.*)
    } :data

    . = ALIGN(4096);

    .reloc : {
        KEEP(*(.reloc))
    } :data

    . = ALIGN(4096);

    .dynsym : {
        *(.dynsym)
    } :data

    . = ALIGN(4096);

    .dynstr : {
        *(.dynstr)
    } :data

    /DISCARD/ : {
        *(.note.GNU-stack)
        *(.comment)
        *(.debug*)
    }
}
