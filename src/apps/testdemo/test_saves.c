#include "test.h"

void test_saves(void) {
    section_start("Saves");

    /* Use slot 9 (last slot) to avoid clobbering real save data */
    int slot = 9;

    /* Erase slot */
    of_save_erase(slot);

    /* Verify erased (should be 0xFF) */
    uint8_t buf[16];
    int rc = of_save_read(slot, buf, 0, 16);
    ASSERT_EQ("erase read", rc, 16);
    int erased = 1;
    for (int i = 0; i < 16; i++)
        if (buf[i] != 0xFF) erased = 0;
    ASSERT("erased 0xFF", erased);

    /* Write pattern */
    uint8_t pattern[32];
    for (int i = 0; i < 32; i++)
        pattern[i] = (uint8_t)(i * 7 + 0x42);
    rc = of_save_write(slot, pattern, 0, 32);
    ASSERT_EQ("write rc", rc, 32);

    /* Read back and verify */
    uint8_t readback[32];
    rc = of_save_read(slot, readback, 0, 32);
    ASSERT_EQ("read rc", rc, 32);
    ASSERT("read match", memcmp(pattern, readback, 32) == 0);

    /* Write at offset */
    uint8_t mid[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
    of_save_write(slot, mid, 100, 4);
    uint8_t midread[4];
    of_save_read(slot, midread, 100, 4);
    ASSERT("offset write", memcmp(mid, midread, 4) == 0);

    /* Original data at offset 0 still intact */
    of_save_read(slot, readback, 0, 32);
    ASSERT("no clobber", memcmp(pattern, readback, 32) == 0);

    /* Flush — triggers bridge Data Slot Write (CRAM1 → SD card) */
    of_save_flush(slot);
    test_pass("flush");

    /* Boundary: write at end of 256KB slot */
    uint8_t edge[4] = { 0xCA, 0xFE, 0xBA, 0xBE };
    rc = of_save_write(slot, edge, 0x40000 - 4, 4);
    ASSERT_EQ("edge write", rc, 4);
    uint8_t edgeread[4];
    of_save_read(slot, edgeread, 0x40000 - 4, 4);
    ASSERT("edge read", memcmp(edge, edgeread, 4) == 0);

    /* Out of bounds should fail */
    rc = of_save_write(slot, edge, 0x40000, 4);
    ASSERT_EQ("oob write", rc, -1);
    rc = of_save_read(slot, edgeread, 0x40000, 4);
    ASSERT_EQ("oob read", rc, -1);

    /* Invalid slot */
    rc = of_save_read(10, buf, 0, 16);
    ASSERT_EQ("bad slot", rc, -1);

    /* fopen("save:N") path */
    FILE *f = fopen("save:9", "rb");
    ASSERT("fopen save", f != NULL);
    if (f) {
        uint8_t fbuf[32];
        size_t n = fread(fbuf, 1, 32, f);
        ASSERT_EQ("fread save", (int)n, 32);
        ASSERT("fread match", memcmp(pattern, fbuf, 32) == 0);
        fclose(f);
    }

    /* Clean up — erase test slot */
    of_save_erase(slot);

    section_end();
}

void test_posix_saves(void) {
    section_start("POSIX Saves");

    /* Write via fopen("save_N") */
    FILE *f = fopen("save_9", "wb");
    ASSERT("fopen wb", f != NULL);
    if (f) {
        uint8_t data[64];
        for (int i = 0; i < 64; i++) data[i] = (uint8_t)(i ^ 0x55);
        size_t n = fwrite(data, 1, 64, f);
        ASSERT_EQ("fwrite", (int)n, 64);
        fclose(f);  /* auto-flush with write_max=64 */
    }

    /* Read back via fopen("save_9") */
    f = fopen("save_9", "rb");
    ASSERT("fopen rb", f != NULL);
    if (f) {
        uint8_t rbuf[64];
        size_t n = fread(rbuf, 1, 64, f);
        ASSERT_EQ("fread", (int)n, 64);
        int ok = 1;
        for (int i = 0; i < 64; i++)
            if (rbuf[i] != (uint8_t)(i ^ 0x55)) { ok = 0; break; }
        ASSERT("data match", ok);
        fclose(f);
    }

    /* Also works with save: prefix */
    f = fopen("save:9", "rb");
    ASSERT("save:9 rb", f != NULL);
    if (f) {
        uint8_t rbuf[4];
        ASSERT("save:9 read", fread(rbuf, 1, 4, f) == 4);
        ASSERT("save:9 data", rbuf[0] == 0x55);
        fclose(f);
    }

    /* Negative: save_10 should fail */
    ASSERT("save_10 null", fopen("save_10", "wb") == NULL);

    /* Edge: write 0 bytes then close — no flush */
    f = fopen("save_9", "wb");
    if (f) fclose(f);
    test_pass("empty close");

    /* Edge: write 1 byte */
    f = fopen("save_9", "wb");
    if (f) {
        uint8_t one = 0xAA;
        fwrite(&one, 1, 1, f);
        fclose(f);
    }
    f = fopen("save_9", "rb");
    if (f) {
        uint8_t check;
        fread(&check, 1, 1, f);
        ASSERT("1byte save", check == 0xAA);
        fclose(f);
    }

    /* Clean up */
    of_save_erase(9);

    section_end();
}
