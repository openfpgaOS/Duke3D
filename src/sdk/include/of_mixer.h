/*
 * of_mixer.h -- Hardware PCM Mixer API for openfpgaOS
 *
 * 32-voice hardware mixer. Mixing runs entirely in FPGA fabric —
 * zero CPU cost during playback.
 * Output: 48kHz stereo via audio FIFO, mixed with OPL3 FM synthesis.
 *
 * Usage:
 *   1. Call of_mixer_init()
 *   2. Allocate sample memory: buf = of_mixer_alloc_samples(size)
 *   3. Load 16-bit signed mono PCM data into buf
 *   4. Play: of_mixer_play(buf, sample_count, sample_rate, 0, volume)
 *   5. Optionally set loop, rate, stereo volume, bidi, position
 *
 * Volume: 0-255 per channel (log curve applied in hardware).
 * Resampling: 16.16 fixed-point rate.
 * Volume ramp: hardware smooths transitions (configurable rate).
 */

#ifndef OF_MIXER_H
#define OF_MIXER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

#define OF_MIXER_MAX_VOICES  32
#define OF_MIXER_OUTPUT_RATE 48000

/* Convert sample rate in Hz to 16.16 fixed-point for raw API.
 * Use at init time to build a note-rate lookup table. */
#define OF_MIXER_RATE_FP16(hz) \
    ((uint32_t)(((uint64_t)(hz) << 16) / OF_MIXER_OUTPUT_RATE))

#ifndef OF_PC

#include "of_services.h"

static inline void of_mixer_init(int max_voices, int output_rate) {
    OF_SVC->mixer_init(max_voices, output_rate);
}

static inline int of_mixer_play(const uint8_t *pcm_s16, uint32_t sample_count,
                                uint32_t sample_rate, int priority, int volume) {
    return OF_SVC->mixer_play(pcm_s16, sample_count, sample_rate, priority, volume);
}

/* Retrigger: redirect an active voice to a new sample without stop/start gap.
 * Amiga-style seamless note retrigger — no click. */
static inline void of_mixer_retrigger(int voice, const uint8_t *pcm_s16,
                                      uint32_t sample_count, uint32_t sample_rate,
                                      int volume) {
    OF_SVC->mixer_retrigger(voice, pcm_s16, sample_count, sample_rate, volume);
}

static inline void of_mixer_stop(int voice) { OF_SVC->mixer_stop(voice); }
static inline void of_mixer_stop_all(void) { OF_SVC->mixer_stop_all(); }

static inline void of_mixer_set_volume(int voice, int volume) {
    OF_SVC->mixer_set_volume(voice, volume);
}

static inline void of_mixer_pump(void) { OF_SVC->mixer_pump(); }

static inline int of_mixer_voice_active(int voice) {
    return OF_SVC->mixer_voice_active(voice);
}

static inline void of_mixer_set_pan(int voice, int pan) {
    OF_SVC->mixer_set_pan(voice, pan);
}

static inline void of_mixer_set_loop(int voice, int loop_start, int loop_end) {
    OF_SVC->mixer_set_loop(voice, loop_start, loop_end);
}

static inline void of_mixer_set_rate(int voice, int sample_rate_hz) {
    OF_SVC->mixer_set_rate(voice, sample_rate_hz);
}

static inline void of_mixer_set_vol_lr(int voice, int vol_l, int vol_r) {
    OF_SVC->mixer_set_vol_lr(voice, vol_l, vol_r);
}

static inline void of_mixer_set_bidi(int voice, int enable) {
    OF_SVC->mixer_set_bidi(voice, enable);
}

static inline int of_mixer_get_position(int voice) {
    return OF_SVC->mixer_get_position(voice);
}

static inline void of_mixer_set_position(int voice, int sample_offset) {
    OF_SVC->mixer_set_position(voice, sample_offset);
}

/* Set rate + stereo volume in one call — tracker hot path. */
static inline void of_mixer_set_voice(int voice, int sample_rate_hz,
                                      int vol_l, int vol_r) {
    OF_SVC->mixer_set_voice(voice, sample_rate_hz, vol_l, vol_r);
}

