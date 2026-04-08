#include "test.h"
#include <time.h>

/* PSRAM / SRAM memory map (cached CPU-side addresses)
 * CRAM0 cached:    0x30000000 – 0x30FFFFFF (16 MB)
 * CRAM1 uncached:  0x39000000 – 0x39FFFFFF (16 MB, bridge-accessible)
 * SRAM:            0x3A000000 – 0x3A03FFFF (256 KB)
 *
 * Uncached mirror: base | 0x08000000  (bypasses D-cache)
 * e.g. CRAM0 uncached = 0x38000000
 */
#define CRAM0_CACHED_BASE   0x30000000  /* CRAM0 cached base */
#define CRAM0_UNCACHED_BASE 0x38000000  /* CRAM0 uncached base */
#define CRAM0_TEST_OFFSET   0x00800000  /* 8 MB offset — avoids slot table at base */
#define CRAM1_UNCACHED_BASE 0x39000000  /* CRAM1 uncached (bridge-accessible) */
#define SRAM_BASE           0x3A000000  /* On-chip SRAM */

/* SDRAM region used to evict D-cache lines (must not overlap app code/data) */
#define SDRAM_EVICT_BASE    0x13F00000

void test_psram_memory(void) {
    section_start("PSRAM Mem");

    volatile uint32_t *cram0 = (volatile uint32_t *)(CRAM0_CACHED_BASE + CRAM0_TEST_OFFSET);
    volatile uint32_t *cram1 = (volatile uint32_t *)CRAM1_UNCACHED_BASE;
    volatile uint32_t *sram  = (volatile uint32_t *)SRAM_BASE;

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
        volatile uint8_t *cram0_b = (volatile uint8_t *)(CRAM0_CACHED_BASE + CRAM0_TEST_OFFSET);
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

void test_cram0_256k(void) {
    section_start("CRAM0 256K");

    /* Uncached 32-bit write then read */
    {
        volatile uint32_t *u = (volatile uint32_t *)(CRAM0_UNCACHED_BASE + CRAM0_TEST_OFFSET);
        u[0] = 0xDEAD1234;
        __asm__ volatile("fence" ::: "memory");
        uint32_t v = u[0];
        ASSERT("uc w/r", v == 0xDEAD1234);
    }

    /* Write via cached alias, read via uncached alias to bypass D-cache.
     * This tests whether PSRAM writeback actually reaches the chip. */
    volatile uint32_t *cram0_c = (volatile uint32_t *)(CRAM0_CACHED_BASE + CRAM0_TEST_OFFSET);
    volatile uint32_t *cram0_u = (volatile uint32_t *)(CRAM0_UNCACHED_BASE + CRAM0_TEST_OFFSET);
    const uint32_t count = 256 * 1024 / 4;  /* 65536 words = 256KB */

    /* Write pass: XOR pattern via cached alias */
    for (uint32_t i = 0; i < count; i++)
        cram0_c[i] = i ^ 0xA5A5A5A5;

    /* Flush D-cache: read 128KB from SDRAM to evict all dirty lines */
    volatile char *evict = (volatile char *)SDRAM_EVICT_BASE;
    for (uint32_t i = 0; i < 131072; i += 64)
        (void)evict[i];
    __asm__ volatile("fence" ::: "memory");

    /* Read-back via UNCACHED alias — goes directly to PSRAM */
    {
        int ok = 1;
        uint32_t fail_addr = 0, fail_exp = 0, fail_got = 0;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t exp = i ^ 0xA5A5A5A5;
            uint32_t got = cram0_u[i];
            if (got != exp) {
                ok = 0;
                fail_addr = i;
                fail_exp = exp;
                fail_got = got;
                break;
            }
        }
        if (ok) {
            test_pass("xor pattern");
        } else {
            snprintf(__buf, sizeof(__buf),
                     "@%lu exp %08lx go %08lx",
                     (unsigned long)fail_addr,
                     (unsigned long)fail_exp,
                     (unsigned long)fail_got);
            test_fail("xor pattern", __buf);
        }
    }

    /* Write pass 2: inverted index via cached */
    for (uint32_t i = 0; i < count; i++)
        cram0_c[i] = ~i;

    /* Flush D-cache again */
    for (uint32_t i = 0; i < 131072; i += 64)
        (void)evict[i];
    __asm__ volatile("fence" ::: "memory");

    /* Read-back pass 2 via uncached */
    {
        int ok = 1;
        uint32_t fail_addr = 0, fail_exp = 0, fail_got = 0;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t exp = ~i;
            uint32_t got = cram0_u[i];
            if (got != exp) {
                ok = 0;
                fail_addr = i;
                fail_exp = exp;
                fail_got = got;
                break;
            }
        }
        if (ok) {
            test_pass("inv pattern");
        } else {
            snprintf(__buf, sizeof(__buf),
                     "@%lu exp %08lx go %08lx",
                     (unsigned long)fail_addr,
                     (unsigned long)fail_exp,
                     (unsigned long)fail_got);
            test_fail("inv pattern", __buf);
        }
    }

    section_end();
}
