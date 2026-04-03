/*
 * openfpgaOS Slot Demo
 * Displays registered file slots with a box-drawn table.
 */

#include "of.h"
#include <stdio.h>

static void draw_hline(int w, int sep, char left, char mid, char right) {
    printf("  %c", left);
    for (int i = 1; i < w - 1; i++)
        printf("%c", (i == sep) ? mid : ACS_HLINE);
    printf("%c\n", right);
}

/* Chip32 slot table in CRAM (0x30000000, written by loader via bridge 0x20000000)
 * Format per entry: [0] magic 0x534C4F54 ("SLOT")
 *                   [4] slot_id
 *                   [8] status (0=missing, other=file size)
 *                   [12] reserved
 * End marker: 0xFFFFFFFF */
#define CRAM_TABLE_BASE  0x30000000
#define SLOT_MAGIC       0x534C4F54

typedef struct {
    uint32_t magic;
    uint32_t slot_id;
    uint32_t status;
    uint32_t reserved;
} chip32_slot_entry_t;

int main(void) {
    of_file_slot_register(1, "os.bin");
    of_file_slot_register(2, "slotdemo.elf");

    printf("\033[2J\033[H");
    printf("\n Bridge Write Test\n\n");

    /* Chip32→SDRAM test (bridge 0x00100000 → CPU 0x10100000) */
    volatile uint32_t *sd = (volatile uint32_t *)0x10100000;
    printf(" C32>SDRAM: %08X %08X %s\n",
           (unsigned)sd[0], (unsigned)sd[1],
           (sd[0] == 0xC32D4A4D) ? "OK" : "FAIL");

    /* Chip32→CRAM test (bridge 0x20000000 → CPU 0x30000000) */
    volatile chip32_slot_entry_t *t = (volatile chip32_slot_entry_t *)CRAM_TABLE_BASE;
    printf(" C32>CRAM:  %08X %08X %s\n",
           (unsigned)t[0].magic, (unsigned)t[0].slot_id,
           (t[0].magic == SLOT_MAGIC) ? "OK" : "FAIL");

    /* CPU→CRAM0 test (bank 0) */
    volatile uint32_t *ct = (volatile uint32_t *)0x30000400;
    ct[0] = 0xDEADBEEF;
    ct[1] = 0xCAFEBABE;
    __asm__ volatile("fence" ::: "memory");
    printf(" CPU>CRAM0: %08X %08X %s\n",
           (unsigned)ct[0], (unsigned)ct[1],
           (ct[0] == 0xDEADBEEF) ? "OK" : "FAIL");

    /* CPU→CRAM1 save region test (bank 1, uncached 0x39000000) */
    volatile uint32_t *sv = (volatile uint32_t *)0x39000000;
    printf("\n CRAM1 Save Region Tests:\n");

    /* 32-bit write/read */
    sv[0] = 0x12345678;
    sv[1] = 0xAABBCCDD;
    __asm__ volatile("fence" ::: "memory");
    printf(" W32 @+0: %08X %s\n", (unsigned)sv[0],
           sv[0] == 0x12345678 ? "OK" : "FAIL");
    printf(" W32 @+4: %08X %s\n", (unsigned)sv[1],
           sv[1] == 0xAABBCCDD ? "OK" : "FAIL");

    /* Byte write/read (tests wstrb across both CRAM chips) */
    volatile uint8_t *sb = (volatile uint8_t *)0x39000000;
    sb[0] = 0x53;  /* byte 0 → CRAM0 low */
    sb[1] = 0x56;  /* byte 1 → CRAM0 high */
    sb[2] = 0xAA;  /* byte 2 → CRAM1 low */
    sb[3] = 0xBB;  /* byte 3 → CRAM1 high */
    __asm__ volatile("fence" ::: "memory");
    int byte_ok = (sb[0]==0x53 && sb[1]==0x56 && sb[2]==0xAA && sb[3]==0xBB);
    printf(" Byte: %02X %02X %02X %02X %s\n",
           sb[0], sb[1], sb[2], sb[3], byte_ok ? "OK" : "FAIL");

    /* Word readback after byte writes */
    printf(" Word: %08X (exp 00BB00AA 5653)\n", (unsigned)sv[0]);

    /* Different offset in save region (slot 1 base) */
    volatile uint32_t *sv1 = (volatile uint32_t *)0x39040000;
    sv1[0] = 0xFEEDFACE;
    __asm__ volatile("fence" ::: "memory");
    printf(" Slot1: %08X %s\n", (unsigned)sv1[0],
           sv1[0] == 0xFEEDFACE ? "OK" : "FAIL");

    /* Bank isolation: CRAM0 shouldn't see CRAM1 data */
    volatile uint32_t *c0 = (volatile uint32_t *)0x30000000;
    printf(" Bank0@0: %08X (should NOT be 12345678)\n", (unsigned)c0[0]);

    while (1)
        of_delay_ms(100);

    return 0;
}
