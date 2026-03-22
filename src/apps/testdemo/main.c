/*
 * openfpgaOS Kernel Test Suite
 * Tests syscalls, malloc, file I/O, terminal, timer.
 * Runs multiple iterations to catch intermittent issues.
 */

#include "of.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int pass_count, fail_count, section_pass;
static char __buf[80];

static void test_pass(const char *name) { (void)name; pass_count++; }

static void test_fail(const char *name, const char *detail) {
    printf("  \033[91mFAIL\033[0m %s: %s\n", name, detail);
    fail_count++;
}

static void section_start(const char *name) {
    printf("  \033[96m%-14s\033[0m ", name);
    section_pass = pass_count;
}

static void section_end(void) {
    int n = pass_count - section_pass;
    if (fail_count == 0)
        printf("\033[92m%d ok\033[0m\n", n);
    else
        printf("\n");
}

#define ASSERT(name, cond) do { \
    if (cond) test_pass(name); \
    else test_fail(name, #cond); \
} while(0)

#define ASSERT_EQ(name, a, b) do { \
    if ((a) == (b)) test_pass(name); \
    else { \
        snprintf(__buf, sizeof(__buf), "%d != %d", (int)(a), (int)(b)); \
        test_fail(name, __buf); \
    } \
} while(0)

/* --- Timer --- */
static void test_timer(void) {
    section_start("Timer");
    uint32_t t1 = of_time_ms();
    ASSERT("nonzero", t1 > 0);
    of_delay_ms(50);
    uint32_t elapsed = of_time_ms() - t1;
    ASSERT("delay", elapsed >= 40 && elapsed < 200);
    ASSERT("time_us", of_time_us() > 0);
    section_end();
}

/* --- Malloc --- */
static void test_malloc(void) {
    section_start("Malloc");

    void *p = malloc(64);
    ASSERT("64B", p != NULL);
    if (p) { memset(p, 0xAA, 64); ASSERT("write", ((unsigned char *)p)[63] == 0xAA); free(p); }

    void *c = calloc(10, 4);
    ASSERT("calloc", c != NULL);
    if (c) { int ok = 1; for (int i = 0; i < 40; i++) if (((unsigned char *)c)[i]) ok = 0; ASSERT("zeroed", ok); free(c); }

    void *r = malloc(16);
    if (r) { memset(r, 0x55, 16); void *r2 = realloc(r, 256); ASSERT("realloc", r2 && ((unsigned char *)r2)[0] == 0x55); if (r2) free(r2); }

    /* Size ladder */
    void *p1 = malloc(1024);        ASSERT("1K", p1 != NULL);
    void *p2 = malloc(64 * 1024);   ASSERT("64K", p2 != NULL);
    void *p3 = malloc(1024 * 1024); ASSERT("1M", p3 != NULL);
    void *p4 = malloc(4*1024*1024); ASSERT("4M", p4 != NULL);
    void *p8 = malloc(8*1024*1024); ASSERT("8M", p8 != NULL);
    if (p8) free(p8);
    free(p1); free(p2); free(p3); free(p4);

    /* Reuse after free */
    void *p5 = malloc(2 * 1024 * 1024);
    ASSERT("2M reuse", p5 != NULL);
    if (p5) { ((unsigned char *)p5)[2*1024*1024-1] = 0xBB; ASSERT("2M write", ((unsigned char *)p5)[2*1024*1024-1] == 0xBB); free(p5); }

    /* Misaligned access tests (trap handler emulation) */
    {
        uint8_t mb[32] __attribute__((aligned(8)));
        for (int i = 0; i < 16; i++) mb[i] = (uint8_t)(0x11 * (i + 1));
        /* mb = 11 22 33 44 55 66 77 88 99 AA BB CC DD EE FF 10 */

        /* 32-bit reads at all misaligned offsets */
        /* mb[0..7] = 0x11 0x22 0x33 0x44 0x55 0x66 0x77 0x88
         * Little-endian lw from &mb[N] = mb[N] | mb[N+1]<<8 | mb[N+2]<<16 | mb[N+3]<<24 */
        ASSERT("lw +1", *(volatile uint32_t *)&mb[1] == 0x55443322);
        ASSERT("lw +2", *(volatile uint32_t *)&mb[2] == 0x66554433);
        ASSERT("lw +3", *(volatile uint32_t *)&mb[3] == 0x77665544);

        /* 16-bit reads at odd offset */
        ASSERT("lh +1", *(volatile uint16_t *)&mb[1] == 0x3322);
        ASSERT("lh +3", *(volatile uint16_t *)&mb[3] == 0x5544);

        /* 32-bit writes at all misaligned offsets */
        mb[0]=0x11; mb[1]=0x22; mb[2]=0x33; mb[3]=0x44;
        mb[4]=0x55; mb[5]=0x66; mb[6]=0x77; mb[7]=0x88;

        *(volatile uint32_t *)&mb[1] = 0xDEADBEEF;
        ASSERT("sw +1", mb[1]==0xEF && mb[2]==0xBE && mb[3]==0xAD && mb[4]==0xDE);
        ASSERT("sw +1 nocrush", mb[0]==0x11 && mb[5]==0x66);

        mb[0]=0x11; mb[1]=0x22; mb[2]=0x33; mb[3]=0x44;
        mb[4]=0x55; mb[5]=0x66; mb[6]=0x77; mb[7]=0x88;

        *(volatile uint32_t *)&mb[2] = 0xCAFEBABE;
        ASSERT("sw +2", mb[2]==0xBE && mb[3]==0xBA && mb[4]==0xFE && mb[5]==0xCA);
        ASSERT("sw +2 nocrush", mb[1]==0x22 && mb[6]==0x77);

        mb[0]=0x11; mb[1]=0x22; mb[2]=0x33; mb[3]=0x44;
        mb[4]=0x55; mb[5]=0x66; mb[6]=0x77; mb[7]=0x88;

        *(volatile uint32_t *)&mb[3] = 0x12345678;
        ASSERT("sw +3", mb[3]==0x78 && mb[4]==0x56 && mb[5]==0x34 && mb[6]==0x12);
        ASSERT("sw +3 nocrush", mb[2]==0x33 && mb[7]==0x88);

        /* 16-bit write at odd offset */
        mb[0]=0x11; mb[1]=0x22; mb[2]=0x33; mb[3]=0x44;

        *(volatile uint16_t *)&mb[1] = 0xABCD;
        ASSERT("sh +1", mb[1]==0xCD && mb[2]==0xAB);
        ASSERT("sh +1 nocrush", mb[0]==0x11 && mb[3]==0x44);

        /* Struct-like pattern (Duke3D GRP index) */
        uint8_t grp[16] __attribute__((aligned(4)));
        memcpy(grp, "FILENAMEXT  ", 12);
        grp[12] = 0x00; grp[13] = 0x10; grp[14] = 0x00; grp[15] = 0x00;
        uint32_t file_size_val = *(volatile uint32_t *)&grp[12];
        ASSERT("grp idx", file_size_val == 0x00001000);
    }

    /* Find max alloc */
    size_t max_sz = 0;
    for (size_t sz = 1024*1024; sz <= 48*1024*1024; sz += 1024*1024) {
        void *q = malloc(sz); if (!q) break; max_sz = sz; free(q);
    }
    snprintf(__buf, sizeof(__buf), "max %dMB", (int)(max_sz/(1024*1024)));
    printf("[%s] ", __buf);

    /* Stress */
    static const size_t sizes[] = { 32, 64, 128, 256, 512, 1024, 4096, 16384, 65536 };
    int ok = 1;
    for (int i = 0; i < 500; i++) {
        size_t sz = sizes[i % 9];
        void *q = malloc(sz); if (!q) { ok = 0; break; }
        ((unsigned char *)q)[sz-1] = (unsigned char)i; free(q);
    }
    ASSERT("500x stress", ok);

    section_end();
}

