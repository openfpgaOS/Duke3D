/*
 * openfpgaOS Application Linker Script
 *
 * Supports optional BRAM hot-path: mark functions with OF_FASTTEXT
 * to place them in on-chip BRAM (~51KB, zero wait state).
 * Apps that don't use OF_FASTTEXT are unaffected — BRAM sections are empty.
 *
 * The OS ELF loader detects BRAM-targeted PT_LOAD segments and copies
 * them to BRAM at load time.
 */

ENTRY(_start)

MEMORY {
    APP_BRAM (rwx) : ORIGIN = 0x00002000, LENGTH = 0xDC00
    SDRAM    (rwx) : ORIGIN = 0x10400000, LENGTH = 48M
}

SECTIONS {
    . = 0x10400000;

    .text : {
        *(.text.start)
        *(.text*)
        *(.rodata*)
        *(.srodata*)
        . = ALIGN(4);
    } > SDRAM

    .data : {
        *(.data*)
        *(.sdata*)
        . = ALIGN(4);
    } > SDRAM

    .dynamic : {
        *(.dynamic)
    } > SDRAM

    .rela.dyn : {
        *(.rela*)
    } > SDRAM

    .bss : {
        __bss_start = .;
        *(.bss*)
        *(.sbss*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > SDRAM

    /* BRAM hot code: VMA in BRAM, LMA stored in ELF after SDRAM sections.
     * Empty for apps that don't use OF_FASTTEXT — no PT_LOAD generated. */
    .app_fasttext : {
        *(.app_fasttext*)
        *(.app_fastrodata*)
    } > APP_BRAM AT> SDRAM

    .app_fastdata : {
        *(.app_fastdata*)
    } > APP_BRAM AT> SDRAM
}
