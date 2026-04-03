#include "test.h"

void test_psram_memory(void) {
    section_start("PSRAM Mem");

    volatile uint32_t *cram0 = (volatile uint32_t *)0x30800000;
    volatile uint32_t *cram1 = (volatile uint32_t *)0x31000000;
    volatile uint32_t *sram  = (volatile uint32_t *)0x3A000000;

    cram0[0] = 0xDEADBEEF;
    cram0[1] = 0xCAFEBABE;
    ASSERT("cram0 w/r[0]", cram0[0] == 0xDEADBEEF);
    ASSERT("cram0 w/r[1]", cram0[1] == 0xCAFEBABE);

    {
        int ok = 1;
        for (int i = 0; i < 32; i++)
            cram0[i] = 1u << i;
        for (int i = 0; i < 32; i++) {
            if (cram0[i] != (1u << i)) { ok = 0; break; }
        }
        ASSERT("cram0 walk1", ok);
    }

    {
        int ok = 1;
        for (uint32_t i = 0; i < 16384; i++)
            cram0[i] = i ^ 0x5A5A5A5A;
        for (uint32_t i = 0; i < 16384; i++) {
            if (cram0[i] != (i ^ 0x5A5A5A5A)) { ok = 0; break; }
        }
        ASSERT("cram0 64K", ok);
    }

    {
        for (uint32_t i = 0; i < 16384; i++)
            cram0[i] = i;

        uint32_t t0 = of_time_us();
        volatile uint32_t sum = 0;
        for (uint32_t i = 0; i < 16384; i++)
            sum += cram0[i];
        uint32_t t1 = of_time_us();
        (void)sum;

        uint32_t elapsed_us = t1 - t0;
        ASSERT("cram0 speed", elapsed_us < 100000);
        (void)elapsed_us;
    }

    cram1[0] = 0x12345678;
    cram1[1] = 0x9ABCDEF0;
    ASSERT("cram1 w/r[0]", cram1[0] == 0x12345678);
    ASSERT("cram1 w/r[1]", cram1[1] == 0x9ABCDEF0);

    {
        int ok = 1;
        for (uint32_t i = 0; i < 4096; i++)
            cram1[i] = ~i;
        for (uint32_t i = 0; i < 4096; i++) {
            if (cram1[i] != ~i) { ok = 0; break; }
        }
        ASSERT("cram1 16K", ok);
    }

    sram[0] = 0xFEEDFACE;
    sram[1] = 0xBAADF00D;
    ASSERT("sram w/r[0]", sram[0] == 0xFEEDFACE);
    ASSERT("sram w/r[1]", sram[1] == 0xBAADF00D);

    {
        int ok = 1;
        for (int i = 0; i < 256; i++)
            sram[i] = (uint32_t)i * 0x01010101;
        for (int i = 0; i < 256; i++) {
            if (sram[i] != (uint32_t)i * 0x01010101) { ok = 0; break; }
        }
        ASSERT("sram 1K", ok);
    }

    {
        cram1[0] = 0x11111111;
        cram0[0] = 0x22222222;
        ASSERT("isolation c1", cram1[0] == 0x11111111);
        ASSERT("isolation c0", cram0[0] == 0x22222222);
    }

    {
        volatile uint8_t *cram0_b = (volatile uint8_t *)0x30800000;
        cram0[0] = 0x00000000;
        cram0_b[1] = 0xAB;
        ASSERT("byte write", (cram0[0] & 0x0000FF00) == 0x0000AB00);
    }

    {
        int ok = 1;
        for (int i = 0; i < 256; i++) {
            cram0[i] = (uint32_t)i;
            cram1[i] = (uint32_t)(i + 0x1000);
        }
        for (int i = 0; i < 256; i++) {
            if (cram0[i] != (uint32_t)i) { ok = 0; break; }
            if (cram1[i] != (uint32_t)(i + 0x1000)) { ok = 0; break; }
        }
        ASSERT("interleave", ok);
    }

    section_end();
}