/* --- File Slots --- */
static int slots_registered;
static void test_file_slots(void) {
    section_start("File Slots");
    if (!slots_registered) {
        of_file_slot_register(3, "test.dat");
        of_file_slot_register(4, "other.bin");
        of_file_slot_register(1, "os.bin");
        slots_registered = 1;
    }
    ASSERT("count", of_file_slot_count() >= 3);
    of_file_slot_t slot;
    ASSERT_EQ("get", of_file_slot_get(0, &slot), 0);
    ASSERT("name", strcmp(slot.filename, "test.dat") == 0);
    ASSERT_EQ("oob", of_file_slot_get(99, &slot), -1);

    /* Wrong slot ID: register a file under a non-existent bridge slot.
     * fopen should succeed (kernel doesn't validate slot IDs at open),
     * but fread should fail or return 0 since the bridge has no data. */
    of_file_slot_register(55, "ghost.bin");
    ASSERT("count+1", of_file_slot_count() >= 4);
    {
        FILE *ghost = fopen("ghost.bin", "rb");
        ASSERT("ghost open", ghost != NULL);
        if (ghost) {
            unsigned char tmp[4];
            size_t n = fread(tmp, 1, 4, ghost);
            ASSERT("ghost read zero", n == 0);
            fclose(ghost);
        }
    }

    section_end();
}

