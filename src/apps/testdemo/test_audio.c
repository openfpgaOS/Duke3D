/*
 * test_audio.c — Mixer and audio tests
 *
 * Test codes:
 *   MX.xx = Hardware mixer tests
 *   AU.xx = Audio FIFO / streaming tests
 */

#include "test.h"
#include <time.h>

void test_mixer(void) {
    section_start("Mixer");

    of_mixer_init(4, 48000);
    of_mixer_stop_all();
    of_mixer_poll_ended();
    test_pass("MX.01 init");

    #define MIX_TONE_LEN 551
    static int16_t pcm_tone_src[MIX_TONE_LEN];
    for (int i = 0; i < MIX_TONE_LEN; i++) {
        int phase = (i * 440 / 11025) & 1;
        pcm_tone_src[i] = phase ? 8000 : -8000;
    }

    void *sample_buf = of_mixer_alloc_samples(MIX_TONE_LEN * sizeof(int16_t));
    ASSERT("MX.02 alloc", sample_buf != NULL);
    if (sample_buf) {
        memcpy(sample_buf, pcm_tone_src, MIX_TONE_LEN * sizeof(int16_t));
        OF_SVC->cache_flush();
    }

    int voice = of_mixer_play((const uint8_t *)sample_buf, MIX_TONE_LEN, 11025, 0, 128);
    ASSERT("MX.03 play", voice >= 0);

    for (int i = 0; i < 10; i++) {
        of_mixer_pump();
        usleep(15 * 1000);
    }

    uint32_t ended = of_mixer_poll_ended();
    ASSERT("MX.04 done", voice >= 0 && (ended & (1u << voice)));

    int v2 = of_mixer_play((const uint8_t *)sample_buf, MIX_TONE_LEN, 11025, 0, 200);
    ASSERT("MX.05 replay", v2 >= 0);
    if (v2 >= 0) {
        of_mixer_set_vol_lr(v2, 255, 0);
        usleep(30 * 1000);
        of_mixer_set_vol_lr(v2, 0, 255);
        usleep(30 * 1000);
        of_mixer_set_vol_lr(v2, 128, 128);
        test_pass("MX.06 stereo");
    }

    int v3 = of_mixer_play((const uint8_t *)sample_buf, MIX_TONE_LEN, 11025, 0, 100);
    if (v3 >= 0) {
        of_mixer_set_loop(v3, 0, MIX_TONE_LEN);
        usleep(100 * 1000);
        ASSERT("MX.07 loop on", of_mixer_voice_active(v3));
        of_mixer_set_loop(v3, -1, 0);
        usleep(100 * 1000);
        of_mixer_poll_ended();
        ASSERT("MX.08 loop off", !of_mixer_voice_active(v3));
    }

    int v4 = of_mixer_play((const uint8_t *)sample_buf, MIX_TONE_LEN, 11025, 0, 100);
    if (v4 >= 0) {
        of_mixer_set_loop(v4, 0, MIX_TONE_LEN);
        usleep(20 * 1000);
        int pos = of_mixer_get_position(v4);
        ASSERT("MX.09 pos rd", pos > 0 && pos < MIX_TONE_LEN);
        of_mixer_set_position(v4, 0);
        usleep(5 * 1000);
        int pos2 = of_mixer_get_position(v4);
        /* After seek to 0 + 5ms at 11025Hz, pos2 ≈ 55 samples */
        ASSERT("MX.10 pos wr", pos2 < MIX_TONE_LEN / 4);
    }

    of_mixer_stop_all();
    test_pass("MX.11 stop");

    of_mixer_free_samples();
    test_pass("MX.12 free");

    section_end();
}

