#include "test.h"

void test_audio_ring(void) {
    section_start("Audio Ring");

    of_audio_init();

    /* Hardware mixer: just verify init and enable work */
    of_mixer_init(32, 48000);
    test_pass("hw mixer init");

    /* Stop all — should not crash */
    of_mixer_stop_all();
    test_pass("stop all");

    section_end();
}

void test_mixer(void) {
    section_start("Mixer");

    of_mixer_init(32, 48000);
    test_pass("init");

    /* Hardware mixer runs autonomously — no pump needed.
     * Just verify the API doesn't crash. */
    of_mixer_pump();  /* no-op */
    test_pass("pump noop");

    section_end();
}

void test_audio(void) {
    section_start("Audio");

    of_audio_init();
    test_pass("init");

    /* Check FIFO has free space */
    int free = of_audio_free();
    ASSERT("fifo free", free > 0);

    /* Generate a short test tone */
    {
        #define TONE_FRAMES 2400
        static int16_t tone[TONE_FRAMES * 2];
        for (int i = 0; i < TONE_FRAMES; i++) {
            uint32_t phase = (uint32_t)i * 440 * 65536 / 48000;
            int32_t tri = (int32_t)(phase & 0xFFFF) - 32768;
            if (tri < 0) tri = -tri;
            tri = tri - 16384;
            int16_t sample = (int16_t)(tri * 2) >> 1;
            tone[i * 2]     = sample;
            tone[i * 2 + 1] = sample;
        }

        int written = of_audio_write(tone, TONE_FRAMES);
        ASSERT("write tone", written > 0);
        (void)written;
    }

    section_end();
}