/* --- File I/O --- */
static void test_file_io(void) {
    section_start("File I/O");

    /* Unregistered filenames should return NULL */
    ASSERT("bad name", fopen("nonexistent.xyz", "rb") == NULL);
    ASSERT("bad .grp", fopen("DUKE3D.GRP", "rb") == NULL);
    ASSERT("bad path", fopen("/data/file.bin", "rb") == NULL);
    ASSERT("empty", fopen("", "rb") == NULL);

    /* Invalid slot: opens ok but read fails (no data on bridge) */
    {
        FILE *bad = fopen("slot:99", "rb");
        if (bad) {
            unsigned char tmp[4];
            ASSERT("slot:99 read fail", fread(tmp, 1, 4, bad) == 0);
            fclose(bad);
        }
    }

    FILE *f = fopen("slot:1", "rb");
    ASSERT("slot:1", f != NULL);
    if (f) {
        unsigned char buf[64];
        ASSERT("read 4", fread(buf, 1, 4, f) == 4);
        fclose(f);
    }

    f = fopen("os.bin", "rb");
    ASSERT("os.bin", f != NULL);
    if (f) {
        unsigned char b1[64], b2[64];
        fread(b1, 1, 64, f);
        fseek(f, 0, SEEK_SET);
        fread(b2, 1, 64, f);
        ASSERT("seek match", memcmp(b1, b2, 64) == 0);
        ASSERT_EQ("ftell", (int)ftell(f), 64);
        fclose(f);
    }

    /* Repeated open/read/close — stress the fd table and bridge */
    int repeat_ok = 1;
    for (int i = 0; i < 20; i++) {
        f = fopen("slot:1", "rb");
        if (!f) { repeat_ok = 0; break; }
        unsigned char buf[16];
        size_t n = fread(buf, 1, 16, f);
        if (n != 16) { repeat_ok = 0; fclose(f); break; }
        fclose(f);
    }
    ASSERT("20x open/read/close", repeat_ok);

    /* Multiple files open simultaneously */
    FILE *f1 = fopen("slot:1", "rb");
    FILE *f2 = fopen("slot:2", "rb");
    ASSERT("multi f1", f1 != NULL);
    ASSERT("multi f2", f2 != NULL);
    if (f1 && f2) {
        unsigned char a[4], b[4];
        fread(a, 1, 4, f1);
        fread(b, 1, 4, f2);
        ASSERT("multi distinct", memcmp(a, b, 4) != 0);
    }
    if (f1) fclose(f1);
    if (f2) fclose(f2);

    /* Cache coherency test: fill buffer with known pattern,
     * then read file data over it, verify we see file data
     * (not stale cached pattern) */
    {
        static uint8_t coh_buf[4096];

        /* Fill with pattern — this goes into D-cache */
        memset(coh_buf, 0xAA, sizeof(coh_buf));
        ASSERT("pre-fill", coh_buf[0] == 0xAA && coh_buf[4095] == 0xAA);

        /* Read file data over the buffer via DMA */
        f = fopen("slot:1", "rb");
        if (f) {
            size_t n = fread(coh_buf, 1, 4096, f);
            fclose(f);

            /* Verify we see file data, not the 0xAA pattern.
             * slot:1 is os.bin — first bytes should NOT be 0xAA */
            ASSERT("coherency sz", n > 0);
            int stale = 1;
            for (int i = 0; i < 16; i++)
                if (coh_buf[i] != 0xAA) stale = 0;
            ASSERT("coherency !stale", !stale);

            /* Read again into same buffer to test second DMA */
            memset(coh_buf, 0x55, sizeof(coh_buf));
            f = fopen("slot:1", "rb");
            if (f) {
                fread(coh_buf, 1, 4096, f);
                fclose(f);
                int stale2 = 1;
                for (int i = 0; i < 16; i++)
                    if (coh_buf[i] != 0x55) stale2 = 0;
                ASSERT("coherency 2nd", !stale2);
            }

            /* Large read coherency: 64KB */
            static uint8_t big_coh[65536];
            memset(big_coh, 0xCC, sizeof(big_coh));
            f = fopen("slot:1", "rb");
            if (f) {
                fread(big_coh, 1, 65536, f);
                fclose(f);
                /* Check first, middle, and last cached regions */
                int ok = (big_coh[0] != 0xCC) &&
                         (big_coh[32768] != 0xCC) &&
                         (big_coh[65535] != 0xCC);
                ASSERT("coherency 64K", ok);
            }
        }
    }

    /* Sequential read coherency: read multiple chunks into same
     * buffer (like Duke3D reading GRP entries), verify each
     * read returns correct data, not stale from previous read */
    {
        static uint8_t seq_buf[4096];

        /* Read offset 0 */
        f = fopen("slot:1", "rb");
        ASSERT("seq open", f != NULL);
        if (f) {
            fread(seq_buf, 1, 64, f);
            uint8_t first[64];
            memcpy(first, seq_buf, 64);

            /* Read offset 64 into same buffer — should be different */
            fread(seq_buf, 1, 64, f);
            ASSERT("seq diff", memcmp(first, seq_buf, 64) != 0);

            /* Seek back and re-read offset 0 — must match original */
            fseek(f, 0, SEEK_SET);
            fread(seq_buf, 1, 64, f);
            ASSERT("seq reread", memcmp(first, seq_buf, 64) == 0);

            /* Many small sequential reads */
            int seq_ok = 1;
            fseek(f, 0, SEEK_SET);
            for (int i = 0; i < 100; i++) {
                size_t n = fread(seq_buf, 1, 32, f);
                if (n != 32) { seq_ok = 0; break; }
            }
            ASSERT("100x 32B reads", seq_ok);

            fclose(f);
        }
    }

    /* Compare direct DMA vs bounce buffer — both should
     * return identical data for the same file offset */
    {
        static uint8_t buf_a[4096];
        static uint8_t buf_b[4096];

        /* First read: goes through sys_read (direct DMA or bounce) */
        f = fopen("slot:1", "rb");
        if (f) {
            fread(buf_a, 1, 4096, f);
            fclose(f);
        }

        /* Second read: same data, same offset */
        f = fopen("slot:1", "rb");
        if (f) {
            fread(buf_b, 1, 4096, f);
            fclose(f);
        }

        ASSERT("dma consistency", memcmp(buf_a, buf_b, 4096) == 0);

        /* Third read after writing to buffer — tests cache eviction */
        memset(buf_a, 0xFF, 4096);
        f = fopen("slot:1", "rb");
        if (f) {
            fread(buf_a, 1, 4096, f);
            fclose(f);
        }
        ASSERT("dma after write", memcmp(buf_a, buf_b, 4096) == 0);
    }

    /* Direct DMA coherency: simulate Duke3D pattern —
     * write to buffer, then DMA over it, read back. Tests
     * that cache flush before/after DMA works correctly. */
    {
        static uint8_t dma_buf[65536];

        /* Write pattern that will be in D-cache */
        for (int i = 0; i < 65536; i++)
            dma_buf[i] = (uint8_t)(i & 0xFF);

        /* DMA file data over it (goes through direct DMA path) */
        f = fopen("slot:1", "rb");
        if (f) {
            fread(dma_buf, 1, 65536, f);
            fclose(f);

            /* Every byte must be file data, not our pattern.
             * Check multiple cache-line-aligned offsets */
            int ok = 1;
            for (int off = 0; off < 65536; off += 64) {
                if (dma_buf[off] == (uint8_t)(off & 0xFF)) {
                    /* Could be coincidence — check neighbors too */
                    if (dma_buf[off+1] == (uint8_t)((off+1) & 0xFF) &&
                        dma_buf[off+2] == (uint8_t)((off+2) & 0xFF)) {
                        ok = 0;  /* 3 matching bytes = stale cache line */
                        break;
                    }
                }
            }
            ASSERT("direct DMA 64K", ok);
        }

        /* Repeated lseek+read pattern (GRP file access) */
        f = fopen("slot:1", "rb");
        if (f) {
            uint8_t a[256], b[256], c[256];

            /* Read at offset 0 */
            fseek(f, 0, SEEK_SET);
            fread(a, 1, 256, f);

            /* Read at offset 1024 */
            fseek(f, 1024, SEEK_SET);
            fread(b, 1, 256, f);

            /* Read at offset 0 again — must match first read */
            fseek(f, 0, SEEK_SET);
            fread(c, 1, 256, f);
            ASSERT("seek+read match", memcmp(a, c, 256) == 0);

            /* Reads at different offsets must differ */
            ASSERT("seek+read diff", memcmp(a, b, 256) != 0);

            /* Interleaved small reads at various offsets */
            int interleave_ok = 1;
            for (int i = 0; i < 50; i++) {
                uint32_t off = (uint32_t)(i * 137) % 4096;
                uint8_t r1[16], r2[16];
                fseek(f, off, SEEK_SET);
                fread(r1, 1, 16, f);
                fseek(f, off, SEEK_SET);
                fread(r2, 1, 16, f);
                if (memcmp(r1, r2, 16) != 0) {
                    interleave_ok = 0;
                    break;
                }
            }
            ASSERT("50x seek+read", interleave_ok);

            fclose(f);
        }
    }

    /* Speed test: read 64KB from slot:1 and measure throughput */
    {
        static uint8_t speed_buf[65536];
        uint32_t t_start = of_time_ms();
        f = fopen("slot:1", "rb");
        if (f) {
            fread(speed_buf, 1, 65536, f);
            fclose(f);
        }
        uint32_t t_elapsed = of_time_ms() - t_start;
        if (t_elapsed > 0) {
            int kbps = (int)(64 * 1000 / t_elapsed);
            snprintf(__buf, sizeof(__buf), "64KB in %dms (%dKB/s)", (int)t_elapsed, kbps);
        } else {
            snprintf(__buf, sizeof(__buf), "64KB in <1ms");
        }
        printf("[%s] ", __buf);
        test_pass("speed");
    }

    section_end();
}

