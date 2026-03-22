/*
 * openfpgaOS Application Linker Script
 * Produces a static-PIE ELF at virtual base 0 (relocated by OS loader)
 */

ENTRY(_start)

SECTIONS {
    . = 0x10400000;

    .text : {
        *(.text.start)
        *(.text*)
        *(.rodata*)
        *(.srodata*)
        . = ALIGN(4);
    }

    .data : {
        *(.data*)
        *(.sdata*)
        . = ALIGN(4);
    }

    .dynamic : {
        *(.dynamic)
    }

    .rela.dyn : {
        *(.rela*)
    }

    .bss : {
        __bss_start = .;
        *(.bss*)
        *(.sbss*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    }
}
