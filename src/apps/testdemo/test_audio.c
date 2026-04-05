#include "test.h"
#include <time.h>

void test_mixer(void) {
    section_start("Mixer");

    /* Init mixer: 4 voices, 48kHz output */
    of_mixer_init(4, 48000);
    test_pass("init");

    /* Generate a short 16-bit signed mono PCM tone (~50ms at 11025Hz) */
    #define MIX_TONE_LEN 551
    static int16_t pcm_tone_src[MIX_TONE_LEN];
    for (int i = 0; i < MIX_TONE_LEN; i++) {
        int phase = (i * 440 / 11025) & 1;
        pcm_tone_src[i] = phase ? 8000 : -8000;
    }

    /* Allocate sample memory and copy tone data */
    void *sample_buf = of_mixer_alloc_samples(MIX_TONE_LEN * sizeof(int16_t));
    ASSERT("alloc", sample_buf != NULL);
    if (sample_buf)
        memcpy(sample_buf, pcm_tone_src, MIX_TONE_LEN * sizeof(int16_t));

    /* Play the tone */
    int voice = of_mixer_play((const uint8_t *)sample_buf, MIX_TONE_LEN, 11025, 0, 128);
    ASSERT("play", voice >= 0);

    /* Wait for playback to finish (~50ms tone) */
    usleep(100 * 1000);

    /* Check voice ended via poll_ended */
    uint32_t ended = of_mixer_poll_ended();
    ASSERT("voice done", voice >= 0 && (ended & (1u << voice)));

    /* Test stereo panning */
    int v2 = of_mixer_play((const uint8_t *)sample_buf, MIX_TONE_LEN, 11025, 0, 200);
    ASSERT("replay", v2 >= 0);
    if (v2 >= 0) {
        of_mixer_set_vol_lr(v2, 255, 0);  /* left only */
        usleep(30 * 1000);
        of_mixer_set_vol_lr(v2, 0, 255);  /* right only */
        usleep(30 * 1000);
        of_mixer_set_vol_lr(v2, 128, 128); /* center */
        test_pass("stereo pan");
    }

    /* Test looping */
    int v3 = of_mixer_play((const uint8_t *)sample_buf, MIX_TONE_LEN, 11025, 0, 100);
    if (v3 >= 0) {
        of_mixer_set_loop(v3, 0, MIX_TONE_LEN);
        usleep(100 * 1000);  /* should still be active (looping) */
        ASSERT("loop active", of_mixer_voice_active(v3));
        of_mixer_set_loop(v3, -1, 0);  /* disable loop */
        usleep(100 * 1000);
        of_mixer_poll_ended();
        ASSERT("loop disable", !of_mixer_voice_active(v3));
    }

    /* Test position read/write */
    int v4 = of_mixer_play((const uint8_t *)sample_buf, MIX_TONE_LEN, 11025, 0, 100);
    if (v4 >= 0) {
        of_mixer_set_loop(v4, 0, MIX_TONE_LEN);
        usleep(20 * 1000);
        int pos = of_mixer_get_position(v4);
        ASSERT("pos read", pos > 0 && pos < MIX_TONE_LEN);
        of_mixer_set_position(v4, 0);  /* seek to start */
        usleep(5 * 1000);
        int pos2 = of_mixer_get_position(v4);
        ASSERT("pos write", pos2 < pos);  /* should have reset */
    }

    of_mixer_stop_all();
    test_pass("stop all");

    of_mixer_free_samples();
    test_pass("free samples");

    section_end();
}

void test_audio(void) {
    section_start("Audio");

    /* Init audio subsystem */
    of_audio_init();
    test_pass("init");

    /* Check FIFO has free space */
    int free = of_audio_free();
    ASSERT("fifo free", free > 0);

    /* Generate a 440Hz tone (50ms) for the audio FIFO */
    {
        #define TONE_RATE 48000
        #define TONE_HZ 440
        #define TONE2_FRAMES 2400
        static int16_t tone2[TONE2_FRAMES * 2];

        for (int i = 0; i < TONE2_FRAMES; i++) {
            uint32_t phase = (uint32_t)i * TONE_HZ * 65536 / TONE_RATE;
            int32_t tri = (int32_t)(phase & 0xFFFF) - 32768;
            if (tri < 0) tri = -tri;
            tri = tri - 16384;
            int16_t sample = (int16_t)(tri * 2);
            sample >>= 1;
            tone2[i * 2]     = sample;
            tone2[i * 2 + 1] = sample;
        }

        int written = of_audio_write(tone2, TONE2_FRAMES);
        ASSERT("write tone", written > 0);

        (void)written;
    }

    section_end();
}