/* --- Saves --- */
static void test_saves(void) {
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

    /* Boundary: write at end of 128KB slot */
    uint8_t edge[4] = { 0xCA, 0xFE, 0xBA, 0xBE };
    rc = of_save_write(slot, edge, 0x20000 - 4, 4);
    ASSERT_EQ("edge write", rc, 4);
    uint8_t edgeread[4];
    of_save_read(slot, edgeread, 0x20000 - 4, 4);
    ASSERT("edge read", memcmp(edge, edgeread, 4) == 0);

    /* Out of bounds should fail */
    rc = of_save_write(slot, edge, 0x20000, 4);
    ASSERT_EQ("oob write", rc, -1);
    rc = of_save_read(slot, edgeread, 0x20000, 4);
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

/* --- Idle Hook --- */
static volatile int idle_hook_count;

static void test_idle_callback(void) {
    idle_hook_count++;
}

static void test_idle_hook(void) {
    section_start("Idle Hook");

    /* Register hook */
    of_set_idle_hook(test_idle_callback);
    idle_hook_count = 0;

    /* Do a file read — hook should be called during DMA wait */
    FILE *f = fopen("slot:1", "rb");
    if (f) {
        uint8_t buf[4096];
        fread(buf, 1, 4096, f);
        fclose(f);
    }

    ASSERT("hook called", idle_hook_count > 0);
    printf("[%d calls] ", idle_hook_count);

    /* Unregister */
    of_set_idle_hook((void *)0);

    /* Read again — hook should NOT be called */
    int prev = idle_hook_count;
    f = fopen("slot:1", "rb");
    if (f) {
        uint8_t buf[256];
        fread(buf, 1, 256, f);
        fclose(f);
    }
    ASSERT("hook off", idle_hook_count == prev);

    section_end();
}

/* --- Audio Ring Buffer --- */
static void test_audio_ring(void) {
    section_start("Audio Ring");

    of_audio_init();

    /* Check ring has free space */
    int free = of_audio_ring_free();
    ASSERT("ring free", free > 0);

    /* Enqueue a test tone (440Hz, 50ms) */
    #define RING_FRAMES 2400
    static int16_t tone[RING_FRAMES * 2];
    for (int i = 0; i < RING_FRAMES; i++) {
        /* Simple triangle wave */
        uint32_t phase = ((uint32_t)i * 440 * 65536 / 48000) & 0xFFFF;
        int32_t tri = (int32_t)(phase & 0xFFFF) - 32768;
        if (tri < 0) tri = -tri;
        tri = (tri - 16384) * 2;
        int16_t s = (int16_t)(tri >> 1);
        tone[i*2] = s;
        tone[i*2+1] = s;
    }

    int enqueued = of_audio_enqueue(tone, RING_FRAMES);
    ASSERT("enqueue", enqueued > 0);

    /* Ring should have less free space now */
    int free_after = of_audio_ring_free();
    ASSERT("ring used", free_after < free);

    /* Do a file read — kernel should drain ring to FIFO during DMA */
    {
        FILE *f = fopen("slot:1", "rb");
        if (f) {
            uint8_t buf[4096];
            fread(buf, 1, 4096, f);
            fclose(f);
        }
    }

    /* After DMA wait, ring should have been drained (at least partially) */
    int free_drained = of_audio_ring_free();
    ASSERT("drained", free_drained > free_after);

    printf("[enq=%d drain=%d] ", enqueued, free_drained - free_after);

    section_end();
}

/* --- Mixer Background --- */
static void test_mixer(void) {
    section_start("Mixer");

    /* Init mixer: 4 voices, 48kHz output */
    of_mixer_init(4, 48000);
    test_pass("init");

    /* Generate a short 8-bit unsigned PCM tone (~50ms at 11025Hz) */
    #define MIX_TONE_LEN 551
    static uint8_t pcm_tone[MIX_TONE_LEN];
    for (int i = 0; i < MIX_TONE_LEN; i++) {
        /* 440Hz triangle wave, unsigned 8-bit (128 = silence) */
        int phase = (i * 440 / 11025) & 1;
        pcm_tone[i] = phase ? 192 : 64;
    }

    /* Play the tone */
    int voice = of_mixer_play(pcm_tone, MIX_TONE_LEN, 11025, 10, 128);
    ASSERT("play", voice >= 0);

    /* Wait for FIFO to drain enough for mixer to pump, then do file I/O.
     * The FIFO is 4096 frames; at 48kHz it takes ~85ms to drain.
     * We need free space so of_mixer_pump() can actually advance voices. */
    of_delay_ms(100);

    /* Now pump repeatedly with file I/O (creates DMA wait windows) */
    for (int i = 0; i < 10; i++) {
        of_mixer_pump();
        FILE *f = fopen("slot:1", "rb");
        if (f) {
            uint8_t buf[4096];
            fread(buf, 1, 4096, f);
            fclose(f);
        }
    }

    /* After draining + pumping, the 551-sample tone at 11025Hz should
     * have finished (only ~25ms of audio, well within our budget) */
    int still_active = of_mixer_voice_active(voice);
    ASSERT("auto-pump", still_active == 0);

    /* Verify we can play another sound (mixer still functional) */
    int v2 = of_mixer_play(pcm_tone, MIX_TONE_LEN, 11025, 10, 64);
    ASSERT("replay", v2 >= 0);

    of_mixer_stop_all();
    test_pass("stop all");

    printf("[voice=%d] ", voice);

    section_end();
}

/* --- Interact --- */
static void test_interact(void) {
    section_start("Interact");

    /* Read interact variables — should return default values or
     * whatever the user set in the Pocket menu. Just verify
     * the read doesn't crash and returns a sane value. */
    uint32_t v0 = of_interact_get(0);
    uint32_t v1 = of_interact_get(1);
    uint32_t v2 = of_interact_get(2);
    test_pass("read[0-2]");

    /* Out of bounds should return 0 */
    ASSERT_EQ("oob", (int)of_interact_get(999), 0);

    /* Print current values for visibility */
    printf("[%d,%d,%d] ", (int)v0, (int)v1, (int)v2);

    section_end();
}

/* --- Audio --- */
static void test_audio(void) {
    section_start("Audio");

    /* Init audio subsystem */
    of_audio_init();
    test_pass("init");

    /* Check FIFO has free space */
    int free = of_audio_free();
    ASSERT("fifo free", free > 0);

    /* Generate a 440Hz sine wave test tone (1 second)
     * 48kHz stereo int16_t = 48000 pairs/sec */
    {
        #define TONE_RATE 48000
        #define TONE_HZ 440
        #define TONE_FRAMES 2400  /* 50ms of audio */
        static int16_t tone[TONE_FRAMES * 2];  /* stereo pairs */

        /* Simple sine approximation using integer math:
         * sin(x) ≈ x - x³/6 for small x, but we'll use a
         * lookup-free triangle→sine approximation */
        for (int i = 0; i < TONE_FRAMES; i++) {
            /* Phase: 0..65535 wrapping */
            uint32_t phase = (uint32_t)i * TONE_HZ * 65536 / TONE_RATE;
            /* Triangle wave → approximate sine */
            int32_t tri = (int32_t)(phase & 0xFFFF) - 32768;
            if (tri < 0) tri = -tri;
            tri = tri - 16384;  /* center around 0 */
            int16_t sample = (int16_t)(tri * 2);  /* scale to int16 range */
            /* ~50% volume */
            sample >>= 1;
            tone[i * 2]     = sample;  /* left */
            tone[i * 2 + 1] = sample;  /* right */
        }

        /* Write to FIFO */
        int written = of_audio_write(tone, TONE_FRAMES);
        ASSERT("write tone", written > 0);

        /* Verify FIFO accepted samples (written > 0 is sufficient —
         * the DAC drains at 48kHz so free space may recover before
         * we can re-check) */

        /* Brief beep should be audible */
        printf("[%dHz] ", TONE_HZ);
    }

    section_end();
}

/* --- Printf --- */
static void test_printf(void) {
    section_start("Printf");
    char buf[64];
    ASSERT_EQ("len", snprintf(buf, sizeof(buf), "hello %d", 42), 8);
    ASSERT("str", strcmp(buf, "hello 42") == 0);
    sprintf(buf, "%x", 0xDEAD);
    ASSERT("hex", strcmp(buf, "dead") == 0);
    snprintf(buf, sizeof(buf), "%05d", 42);
    ASSERT("pad", strcmp(buf, "00042") == 0);
    section_end();
}

/* --- String --- */
static void test_string(void) {
    section_start("String");
    ASSERT_EQ("strlen", (int)strlen("hello"), 5);
    ASSERT_EQ("strcmp", strcmp("abc", "abc"), 0);
    ASSERT("strchr", strchr("hello", 'l') != NULL);
    ASSERT("strstr", strstr("hello world", "world") != NULL);
    char d[32] = "foo"; strcat(d, "bar");
    ASSERT("strcat", strcmp(d, "foobar") == 0);
    ASSERT("memcmp", memcmp("abc", "abc", 3) == 0);
    char m[8]; memset(m, 0x42, 8);
    ASSERT("memset", m[0] == 0x42 && m[7] == 0x42);
    section_end();
}

/* --- PSRAM Memory Integrity --- */
static void test_psram_memory(void) {
    section_start("PSRAM Mem");

    /* CRAM0 base addresses */
    volatile uint32_t *cram0 = (volatile uint32_t *)0x30000000;
    volatile uint32_t *cram1 = (volatile uint32_t *)0x31000000;
    volatile uint32_t *sram  = (volatile uint32_t *)0x3A000000;

    /* === CRAM0 basic write/read === */
    cram0[0] = 0xDEADBEEF;
    cram0[1] = 0xCAFEBABE;
    ASSERT("cram0 w/r[0]", cram0[0] == 0xDEADBEEF);
    ASSERT("cram0 w/r[1]", cram0[1] == 0xCAFEBABE);

    /* === CRAM0 walking-ones pattern (32 words) === */
    {
        int ok = 1;
        for (int i = 0; i < 32; i++)
            cram0[i] = 1u << i;
        for (int i = 0; i < 32; i++) {
            if (cram0[i] != (1u << i)) { ok = 0; break; }
        }
        ASSERT("cram0 walk1", ok);
    }

    /* === CRAM0 address line test (write distinct values at power-of-2 offsets) === */
    {
        int ok = 1;
        /* Test address lines up to 2MB (512K words) */
        for (int bit = 0; bit < 19; bit++) {
            uint32_t offset = 1u << bit;
            cram0[offset] = 0xA5000000 | offset;
        }
        for (int bit = 0; bit < 19; bit++) {
            uint32_t offset = 1u << bit;
            if (cram0[offset] != (0xA5000000 | offset)) { ok = 0; break; }
        }
        ASSERT("cram0 addr", ok);
    }

    /* === CRAM0 large block write/readback (64KB) === */
    {
        int ok = 1;
        for (uint32_t i = 0; i < 16384; i++)
            cram0[i] = i ^ 0x5A5A5A5A;
        for (uint32_t i = 0; i < 16384; i++) {
            if (cram0[i] != (i ^ 0x5A5A5A5A)) { ok = 0; break; }
        }
        ASSERT("cram0 64K", ok);
    }

    /* === CRAM0 burst read speed benchmark (64KB) === */
    {
        /* Write known data */
        for (uint32_t i = 0; i < 16384; i++)
            cram0[i] = i;

        uint32_t t0 = of_time_us();
        /* Read 64KB — will use sync burst on CRAM0 via I-cache/D-cache line fills */
        volatile uint32_t sum = 0;
        for (uint32_t i = 0; i < 16384; i++)
            sum += cram0[i];
        uint32_t t1 = of_time_us();
        (void)sum;

        uint32_t elapsed_us = t1 - t0;
        if (elapsed_us > 0) {
            int kbps = (int)(64 * 1000000 / elapsed_us);
            snprintf(__buf, sizeof(__buf), "64KB in %dus (%dKB/s)", (int)elapsed_us, kbps);
        } else {
            snprintf(__buf, sizeof(__buf), "64KB in <1us");
        }
        printf("[%s] ", __buf);
        test_pass("cram0 speed");
    }

    /* === CRAM1 basic write/read === */
    cram1[0] = 0x12345678;
    cram1[1] = 0x9ABCDEF0;
    ASSERT("cram1 w/r[0]", cram1[0] == 0x12345678);
    ASSERT("cram1 w/r[1]", cram1[1] == 0x9ABCDEF0);

    /* === CRAM1 large block (16KB) === */
    {
        int ok = 1;
        for (uint32_t i = 0; i < 4096; i++)
            cram1[i] = ~i;
        for (uint32_t i = 0; i < 4096; i++) {
            if (cram1[i] != ~i) { ok = 0; break; }
        }
        ASSERT("cram1 16K", ok);
    }

    /* === SRAM basic write/read === */
    sram[0] = 0xFEEDFACE;
    sram[1] = 0xBAADF00D;
    ASSERT("sram w/r[0]", sram[0] == 0xFEEDFACE);
    ASSERT("sram w/r[1]", sram[1] == 0xBAADF00D);

    /* === SRAM walking pattern (256 words = 1KB) === */
    {
        int ok = 1;
        for (int i = 0; i < 256; i++)
            sram[i] = (uint32_t)i * 0x01010101;
        for (int i = 0; i < 256; i++) {
            if (sram[i] != (uint32_t)i * 0x01010101) { ok = 0; break; }
        }
        ASSERT("sram 1K", ok);
    }

    /* === Cross-chip isolation: writing CRAM0 doesn't corrupt CRAM1 === */
    {
        cram1[0] = 0x11111111;
        cram0[0] = 0x22222222;
        ASSERT("isolation c1", cram1[0] == 0x11111111);
        ASSERT("isolation c0", cram0[0] == 0x22222222);
    }

    /* === Byte-enable test (partial word writes) === */
    {
        volatile uint8_t *cram0_b = (volatile uint8_t *)0x30000000;
        cram0[0] = 0x00000000;
        cram0_b[1] = 0xAB;
        ASSERT("byte write", (cram0[0] & 0x0000FF00) == 0x0000AB00);
    }

    /* === Stress: interleaved CRAM0/CRAM1 access (catch mux/arbitration bugs) === */
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

/* --- Shutdown Handshake --- */
static void test_shutdown(void) {
    section_start("Shutdown");

    /* Read shutdown register — should not be pending (we're running) */
    volatile uint32_t *shutdown_reg = (volatile uint32_t *)(0x40000000 + 0xB0);
    uint32_t val = *shutdown_reg;
    ASSERT("not pending", (val & 1) == 0);

    /* Write ack bit (should be safe when not pending) */
    *shutdown_reg = 1;
    test_pass("ack write");

    section_end();
}

/* --- Version --- */
/* --- LZW Compression --- */
#define LZW_TEST_SIZE 256
static uint8_t lzw_orig[LZW_TEST_SIZE];
static uint8_t lzw_comp[LZW_TEST_SIZE + (LZW_TEST_SIZE >> 4) + 16];
static uint8_t lzw_decomp[LZW_TEST_SIZE];

static void test_lzw(void) {
    section_start("LZW");

    /* Round-trip: compressible pattern */
    for (int i = 0; i < LZW_TEST_SIZE; i++)
        lzw_orig[i] = (uint8_t)(i & 0x1F);

    int32_t comp_len = of_lzw_compress(lzw_orig, LZW_TEST_SIZE, lzw_comp);
    ASSERT("compress ok", comp_len > 0);
    ASSERT("compress smaller", comp_len < LZW_TEST_SIZE);

    if (comp_len > 0) {
        int32_t decomp_len = of_lzw_uncompress(lzw_comp, comp_len, lzw_decomp);
        ASSERT_EQ("decomp len", decomp_len, LZW_TEST_SIZE);
        ASSERT("round-trip", memcmp(lzw_orig, lzw_decomp, LZW_TEST_SIZE) == 0);
    }

    /* All-zeros: should compress well */
    memset(lzw_orig, 0, LZW_TEST_SIZE);
    comp_len = of_lzw_compress(lzw_orig, LZW_TEST_SIZE, lzw_comp);
    ASSERT("zeros ok", comp_len > 0);

    if (comp_len > 0) {
        int32_t decomp_len = of_lzw_uncompress(lzw_comp, comp_len, lzw_decomp);
        ASSERT_EQ("zeros len", decomp_len, LZW_TEST_SIZE);
        ASSERT("zeros match", memcmp(lzw_orig, lzw_decomp, LZW_TEST_SIZE) == 0);
    }

    /* Random-ish data: must still round-trip */
    {
        uint32_t seed = 0xDEADBEEF;
        for (int i = 0; i < LZW_TEST_SIZE; i++) {
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            lzw_orig[i] = (uint8_t)(seed & 0xFF);
        }
    }
    comp_len = of_lzw_compress(lzw_orig, LZW_TEST_SIZE, lzw_comp);
    ASSERT("rand ok", comp_len > 0);

    if (comp_len > 0) {
        int32_t decomp_len = of_lzw_uncompress(lzw_comp, comp_len, lzw_decomp);
        ASSERT_EQ("rand len", decomp_len, LZW_TEST_SIZE);
        ASSERT("rand match", memcmp(lzw_orig, lzw_decomp, LZW_TEST_SIZE) == 0);
    }

    section_end();
}

static void test_version(void) {
    section_start("Version");
    uint32_t ver = of_get_version();
    ASSERT("nonzero", ver != 0);
    printf("v%d.%d.%d ", (ver>>16)&0xFF, (ver>>8)&0xFF, ver&0xFF);
    section_end();
}

/* --- Main --- */
#define NUM_ITERATIONS 3

int main(void) {
    int iteration;
    for (iteration = 0; iteration < NUM_ITERATIONS; iteration++) {
        pass_count = 0;
        fail_count = 0;

        printf("\033[2J\033[H");
        printf("\033[93m  openfpgaOS Test [%d/%d]\033[0m\n\n",
               iteration + 1, NUM_ITERATIONS);

        test_timer();
        test_malloc();
        test_psram_memory();
        test_file_slots();
        test_file_io();
        test_saves();
        test_shutdown();
        test_idle_hook();
        test_audio_ring();
        test_mixer();
        test_interact();
        test_audio();
        test_printf();
        test_string();
        test_lzw();
        test_version();

        printf("\n  \033[96mTotal:\033[0m %d passed", pass_count);
        if (fail_count > 0)
            printf(", \033[91m%d failed\033[0m", fail_count);
        printf("\n");

        if (fail_count > 0) {
            printf("  \033[91mFAILED\033[0m\n");
            break;
        }

        if (iteration < NUM_ITERATIONS - 1) {
            printf("  \033[92mPASS\033[0m -- next in 2s...\n");
            of_delay_ms(2000);
        } else {
            printf("\n  \033[92mALL %d ITERATIONS PASSED\033[0m\n", NUM_ITERATIONS);
        }
    }

    while (1)
        of_delay_ms(100);
    return 0;
}