void test_mixer_adv(void) {
    section_start("Mixer Adv");

    of_mixer_init(32, 48000);
    of_mixer_stop_all();
    of_mixer_poll_ended();
    of_mixer_free_samples();

    /* Generate tone in CRAM1 */
    #define ADV_TONE_LEN 551
    static int16_t adv_tone_src[ADV_TONE_LEN];
    for (int i = 0; i < ADV_TONE_LEN; i++)
        adv_tone_src[i] = (i & 1) ? 8000 : -8000;

    int16_t *s16_buf = (int16_t *)of_mixer_alloc_samples(ADV_TONE_LEN * 2);
    if (!s16_buf) { test_fail("MA.00 alloc", "null"); section_end(); return; }
    memcpy(s16_buf, adv_tone_src, ADV_TONE_LEN * 2);
    OF_SVC->cache_flush();

    /* MA.01: 8-bit playback — allocate 8-bit signed, play, verify voice starts */
    {
        int8_t *s8_buf = (int8_t *)of_mixer_alloc_samples(ADV_TONE_LEN);
        ASSERT("MA.01a alloc8", s8_buf != NULL);
        if (s8_buf) {
            for (int i = 0; i < ADV_TONE_LEN; i++)
                s8_buf[i] = (i & 1) ? 31 : -31;
            OF_SVC->cache_flush();
            int v = of_mixer_play_8bit((const uint8_t *)s8_buf, ADV_TONE_LEN, 11025, 0, 200);
            ASSERT("MA.01b play8", v >= 0);
            if (v >= 0) {
                usleep(10 * 1000);
                ASSERT("MA.01c actv8", of_mixer_voice_active(v));
                of_mixer_stop(v);
            }
        }
    }

    /* MA.02: priority-based voice stealing.
     * Fill all voices with looping low-priority sounds, then test stealing. */
    {
        of_mixer_stop_all();
        of_mixer_poll_ended();
        int filled = 0;
        for (int i = 0; i < 31; i++) {
            int v = of_mixer_play((const uint8_t *)s16_buf, ADV_TONE_LEN, 11025, 0, 50);
            if (v >= 0) {
                of_mixer_set_loop(v, 0, ADV_TONE_LEN);  /* keep alive */
                filled++;
            }
        }
        ASSERT("MA.02a fill", filled >= 28);

        /* All voices busy — same priority play should fail (can't steal equal) */
        int vlo = of_mixer_play((const uint8_t *)s16_buf, ADV_TONE_LEN, 11025, 0, 50);
        ASSERT("MA.02b nofree", vlo < 0);

        /* Higher priority should steal a low-priority voice */
        int vhi = of_mixer_play((const uint8_t *)s16_buf, ADV_TONE_LEN, 11025, 10, 100);
        ASSERT("MA.02c steal", vhi >= 0);

        of_mixer_stop_all();
        of_mixer_poll_ended();
    }

    /* MA.03: set_pan at center preserves volume.
     * Play at vol=200, set pan to center (128), check voice is still active.
     * (The old bug made center pan = half volume.) */
    {
        int v = of_mixer_play((const uint8_t *)s16_buf, ADV_TONE_LEN, 11025, 0, 200);
        ASSERT("MA.03a play", v >= 0);
        if (v >= 0) {
            of_mixer_set_pan(v, 128);  /* center */
            usleep(10 * 1000);
            ASSERT("MA.03b pan", of_mixer_voice_active(v));
            of_mixer_stop(v);
        }
    }

    /* MA.06: retrigger — play, retrigger mid-playback, verify still active */
    {
        int v = of_mixer_play((const uint8_t *)s16_buf, ADV_TONE_LEN, 11025, 0, 150);
        ASSERT("MA.06a play", v >= 0);
        if (v >= 0) {
            usleep(20 * 1000);
            ASSERT("MA.06b pre", of_mixer_voice_active(v));
            of_mixer_retrigger(v, (const uint8_t *)s16_buf, ADV_TONE_LEN, 11025, 150);
            usleep(10 * 1000);
            ASSERT("MA.06c post", of_mixer_voice_active(v));
            /* After retrigger, position should be near start */
            int pos = of_mixer_get_position(v);
            ASSERT("MA.06d pos", pos < ADV_TONE_LEN / 2);
            of_mixer_stop(v);
        }
    }

    /* MA.07: bidi looping — play with bidi, verify stays active */
    {
        int v = of_mixer_play((const uint8_t *)s16_buf, ADV_TONE_LEN, 11025, 0, 100);
        if (v >= 0) {
            of_mixer_set_loop(v, 0, ADV_TONE_LEN);
            of_mixer_set_bidi(v, 1);
            usleep(100 * 1000);  /* longer than one pass */
            ASSERT("MA.07 bidi", of_mixer_voice_active(v));
            of_mixer_stop(v);
        }
    }

    /* MA.08: volume ramp — set vol_rate, change volume, verify voice active */
    {
        int v = of_mixer_play((const uint8_t *)s16_buf, ADV_TONE_LEN, 11025, 0, 255);
        if (v >= 0) {
            of_mixer_set_loop(v, 0, ADV_TONE_LEN);
            of_mixer_set_volume_ramp(v, 4);  /* slow ramp */
            of_mixer_set_volume(v, 0);    /* ramp to 0 */
            usleep(10 * 1000);
            ASSERT("MA.08 ramp", of_mixer_voice_active(v));  /* still active during ramp */
            of_mixer_stop(v);
        }
    }

    /* MA.09: voice end callback via poll_ended bitmask */
    {
        of_mixer_stop_all();
        of_mixer_poll_ended();  /* clear stale bits */
        int v = of_mixer_play((const uint8_t *)s16_buf, ADV_TONE_LEN, 11025, 0, 100);
        ASSERT("MA.09a play", v >= 0);
        if (v >= 0) {
            /* Wait for voice to finish (~50ms) */
            for (int i = 0; i < 15; i++) usleep(10 * 1000);
            uint32_t ended = of_mixer_poll_ended();
            ASSERT("MA.09b ended", ended & (1u << v));
            ASSERT("MA.09c dead", !of_mixer_voice_active(v));
        }
    }

    /* MA.10: playback rate verification.
     * Play at 48000 Hz (1:1 with output). After 10ms, position should be
     * ~480 samples. Allow ±20% tolerance for timer + scheduling jitter. */
    {
        of_mixer_stop_all();
        of_mixer_poll_ended();
        /* Need a longer sample for rate test */
        #define RATE_TONE_LEN 4800  /* 100ms at 48kHz */
        int16_t *rate_buf = (int16_t *)of_mixer_alloc_samples(RATE_TONE_LEN * 2);
        if (rate_buf) {
            for (int i = 0; i < RATE_TONE_LEN; i++)
                rate_buf[i] = (i & 1) ? 4000 : -4000;
            OF_SVC->cache_flush();

            int v = of_mixer_play((const uint8_t *)rate_buf, RATE_TONE_LEN, 48000, 0, 100);
            ASSERT("MA.10a play", v >= 0);
            if (v >= 0) {
                of_mixer_set_loop(v, 0, RATE_TONE_LEN);
                usleep(10 * 1000);  /* 10ms */
                int pos = of_mixer_get_position(v);
                /* At 48kHz, 10ms = 480 samples. Allow 384-576 (±20%) */
                ASSERT("MA.10b rate", pos >= 384 && pos <= 576);
                of_mixer_stop(v);
            }
        }
    }

    /* MA.11: playback rate at half speed.
     * Play at 24000 Hz. After 10ms, position should be ~240 samples. */
    {
        of_mixer_stop_all();
        of_mixer_poll_ended();
        int16_t *rate_buf = (int16_t *)of_mixer_alloc_samples(RATE_TONE_LEN * 2);
        if (rate_buf) {
            for (int i = 0; i < RATE_TONE_LEN; i++)
                rate_buf[i] = (i & 1) ? 4000 : -4000;
            OF_SVC->cache_flush();

            int v = of_mixer_play((const uint8_t *)rate_buf, RATE_TONE_LEN, 24000, 0, 100);
            if (v >= 0) {
                of_mixer_set_loop(v, 0, RATE_TONE_LEN);
                usleep(10 * 1000);
                int pos = of_mixer_get_position(v);
                /* At 24kHz, 10ms = 240 samples. Allow ±20%: 192-288 */
                ASSERT("MA.11 half", pos >= 192 && pos <= 288);
                of_mixer_stop(v);
            }
        }
    }

    /* MA.12: multi-voice independence.
     * Play two voices at different rates, measure position DELTA over
     * a fixed interval. Delta is immune to loop wrap and start jitter.
     * v1 at 48kHz should advance ~4.35x faster than v2 at 11025Hz. */
    {
        of_mixer_stop_all();
        of_mixer_poll_ended();
        int16_t *buf1 = (int16_t *)of_mixer_alloc_samples(RATE_TONE_LEN * 2);
        int16_t *buf2 = (int16_t *)of_mixer_alloc_samples(RATE_TONE_LEN * 2);
        if (buf1 && buf2) {
            for (int i = 0; i < RATE_TONE_LEN; i++) {
                buf1[i] = (int16_t)(i & 0xFF);
                buf2[i] = (int16_t)(~i & 0xFF);
            }
            OF_SVC->cache_flush();

            int v1 = of_mixer_play((const uint8_t *)buf1, RATE_TONE_LEN, 48000, 0, 100);
            int v2 = of_mixer_play((const uint8_t *)buf2, RATE_TONE_LEN, 11025, 0, 100);
            ASSERT("MA.12a v1", v1 >= 0);
            ASSERT("MA.12b v2", v2 >= 0);
            if (v1 >= 0 && v2 >= 0) {
                of_mixer_set_loop(v1, 0, RATE_TONE_LEN);
                of_mixer_set_loop(v2, 0, RATE_TONE_LEN);
                /* Let them run, then sample position twice */
                usleep(10 * 1000);
                int p1a = of_mixer_get_position(v1);
                int p2a = of_mixer_get_position(v2);
                usleep(10 * 1000);
                int p1b = of_mixer_get_position(v1);
                int p2b = of_mixer_get_position(v2);
                /* Compute deltas (handle loop wrap) */
                int d1 = p1b - p1a;
                if (d1 < 0) d1 += RATE_TONE_LEN;
                int d2 = p2b - p2a;
                if (d2 < 0) d2 += RATE_TONE_LEN;
                /* d1 should be ~2-5x larger than d2 (48000/11025 ≈ 4.35) */
                if (d1 > d2 * 2 && d2 > 0) {
                    test_pass("MA.12c rate");
                } else {
                    snprintf(__buf, sizeof(__buf),
                             "v%d/%d d %d/%d", v1, v2, d1, d2);
                    test_fail("MA.12c rate", __buf);
                }
                of_mixer_stop(v1);
                of_mixer_stop(v2);
            }
        }
    }

    /* MA.13: voice end timing — play at 48kHz, 2400 samples = 50ms.
     * Should end between 40-70ms (allow jitter). */
    {
        of_mixer_stop_all();
        of_mixer_poll_ended();
        #define END_TONE_LEN 2400
        int16_t *end_buf = (int16_t *)of_mixer_alloc_samples(END_TONE_LEN * 2);
        if (end_buf) {
            for (int i = 0; i < END_TONE_LEN; i++)
                end_buf[i] = (i & 1) ? 2000 : -2000;
            OF_SVC->cache_flush();

            int v = of_mixer_play((const uint8_t *)end_buf, END_TONE_LEN, 48000, 0, 100);
            if (v >= 0) {
                /* Should still be active at 30ms */
                usleep(30 * 1000);
                ASSERT("MA.13a @30ms", of_mixer_voice_active(v));
                /* Should be done by 70ms */
                usleep(40 * 1000);
                of_mixer_poll_ended();
                ASSERT("MA.13b @70ms", !of_mixer_voice_active(v));
            }
        }
    }

    of_mixer_stop_all();
    of_mixer_free_samples();
    /* Restore group/master to defaults for subsequent tests */
    of_mixer_set_group_volume(OF_MIXER_GROUP_SFX, 255);
    of_mixer_set_group_volume(OF_MIXER_GROUP_MUSIC, 255);
    of_mixer_set_master_volume(255);

    section_end();
}

