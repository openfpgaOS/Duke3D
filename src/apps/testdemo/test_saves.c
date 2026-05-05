#include "test.h"

void test_saves(void) {
    section_start("Saves");

    /* Use slot 9 (last slot) to avoid clobbering real save data */

    /* Erase slot: write 256 bytes of 0xFF */
    {
        FILE *ef = fopen("save:9", "wb");
        if (ef) {
            uint8_t ff[256];
            memset(ff, 0xFF, sizeof(ff));
            fwrite(ff, 1, sizeof(ff), ef);
            fclose(ef);
        }
    }

    /* Verify erased (should be 0xFF) via fopen */
    {
        FILE *f = fopen("save:9", "rb");
        ASSERT("erase open", f != NULL);
        if (f) {
            uint8_t buf[16];
            size_t n = fread(buf, 1, 16, f);
            ASSERT_EQ("erase read", (int)n, 16);
            int erased = 1;
            for (int i = 0; i < 16; i++)
                if (buf[i] != 0xFF) erased = 0;
            ASSERT("erased 0xFF", erased);
            fclose(f);
        }
    }

    /* Write pattern via fopen */
    uint8_t pattern[32];
    for (int i = 0; i < 32; i++)
        pattern[i] = (uint8_t)(i * 7 + 0x42);
    {
        FILE *f = fopen("save:9", "r+b");
        if (!f) f = fopen("save:9", "wb");
        ASSERT("write open", f != NULL);
        if (f) {
            size_t n = fwrite(pattern, 1, 32, f);
            ASSERT_EQ("write rc", (int)n, 32);
            fclose(f);
        }
    }

    /* Read back and verify */
    {
        FILE *f = fopen("save:9", "rb");
        ASSERT("readback open", f != NULL);
        if (f) {
            uint8_t readback[32];
            size_t n = fread(readback, 1, 32, f);
            ASSERT_EQ("read rc", (int)n, 32);
            ASSERT("read match", memcmp(pattern, readback, 32) == 0);
            fclose(f);
        }
    }

    /* Write at offset */
    {
        uint8_t mid[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
        FILE *f = fopen("save:9", "r+b");
        ASSERT("mid open", f != NULL);
        if (f) {
            fseek(f, 100, SEEK_SET);
            fwrite(mid, 1, 4, f);
            fclose(f);
        }
        uint8_t midread[4];
        f = fopen("save:9", "rb");
        if (f) {
            fseek(f, 100, SEEK_SET);
            fread(midread, 1, 4, f);
            ASSERT("offset write", memcmp(mid, midread, 4) == 0);
            fclose(f);
        }

        /* Original data at offset 0 still intact */
        f = fopen("save:9", "rb");
        if (f) {
            uint8_t readback[32];
            fread(readback, 1, 32, f);
            ASSERT("no clobber", memcmp(pattern, readback, 32) == 0);
            fclose(f);
        }
    }

    /* fopen("save:N") path -- read back the pattern we wrote */
    {
        FILE *f = fopen("save:9", "rb");
        ASSERT("fopen save", f != NULL);
        if (f) {
            uint8_t fbuf[32];
            size_t n = fread(fbuf, 1, 32, f);
            ASSERT_EQ("fread save", (int)n, 32);
            ASSERT("fread match", memcmp(pattern, fbuf, 32) == 0);
            fclose(f);
        }
    }

    /* Clean up -- erase test slot: write 256 bytes of 0xFF */
    {
        FILE *ef = fopen("save:9", "wb");
        if (ef) {
            uint8_t ff[256];
            memset(ff, 0xFF, sizeof(ff));
            fwrite(ff, 1, sizeof(ff), ef);
            fclose(ef);
        }
    }

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

    /* Duke3D pattern: write placeholder header + payload, close, then
     * reopen r+b to patch data_size and magic. This catches regressions
     * where the first close persists but the header backfill is lost. */
    {
        enum { DUKE_HEADER = 20, DUKE_PAYLOAD = 48 };
        const uint32_t duke_magic = 0x444B5356u; /* "DKSV" */
        const uint32_t duke_size = DUKE_HEADER + DUKE_PAYLOAD;
        uint8_t header[DUKE_HEADER] = {0};
        uint8_t payload[DUKE_PAYLOAD];
        uint8_t check_payload[DUKE_PAYLOAD];
        uint32_t got_size = 0;
        uint32_t got_magic = 0;

        for (int i = 0; i < DUKE_PAYLOAD; i++)
            payload[i] = (uint8_t)(0x30 + i * 3);

        f = fopen("save_9", "wb");
        ASSERT("duke wb open", f != NULL);
        if (f) {
            ASSERT("duke hdr reserve",
                   fwrite(header, 1, DUKE_HEADER, f) == DUKE_HEADER);
            ASSERT("duke payload",
                   fwrite(payload, 1, DUKE_PAYLOAD, f) == DUKE_PAYLOAD);
            ASSERT("duke first close", fclose(f) == 0);
        }

        f = fopen("save_9", "r+b");
        ASSERT("duke r+b open", f != NULL);
        if (f) {
            ASSERT("duke size seek", fseek(f, 0, SEEK_SET) == 0);
            ASSERT("duke size write", fwrite(&duke_size, 4, 1, f) == 1);
            ASSERT("duke magic seek", fseek(f, 16, SEEK_SET) == 0);
            ASSERT("duke magic write", fwrite(&duke_magic, 4, 1, f) == 1);
            ASSERT("duke second close", fclose(f) == 0);
        }

        f = fopen("save_9", "rb");
        ASSERT("duke rb open", f != NULL);
        if (f) {
            ASSERT("duke size read", fread(&got_size, 4, 1, f) == 1);
            ASSERT("duke magic seek2", fseek(f, 16, SEEK_SET) == 0);
            ASSERT("duke magic read", fread(&got_magic, 4, 1, f) == 1);
            ASSERT("duke payload seek", fseek(f, DUKE_HEADER, SEEK_SET) == 0);
            ASSERT("duke payload read",
                   fread(check_payload, 1, DUKE_PAYLOAD, f) == DUKE_PAYLOAD);
            ASSERT("duke size match", got_size == duke_size);
            ASSERT("duke magic match", got_magic == duke_magic);
            ASSERT("duke payload match",
                   memcmp(payload, check_payload, DUKE_PAYLOAD) == 0);
            fclose(f);
        }
    }

    /* Clean up: write 256 bytes of 0xFF */
    {
        FILE *ef = fopen("save:9", "wb");
        if (ef) {
            uint8_t ff[256];
            memset(ff, 0xFF, sizeof(ff));
            fwrite(ff, 1, sizeof(ff), ef);
            fclose(ef);
        }
    }

    section_end();
}