static inline void of_mixer_set_rate_raw(int voice, uint32_t rate_fp16) {
    OF_SVC->mixer_set_rate_raw(voice, rate_fp16);
}

/* Set rate (16.16 raw) + stereo volume — zero-division tracker hot path. */
static inline void of_mixer_set_voice_raw(int voice, uint32_t rate_fp16,
                                          int vol_l, int vol_r) {
    OF_SVC->mixer_set_voice_raw(voice, rate_fp16, vol_l, vol_r);
}

static inline void of_mixer_set_vol_rate(int voice, int rate) {
    OF_SVC->mixer_set_vol_rate(voice, rate);
}

static inline uint32_t of_mixer_poll_ended(void) {
    return OF_SVC->mixer_poll_ended();
}

static inline void *of_mixer_alloc_samples(size_t size) {
    return OF_SVC->mixer_alloc_samples((uint32_t)size);
}

static inline void of_mixer_free_samples(void) {
    OF_SVC->mixer_free_samples();
}

/* Register a callback invoked when any mixer voice finishes playing.
 * Callback receives bitmask of ended voices. Pass NULL to disable.
 * Callback runs in kernel context — keep it short. */
static inline void of_mixer_set_end_callback(void (*cb)(uint32_t ended_mask)) {
    OF_SVC->mixer_set_end_callback(cb);
}

#else /* OF_PC */

#include <stdlib.h>

static inline void of_mixer_init(int max_voices, int output_rate) {
    (void)max_voices; (void)output_rate;
}
static inline int of_mixer_play(const uint8_t *pcm_s16, uint32_t sample_count,
                                uint32_t sample_rate, int priority, int volume) {
    (void)pcm_s16; (void)sample_count; (void)sample_rate;
    (void)priority; (void)volume;
    return -1;
}
static inline void of_mixer_stop(int voice) { (void)voice; }
static inline void of_mixer_stop_all(void) {}
static inline void of_mixer_set_volume(int voice, int volume) {
    (void)voice; (void)volume;
}
static inline void of_mixer_pump(void) {}
static inline int of_mixer_voice_active(int voice) { (void)voice; return 0; }
static inline void of_mixer_set_pan(int voice, int pan) {
    (void)voice; (void)pan;
}
static inline void of_mixer_set_loop(int voice, int loop_start, int loop_end) {
    (void)voice; (void)loop_start; (void)loop_end;
}
static inline void of_mixer_set_rate(int voice, int sample_rate_hz) {
    (void)voice; (void)sample_rate_hz;
}
static inline void of_mixer_set_vol_lr(int voice, int vol_l, int vol_r) {
    (void)voice; (void)vol_l; (void)vol_r;
}
static inline void of_mixer_set_bidi(int voice, int enable) {
    (void)voice; (void)enable;
}
static inline int of_mixer_get_position(int voice) { (void)voice; return 0; }
static inline void of_mixer_set_position(int voice, int sample_offset) {
    (void)voice; (void)sample_offset;
}
static inline void of_mixer_set_voice(int voice, int sample_rate_hz,
                                      int vol_l, int vol_r) {
    (void)voice; (void)sample_rate_hz; (void)vol_l; (void)vol_r;
}
static inline void of_mixer_set_rate_raw(int voice, uint32_t rate_fp16) {
    (void)voice; (void)rate_fp16;
}
static inline void of_mixer_set_voice_raw(int voice, uint32_t rate_fp16,
                                          int vol_l, int vol_r) {
    (void)voice; (void)rate_fp16; (void)vol_l; (void)vol_r;
}
static inline void of_mixer_set_vol_rate(int voice, int rate) {
    (void)voice; (void)rate;
}
static inline uint32_t of_mixer_poll_ended(void) { return 0; }
static inline void *of_mixer_alloc_samples(size_t size) { return malloc(size); }
static inline void of_mixer_free_samples(void) { /* no-op on PC */ }

#endif /* OF_PC */

#ifdef __cplusplus
}
#endif

#endif /* OF_MIXER_H */