/* ================================================================
 * Stress test: 31 PCM voices + OPL3 simultaneously
 *
 * This simulates a worst-case Duke3D scenario: many SFX playing
 * while MIDI music runs on OPL3. Tests mixer FSM timing budget,
 * CRAM1 bus bandwidth, and audio FIFO underrun.
 * ================================================================ */
void test_mixer_stress(void) {
    section_start("Mixer Strss");

    of_mixer_init(32, 48000);
    of_mixer_stop_all();
    of_mixer_poll_ended();
    of_mixer_free_samples();

    /* Allocate 31 different sample buffers with unique patterns.
     * Flush after each buffer — total data (149KB) exceeds D-cache (64KB),
     * so natural eviction from the 0x39 alias may lose data. */
    #define STRESS_LEN 2400
    int16_t *bufs[31];
    int alloc_ok = 1;
    for (int i = 0; i < 31; i++) {
        bufs[i] = (int16_t *)of_mixer_alloc_samples(STRESS_LEN * 2);
        if (!bufs[i]) { alloc_ok = 0; break; }
        int half_period = 10 + i * 3;
        for (int j = 0; j < STRESS_LEN; j++)
            bufs[i][j] = ((j / half_period) & 1) ? 2000 : -2000;
        OF_SVC->cache_flush();  /* flush each buffer individually */
    }
    ASSERT("ST.01 alloc31", alloc_ok);

    /* ST.02: Start all 31 voices simultaneously at different rates */
    int voices[31];
    int started = 0;
    for (int i = 0; i < 31 && bufs[i]; i++) {
        int rate = 11025 + i * 1200;  /* 11025 to 47025 Hz */
        voices[i] = of_mixer_play((const uint8_t *)bufs[i], STRESS_LEN,
                                  rate, i, 80);
        if (voices[i] >= 0) {
            of_mixer_set_loop(voices[i], 0, STRESS_LEN);
            /* Spread across stereo field */
            of_mixer_set_pan(voices[i], (i * 255) / 30);
            started++;
        }
    }
    ASSERT("ST.02 start31", started >= 29);

    /* ST.03: Start OPL3 FM while all 31 voices are playing.
     * Play a simple chord: 3 OPL channels with different frequencies. */
    of_audio_opl_reset();
    /* Enable OPL3 mode */
    of_audio_opl_write(0x105, 0x01);  /* OPL3 enable */
    /* Channel 0: ~440Hz (A4) */
    of_audio_opl_write(0x20, 0x21);   /* op1: sustain, multiply=1 */
    of_audio_opl_write(0x40, 0x10);   /* op1: volume (lower = louder) */
    of_audio_opl_write(0x60, 0xF0);   /* op1: attack=F, decay=0 */
    of_audio_opl_write(0x80, 0x0F);   /* op1: sustain=0, release=F */
    of_audio_opl_write(0x23, 0x21);   /* op2: sustain, multiply=1 */
    of_audio_opl_write(0x43, 0x00);   /* op2: max volume */
    of_audio_opl_write(0x63, 0xF0);   /* op2: attack=F, decay=0 */
    of_audio_opl_write(0x83, 0x0F);   /* op2: sustain=0, release=F */
    of_audio_opl_write(0xA0, 0x41);   /* freq low byte */
    of_audio_opl_write(0xB0, 0x32);   /* freq high + key on + octave */
    /* Channel 1: ~554Hz (C#5) */
    of_audio_opl_write(0x21, 0x21);
    of_audio_opl_write(0x41, 0x10);
    of_audio_opl_write(0x61, 0xF0);
    of_audio_opl_write(0x81, 0x0F);
    of_audio_opl_write(0x24, 0x21);
    of_audio_opl_write(0x44, 0x00);
    of_audio_opl_write(0x64, 0xF0);
    of_audio_opl_write(0x84, 0x0F);
    of_audio_opl_write(0xA1, 0x90);
    of_audio_opl_write(0xB1, 0x32);
    /* Channel 2: ~659Hz (E5) */
    of_audio_opl_write(0x22, 0x21);
    of_audio_opl_write(0x42, 0x10);
    of_audio_opl_write(0x62, 0xF0);
    of_audio_opl_write(0x82, 0x0F);
    of_audio_opl_write(0x25, 0x21);
    of_audio_opl_write(0x45, 0x00);
    of_audio_opl_write(0x65, 0xF0);
    of_audio_opl_write(0x85, 0x0F);
    of_audio_opl_write(0xA2, 0xE0);
    of_audio_opl_write(0xB2, 0x32);
    test_pass("ST.03 opl3 on");

    /* ST.04: Let everything run for 200ms — this is the stress window.
     * All 31 mixer voices + 3 OPL channels running simultaneously.
     * Check every 50ms that voices are still active (no underrun/crash). */
    {
        int alive_50 = 0, alive_100 = 0, alive_150 = 0, alive_200 = 0;
        usleep(50 * 1000);
        for (int i = 0; i < 31; i++)
            if (voices[i] >= 0 && of_mixer_voice_active(voices[i])) alive_50++;
        usleep(50 * 1000);
        for (int i = 0; i < 31; i++)
            if (voices[i] >= 0 && of_mixer_voice_active(voices[i])) alive_100++;
        usleep(50 * 1000);
        for (int i = 0; i < 31; i++)
            if (voices[i] >= 0 && of_mixer_voice_active(voices[i])) alive_150++;
        usleep(50 * 1000);
        for (int i = 0; i < 31; i++)
            if (voices[i] >= 0 && of_mixer_voice_active(voices[i])) alive_200++;

        /* All looping voices should remain active throughout */
        ASSERT("ST.04a @50ms", alive_50 >= started - 1);
        ASSERT("ST.04b @100ms", alive_100 >= started - 1);
        ASSERT("ST.04c @150ms", alive_150 >= started - 1);
        ASSERT("ST.04d @200ms", alive_200 >= started - 1);
    }

    /* ST.05: verify positions are advancing (not stuck) */
    {
        int advancing = 0;
        for (int i = 0; i < 31; i++) {
            if (voices[i] < 0) continue;
            int p1 = of_mixer_get_position(voices[i]);
            usleep(5 * 1000);
            int p2 = of_mixer_get_position(voices[i]);
            if (p2 != p1) advancing++;
        }
        ASSERT("ST.05 advance", advancing >= started - 2);
    }

    /* ST.06: stop OPL3, verify mixer voices unaffected */
    of_audio_opl_write(0xB0, 0x00);  /* key off ch0 */
    of_audio_opl_write(0xB1, 0x00);  /* key off ch1 */
    of_audio_opl_write(0xB2, 0x00);  /* key off ch2 */
    {
        usleep(20 * 1000);
        int alive = 0;
        for (int i = 0; i < 31; i++)
            if (voices[i] >= 0 && of_mixer_voice_active(voices[i])) alive++;
        ASSERT("ST.06 post opl", alive >= started - 2);
    }

    /* ST.07: stop all voices, verify clean shutdown */
    of_mixer_stop_all();
    usleep(10 * 1000);
    of_mixer_poll_ended();
    {
        int alive = 0;
        for (int i = 0; i < 31; i++)
            if (voices[i] >= 0 && of_mixer_voice_active(voices[i])) alive++;
        ASSERT("ST.07 stopped", alive == 0);
    }

    of_audio_opl_reset();
    of_mixer_free_samples();

    section_end();
}

/* ================================================================
 * OPL3 FM synthesis tests (OP.xx)
 *
 * YMF262 register map reference:
 *   Operator slots: 0x20-0x35 (char), 0x40-0x55 (level), 0x60-0x75 (AD),
 *                   0x80-0x95 (SR), 0xE0-0xF5 (waveform)
 *   Channels: 0xA0-0xA8 (freq lo), 0xB0-0xB8 (freq hi + key),
 *             0xC0-0xC8 (feedback/connection/output)
 *   Bank 1 (OPL3): add 0x100 to all above
 *   Global: 0x01 (test), 0x08 (CSW/NTS), 0xBD (percussion), 0x105 (OPL3 enable)
 *
 * Operator-to-channel mapping (2-op mode):
 *   Ch 0-2: ops {0,3}, {1,4}, {2,5}
 *   Ch 3-5: ops {6,9}, {7,10}, {8,11}
 *   Ch 6-8: ops {12,15}, {13,16}, {14,17}
 * ================================================================ */

/* OPL3 operator slot offset for a given channel (2-op mode) */
static const uint8_t opl_op1[] = {0,1,2,8,9,10,16,17,18};
static const uint8_t opl_op2[] = {3,4,5,11,12,13,19,20,21};

/* Program a simple 2-op FM instrument on a channel */
static void opl_program_ch(int bank, int ch, uint8_t ws, uint8_t tl, uint8_t fb_cnt) {
    int base = bank ? 0x100 : 0;
    int m = opl_op1[ch], c = opl_op2[ch];
    of_audio_opl_write(base + 0x20 + m, 0x21);  /* EG=1, MULT=1 */
    of_audio_opl_write(base + 0x40 + m, tl);    /* modulator level */
    of_audio_opl_write(base + 0x60 + m, 0xF0);  /* AR=15, DR=0 */
    of_audio_opl_write(base + 0x80 + m, 0x0F);  /* SL=0, RR=15 */
    of_audio_opl_write(base + 0xE0 + m, ws);    /* waveform */
    of_audio_opl_write(base + 0x20 + c, 0x21);
    of_audio_opl_write(base + 0x40 + c, 0x00);  /* carrier max vol */
    of_audio_opl_write(base + 0x60 + c, 0xF0);
    of_audio_opl_write(base + 0x80 + c, 0x0F);
    of_audio_opl_write(base + 0xE0 + c, ws);
    of_audio_opl_write(base + 0xC0 + ch, fb_cnt | 0x30);  /* L+R output */
}

/* Key on/off */
static void opl_key_on(int bank, int ch, uint16_t fnum, uint8_t block) {
    int base = bank ? 0x100 : 0;
    of_audio_opl_write(base + 0xA0 + ch, fnum & 0xFF);
    of_audio_opl_write(base + 0xB0 + ch, 0x20 | ((block & 7) << 2) | ((fnum >> 8) & 3));
}
static void opl_key_off(int bank, int ch) {
    int base = bank ? 0x100 : 0;
    of_audio_opl_write(base + 0xB0 + ch, 0x00);
}

void test_opl3(void) {
    section_start("OPL3");

    /* OP.01: reset clears all 512 registers */
    of_audio_opl_reset();
    test_pass("OP.01 reset");

    /* OP.02: enable OPL3 mode + waveform select */
    of_audio_opl_write(0x105, 0x01);  /* NEW=1 (OPL3 mode) */
    of_audio_opl_write(0x01, 0x20);   /* WSE=1 (enable waveform select) */
    test_pass("OP.02 opl3en");

    /* OP.03: 2-op FM instrument, key on A4 (440Hz), key off */
    opl_program_ch(0, 0, 0, 0x1A, 0x01);  /* sine, mod TL=26, additive */
    opl_key_on(0, 0, 0x241, 4);           /* 440Hz, block 4 */
    usleep(50 * 1000);
    opl_key_off(0, 0);
    usleep(30 * 1000);
    test_pass("OP.03 2op A4");

    /* OP.04: all 4 waveforms — sine, half-sine, abs-sine, quarter-sine */
    {
        uint8_t ws[] = {0, 1, 2, 3};
        uint16_t fn[] = {0x241, 0x287, 0x2D0, 0x300};  /* A4, B4, C#5, D5 */
        for (int i = 0; i < 4; i++) {
            opl_program_ch(0, i, ws[i], 0x18, 0x01);
            opl_key_on(0, i, fn[i], 4);
        }
        usleep(80 * 1000);
        for (int i = 0; i < 4; i++) opl_key_off(0, i);
        usleep(20 * 1000);
        test_pass("OP.04 4 wvfrm");
    }

    /* OP.05: all 9 bank-0 channels (full OPL2 polyphony) */
    {
        of_audio_opl_reset();
        of_audio_opl_write(0x105, 0x01);
        of_audio_opl_write(0x01, 0x20);
        for (int ch = 0; ch < 9; ch++) {
            opl_program_ch(0, ch, 0, 0x14, 0x01);
            opl_key_on(0, ch, 0x200 + ch * 0x20, 3 + ch / 3);
        }
        usleep(100 * 1000);
        for (int ch = 0; ch < 9; ch++) opl_key_off(0, ch);
        usleep(20 * 1000);
        test_pass("OP.05 9ch");
    }

    /* OP.06: all 9 bank-1 channels (OPL3 channels 9-17) */
    {
        for (int ch = 0; ch < 9; ch++) {
            opl_program_ch(1, ch, 0, 0x14, 0x01);
            opl_key_on(1, ch, 0x240 + ch * 0x18, 4);
        }
        usleep(100 * 1000);
        for (int ch = 0; ch < 9; ch++) opl_key_off(1, ch);
        usleep(20 * 1000);
        test_pass("OP.06 bank1");
    }

    /* OP.07: 18-channel simultaneous (all OPL3 channels, both banks) */
    {
        for (int ch = 0; ch < 9; ch++) {
            opl_program_ch(0, ch, ch % 4, 0x18, 0x01);
            opl_key_on(0, ch, 0x1C0 + ch * 0x20, 4);
            opl_program_ch(1, ch, (ch + 2) % 4, 0x18, 0x01);
            opl_key_on(1, ch, 0x200 + ch * 0x20, 4);
        }
        usleep(100 * 1000);
        for (int ch = 0; ch < 9; ch++) {
            opl_key_off(0, ch);
            opl_key_off(1, ch);
        }
        usleep(20 * 1000);
        test_pass("OP.07 18ch");
    }

    /* OP.08: feedback sweep — all 8 feedback levels on ch0 */
    {
        opl_program_ch(0, 0, 0, 0x10, 0x00);  /* FM mode (CNT=0) */
        for (int fb = 0; fb < 8; fb++) {
            of_audio_opl_write(0xC0, 0x30 | (fb << 1));  /* FB=fb, CNT=0, L+R */
            opl_key_on(0, 0, 0x241, 4);
            usleep(30 * 1000);
            opl_key_off(0, 0);
            usleep(10 * 1000);
        }
        test_pass("OP.08 fb swp");
    }

    /* OP.09: ADSR envelope test — program extreme envelopes */
    {
        int m = opl_op1[0], c = opl_op2[0];
        of_audio_opl_write(0xC0, 0x31);  /* additive mode */

        /* Slow attack (AR=1), fast release (RR=15) */
        of_audio_opl_write(0x20 + m, 0x21);
        of_audio_opl_write(0x40 + m, 0x00);
        of_audio_opl_write(0x60 + m, 0x10);  /* AR=1, DR=0 */
        of_audio_opl_write(0x80 + m, 0x0F);  /* SL=0, RR=15 */
        of_audio_opl_write(0x20 + c, 0x21);
        of_audio_opl_write(0x40 + c, 0x00);
        of_audio_opl_write(0x60 + c, 0x10);  /* AR=1, DR=0 */
        of_audio_opl_write(0x80 + c, 0x0F);
        opl_key_on(0, 0, 0x241, 4);
        usleep(100 * 1000);  /* slow attack builds over 100ms */
        opl_key_off(0, 0);
        usleep(30 * 1000);   /* fast release */

        /* Fast attack (AR=15), slow release (RR=1) */
        of_audio_opl_write(0x60 + m, 0xF0);
        of_audio_opl_write(0x80 + m, 0x01);  /* RR=1 */
        of_audio_opl_write(0x60 + c, 0xF0);
        of_audio_opl_write(0x80 + c, 0x01);
        opl_key_on(0, 0, 0x241, 4);
        usleep(30 * 1000);
        opl_key_off(0, 0);
        usleep(200 * 1000);  /* slow release fades over 200ms */
        test_pass("OP.09 ADSR");
    }

    /* OP.10: tremolo + vibrato */
    {
        of_audio_opl_write(0xBD, 0xC0);  /* deep tremolo + deep vibrato */
        int m = opl_op1[0], c = opl_op2[0];
        of_audio_opl_write(0x20 + m, 0xE1);  /* AM=1, VIB=1, EG=1, MULT=1 */
        of_audio_opl_write(0x40 + m, 0x10);
        of_audio_opl_write(0x60 + m, 0xF0);
        of_audio_opl_write(0x80 + m, 0x07);
        of_audio_opl_write(0x20 + c, 0xE1);
        of_audio_opl_write(0x40 + c, 0x00);
        of_audio_opl_write(0x60 + c, 0xF0);
        of_audio_opl_write(0x80 + c, 0x07);
        of_audio_opl_write(0xC0, 0x31);
        opl_key_on(0, 0, 0x241, 4);
        usleep(200 * 1000);  /* hear the warble */
        opl_key_off(0, 0);
        usleep(30 * 1000);
        of_audio_opl_write(0xBD, 0x00);  /* disable trem/vib */
        test_pass("OP.10 trmvib");
    }

    /* OP.11: percussion/rhythm mode (BD, SD, TT, CY, HH) */
    {
        of_audio_opl_reset();
        of_audio_opl_write(0x105, 0x01);
        of_audio_opl_write(0x01, 0x20);

        /* Program percussion operator slots (ch6=BD, ch7=SD+HH, ch8=TT+CY) */
        /* Bass drum: ops 12,15 */
        of_audio_opl_write(0x20 + 12, 0x01); of_audio_opl_write(0x40 + 12, 0x00);
        of_audio_opl_write(0x60 + 12, 0xF4); of_audio_opl_write(0x80 + 12, 0x75);
        of_audio_opl_write(0x20 + 15, 0x01); of_audio_opl_write(0x40 + 15, 0x00);
        of_audio_opl_write(0x60 + 15, 0xF5); of_audio_opl_write(0x80 + 15, 0x75);
        of_audio_opl_write(0xC6, 0x30);
        of_audio_opl_write(0xA6, 0x40); of_audio_opl_write(0xB6, 0x14);

        /* Snare/HiHat: ops 13,16 */
        of_audio_opl_write(0x20 + 16, 0x01); of_audio_opl_write(0x40 + 16, 0x00);
        of_audio_opl_write(0x60 + 16, 0xFF); of_audio_opl_write(0x80 + 16, 0x78);
        of_audio_opl_write(0x20 + 13, 0x01); of_audio_opl_write(0x40 + 13, 0x0D);
        of_audio_opl_write(0x60 + 13, 0xFF); of_audio_opl_write(0x80 + 13, 0x78);
        of_audio_opl_write(0xA7, 0x00); of_audio_opl_write(0xB7, 0x11);

        /* Tom/Cymbal: ops 14,17 */
        of_audio_opl_write(0x20 + 14, 0x05); of_audio_opl_write(0x40 + 14, 0x00);
        of_audio_opl_write(0x60 + 14, 0xF5); of_audio_opl_write(0x80 + 14, 0x68);
        of_audio_opl_write(0x20 + 17, 0x01); of_audio_opl_write(0x40 + 17, 0x00);
        of_audio_opl_write(0x60 + 17, 0xF7); of_audio_opl_write(0x80 + 17, 0x58);
        of_audio_opl_write(0xA8, 0x00); of_audio_opl_write(0xB8, 0x10);

        /* Enable rhythm mode: BD, SD, TT, CY, HH */
        of_audio_opl_write(0xBD, 0x3F);  /* rhythm=1, all 5 perc on */
        usleep(80 * 1000);
        of_audio_opl_write(0xBD, 0x20);  /* rhythm=1, all perc off */
        usleep(30 * 1000);

        /* Individual hits */
        of_audio_opl_write(0xBD, 0x30); usleep(40 * 1000);  /* BD only */
        of_audio_opl_write(0xBD, 0x28); usleep(40 * 1000);  /* SD only */
        of_audio_opl_write(0xBD, 0x24); usleep(40 * 1000);  /* TT only */
        of_audio_opl_write(0xBD, 0x22); usleep(40 * 1000);  /* CY only */
        of_audio_opl_write(0xBD, 0x21); usleep(40 * 1000);  /* HH only */
        of_audio_opl_write(0xBD, 0x00);  /* rhythm off */
        usleep(20 * 1000);
        test_pass("OP.11 perc");
    }

    /* OP.12: stereo panning — L only, R only, both */
    {
        of_audio_opl_reset();
        of_audio_opl_write(0x105, 0x01);
        of_audio_opl_write(0x01, 0x20);
        opl_program_ch(0, 0, 0, 0x10, 0x01);

        of_audio_opl_write(0xC0, 0x10);  /* L only */
        opl_key_on(0, 0, 0x241, 4);
        usleep(50 * 1000);
        opl_key_off(0, 0); usleep(10 * 1000);

        of_audio_opl_write(0xC0, 0x20);  /* R only */
        opl_key_on(0, 0, 0x241, 4);
        usleep(50 * 1000);
        opl_key_off(0, 0); usleep(10 * 1000);

        of_audio_opl_write(0xC0, 0x30);  /* both */
        opl_key_on(0, 0, 0x241, 4);
        usleep(50 * 1000);
        opl_key_off(0, 0); usleep(10 * 1000);

        test_pass("OP.12 stereo");
    }

    /* OP.13: frequency sweep — chromatic scale across one octave */
    {
        /* F-numbers for C4 through B4 (block 4) */
        uint16_t scale[] = {0x16B,0x181,0x198,0x1B0,0x1CA,0x1E5,
                            0x202,0x220,0x241,0x263,0x287,0x2AE};
        opl_program_ch(0, 0, 0, 0x10, 0x01);
        for (int n = 0; n < 12; n++) {
            opl_key_on(0, 0, scale[n], 4);
            usleep(30 * 1000);
            opl_key_off(0, 0);
            usleep(5 * 1000);
        }
        test_pass("OP.13 scale");
    }

    /* OP.14: rapid register hammering — 10 bursts of 18-channel on/off
     * (simulates MIDI with max polyphony) */
    {
        for (int ch = 0; ch < 9; ch++) {
            opl_program_ch(0, ch, 0, 0x14, 0x01);
            opl_program_ch(1, ch, 0, 0x14, 0x01);
        }
        for (int burst = 0; burst < 10; burst++) {
            for (int ch = 0; ch < 9; ch++) {
                opl_key_on(0, ch, 0x200 + burst * 0x10 + ch * 0x08, 4);
                opl_key_on(1, ch, 0x240 + burst * 0x10 + ch * 0x08, 4);
            }
            usleep(15 * 1000);
            for (int ch = 0; ch < 9; ch++) {
                opl_key_off(0, ch);
                opl_key_off(1, ch);
            }
        }
        test_pass("OP.14 rapid");
    }

    /* OP.15: KSR + KSL — key scale rate and level */
    {
        opl_program_ch(0, 0, 0, 0x00, 0x01);
        int m = opl_op1[0];
        of_audio_opl_write(0x20 + m, 0x31);  /* KSR=1 */
        of_audio_opl_write(0x40 + m, 0xC0);  /* KSL=3, TL=0 */
        opl_key_on(0, 0, 0x241, 2);  /* low octave */
        usleep(40 * 1000);
        opl_key_off(0, 0); usleep(10 * 1000);
        opl_key_on(0, 0, 0x241, 6);  /* high octave — should be quieter (KSL) */
        usleep(40 * 1000);
        opl_key_off(0, 0); usleep(10 * 1000);
        test_pass("OP.15 ksr ksl");
    }

    /* OP.16: multiplier sweep — MULT 0-15 on modulator */
    {
        opl_program_ch(0, 0, 0, 0x10, 0x00);  /* FM mode */
        int m = opl_op1[0];
        uint8_t mults[] = {0,1,2,3,4,5,6,7,8,9,10,12,15};
        for (int i = 0; i < 13; i++) {
            of_audio_opl_write(0x20 + m, 0x20 | mults[i]);
            opl_key_on(0, 0, 0x241, 4);
            usleep(25 * 1000);
            opl_key_off(0, 0);
            usleep(5 * 1000);
        }
        test_pass("OP.16 mult");
    }

    /* OP.17: MIDI API — init, volume, bank */
    {
        int rc = of_midi_init();
        ASSERT("OP.17a init", rc == 0);
        of_midi_set_volume(200);
        ASSERT("OP.17b vol", of_midi_get_volume() == 200);
        const uint8_t *bank = of_midi_builtin_bank();
        ASSERT("OP.17c bank", bank != NULL);
        of_midi_set_volume(255);
    }

    /* ============================================================
     * OP.18-OP.25: midi_test_spec.md targeted tests
     * ============================================================ */
    of_audio_opl_reset();
    of_audio_opl_write(0x105, 0x01);
    of_audio_opl_write(0x01, 0x20);
    of_audio_opl_write(0x101, 0x20);  /* bank1 WSE */

    /* OP.18: carrier waveform addressing — set CarWS=3, ModWS=0.
     * Validates op_car table offsets (slot 3 vs slot 0). */
    {
        int m = opl_op1[0], c = opl_op2[0];
        of_audio_opl_write(0x20 + m, 0x21);
        of_audio_opl_write(0x40 + m, 0x10);
        of_audio_opl_write(0x60 + m, 0xF0);
        of_audio_opl_write(0x80 + m, 0x0F);
        of_audio_opl_write(0xE0 + m, 0x00);  /* ModWS=0 */
        of_audio_opl_write(0x20 + c, 0x21);
        of_audio_opl_write(0x40 + c, 0x00);
        of_audio_opl_write(0x60 + c, 0xF0);
        of_audio_opl_write(0x80 + c, 0x0F);
        of_audio_opl_write(0xE0 + c, 0x03);  /* CarWS=3 (quarter-sine) */
        of_audio_opl_write(0xC0, 0x31);
        opl_key_on(0, 0, 0x241, 4);
        usleep(40 * 1000);
        opl_key_off(0, 0);
        usleep(20 * 1000);
        test_pass("OP.18 carWS");
    }

    /* OP.19: KEY-OFF state preservation.
     * Program guitar, key on, key off, key on AGAIN without reprogramming.
     * If the second key-on doesn't sound buzzy, the OPL3 core is silently
     * resetting waveform/feedback registers on key-off. */
    {
        opl_program_ch(0, 0, 1, 0x10, 0x0D);  /* WS=1, FB=6, CNT=1 */
        opl_key_on(0, 0, 0x241, 4);
        usleep(60 * 1000);
        opl_key_off(0, 0);
        usleep(50 * 1000);
        /* Second key-on with NO reprogramming */
        opl_key_on(0, 0, 0x241, 4);
        usleep(60 * 1000);
        opl_key_off(0, 0);
        usleep(20 * 1000);
        /* Pass = no crash; user verifies both notes sound identical */
        test_pass("OP.19 keepst");
    }

    /* OP.20: channel reuse with different instruments.
     * Guitar → key off → piano → key off → guitar.
     * All on the same channel. Validates instrument reload works. */
    {
        /* Guitar 1 */
        opl_program_ch(0, 0, 1, 0x10, 0x0D);
        opl_key_on(0, 0, 0x241, 4);
        usleep(50 * 1000);
        opl_key_off(0, 0);
        usleep(20 * 1000);
        /* Piano */
        opl_program_ch(0, 0, 0, 0x18, 0x01);
        opl_key_on(0, 0, 0x241, 4);
        usleep(50 * 1000);
        opl_key_off(0, 0);
        usleep(20 * 1000);
        /* Guitar 2 — should sound like guitar 1 */
        opl_program_ch(0, 0, 1, 0x10, 0x0D);
        opl_key_on(0, 0, 0x241, 4);
        usleep(50 * 1000);
        opl_key_off(0, 0);
        usleep(20 * 1000);
        test_pass("OP.20 reuse");
    }

    /* OP.21: multi-channel instrument loading.
     * Load different instruments on ch0-3, key on simultaneously.
     * Validates that programming one channel doesn't disturb others. */
    {
        opl_program_ch(0, 0, 0, 0x10, 0x01);  /* sine, additive */
        opl_program_ch(0, 1, 1, 0x10, 0x0D);  /* half-sine, FM, FB=6 */
        opl_program_ch(0, 2, 2, 0x10, 0x05);  /* abs-sine, FM, FB=2 */
        opl_program_ch(0, 3, 3, 0x10, 0x09);  /* quarter-sine, FM, FB=4 */
        opl_key_on(0, 0, 0x200, 3);
        opl_key_on(0, 1, 0x241, 4);
        opl_key_on(0, 2, 0x287, 4);
        opl_key_on(0, 3, 0x2D0, 4);
        usleep(100 * 1000);
        for (int i = 0; i < 4; i++) opl_key_off(0, i);
        usleep(20 * 1000);
        test_pass("OP.21 multi");
    }

    /* OP.22: cross-translation-unit register writes.
     * Calls into test_opl_xtu.c (compiled separately). If this sounds
     * different from OP.04 (in-unit waveform test), we have a cross-TU bug. */
    {
        extern void xtu_program_guitar(int bank);
        extern void xtu_key_on_a4(int bank);
        extern void xtu_key_off(int bank);
        xtu_program_guitar(0);
        xtu_key_on_a4(0);
        usleep(60 * 1000);
        xtu_key_off(0);
        usleep(20 * 1000);
        test_pass("OP.22 xtu");
    }

    /* OP.23: cross-TU on bank 1.
     * Same as OP.22 but on OPL3 channel 9 (bank 1). */
    {
        extern void xtu_program_guitar(int bank);
        extern void xtu_key_on_a4(int bank);
        extern void xtu_key_off(int bank);
        xtu_program_guitar(1);
        xtu_key_on_a4(1);
        usleep(60 * 1000);
        xtu_key_off(1);
        usleep(20 * 1000);
        test_pass("OP.23 xtu b1");
    }

    /* OP.24: services-table dispatch isolation.
     * Same writes as OP.04 but explicitly through OF_SVC->opl_write
     * to verify the function pointer dispatch isn't corrupting writes. */
    {
        OF_SVC->opl_write(0x20, 0x21);
        OF_SVC->opl_write(0x40, 0x10);
        OF_SVC->opl_write(0x60, 0xF0);
        OF_SVC->opl_write(0x80, 0x0F);
        OF_SVC->opl_write(0xE0, 0x01);
        OF_SVC->opl_write(0x23, 0x21);
        OF_SVC->opl_write(0x43, 0x00);
        OF_SVC->opl_write(0x63, 0xF0);
        OF_SVC->opl_write(0x83, 0x0F);
        OF_SVC->opl_write(0xE3, 0x01);
        OF_SVC->opl_write(0xC0, 0x3D);
        OF_SVC->opl_write(0xA0, 0x41);
        OF_SVC->opl_write(0xB0, 0x32);
        usleep(60 * 1000);
        OF_SVC->opl_write(0xB0, 0x12);
        usleep(20 * 1000);
        test_pass("OP.24 svctbl");
    }

    /* OP.25: WSE off vs WSE on — same registers, different result. */
    {
        opl_program_ch(0, 0, 1, 0x10, 0x0D);  /* WS=1, FB=6 */

        /* WSE OFF: waveform select ignored, all WS=0 */
        of_audio_opl_write(0x01, 0x00);
        opl_key_on(0, 0, 0x241, 4);
        usleep(50 * 1000);
        opl_key_off(0, 0);
        usleep(20 * 1000);

        /* WSE ON: WS=1 takes effect */
        of_audio_opl_write(0x01, 0x20);
        opl_key_on(0, 0, 0x241, 4);
        usleep(50 * 1000);
        opl_key_off(0, 0);
        usleep(20 * 1000);
        test_pass("OP.25 wse");
    }

    /* OP.26: clean reset */
    of_audio_opl_reset();
    test_pass("OP.26 reset");

    section_end();
}

void test_audio_stream(void) {
    section_start("Audio Strm");

    /* AS.01: stream open */
    int rc = of_audio_stream_open(48000);
    ASSERT("AS.01 open", rc == 0);

    /* AS.02: stream ready (should be ready immediately — no data written yet) */
    ASSERT("AS.02 ready", of_audio_stream_ready());

    /* AS.03: stream write */
    {
        #define STRM_FRAMES 4096
        static int16_t strm_buf[STRM_FRAMES];
        for (int i = 0; i < STRM_FRAMES; i++) {
            int phase = ((uint32_t)i * 440 * 65536u / 48000) & 0xFFFF;
            strm_buf[i] = (int16_t)((phase < 32768 ? phase : 65536 - phase) - 16384);
        }
        int written = of_audio_stream_write(strm_buf, STRM_FRAMES);
        ASSERT("AS.03 write", written > 0);
    }

    /* AS.04: second write (ping-pong) */
    {
        static int16_t strm_buf2[STRM_FRAMES];
        for (int i = 0; i < STRM_FRAMES; i++)
            strm_buf2[i] = (int16_t)(i & 1 ? 4000 : -4000);
        int written = of_audio_stream_write(strm_buf2, STRM_FRAMES);
        ASSERT("AS.04 write2", written > 0);
    }

    /* AS.05: close */
    of_audio_stream_close();
    test_pass("AS.05 close");

    /* AS.06: re-open after close */
    rc = of_audio_stream_open(22050);
    ASSERT("AS.06 reopen", rc == 0);
    of_audio_stream_close();

    section_end();
}

void test_audio(void) {
    section_start("Audio");

    of_audio_init();
    test_pass("AU.01 init");

    int free = of_audio_free();
    ASSERT("AU.02 free", free > 0);

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
        ASSERT("AU.03 write", written > 0);
        (void)written;
    }

    section_end();
}
