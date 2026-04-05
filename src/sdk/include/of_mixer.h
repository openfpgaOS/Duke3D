/*
 * of_mixer.h -- Hardware PCM Mixer API for openfpgaOS
 *
 * 32-voice hardware mixer with linear interpolation. Mixing runs
 * entirely in FPGA fabric — zero CPU cost during playback.
 * Output: 48kHz stereo via audio FIFO, mixed with OPL3 FM synthesis.
 *
 * Usage:
 *   1. Call of_mixer_init()
 *   2. Allocate sample memory: buf = of_mixer_alloc_samples(size)
 *   3. Load 16-bit signed mono PCM data into buf
 *   4. Play: of_mixer_play(buf, sample_count, sample_rate, 0, volume)
 *   5. Optionally set loop, rate, stereo volume, bidi, position
 *
 * Volume: 0-255 per channel (8.8 fixed-point in hardware).
 * Resampling: 16.16 fixed-point rate with linear interpolation.
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

#include "of_syscall.h"
#include "of_syscall_numbers.h"

static inline void of_mixer_init(int max_voices, int output_rate) {
    __of_syscall2(OF_SYS_MIXER_INIT, max_voices, output_rate);
}

/* Play a sample.
 * pcm_s16: pointer from of_mixer_alloc_samples() with 16-bit signed mono data.
 * sample_count: number of 16-bit samples.
 * sample_rate: original sample rate in Hz (resampled to 48kHz).
 * priority: reserved, pass 0.
 * volume: 0-255.
 * Returns voice index (0-31) or -1 if no voice available. */
static inline int of_mixer_play(const uint8_t *pcm_s16, uint32_t sample_count,
                                uint32_t sample_rate, int priority, int volume) {
    return (int)__of_syscall5(OF_SYS_MIXER_PLAY, (long)pcm_s16, sample_count,
                              sample_rate, priority, volume);
}

static inline void of_mixer_stop(int voice) {
    __of_syscall1(OF_SYS_MIXER_STOP, voice);
}

static inline void of_mixer_stop_all(void) {
    __of_syscall0(OF_SYS_MIXER_STOP_ALL);
}

static inline void of_mixer_set_volume(int voice, int volume) {
    __of_syscall2(OF_SYS_MIXER_SET_VOLUME, voice, volume);
}

/* No-op: hardware mixer runs autonomously, no CPU pumping needed. */
static inline void of_mixer_pump(void) { }

static inline int of_mixer_voice_active(int voice) {
    return (int)__of_syscall1(OF_SYS_MIXER_VOICE_ACTIVE, voice);
}

/* Set pan position. pan: 0=left, 128=center, 255=right. */
static inline void of_mixer_set_pan(int voice, int pan) {
    __of_syscall2(OF_SYS_MIXER_SET_PAN, voice, pan);
}

/* Set loop points. Enables LOOP flag.
 * Pass loop_start=-1 to disable looping. */
static inline void of_mixer_set_loop(int voice, int loop_start, int loop_end) {
    __of_syscall3(OF_SYS_MIXER_SET_LOOP, voice, loop_start, loop_end);
}

/* Set playback rate from sample rate in Hz.
 * Kernel converts to 16.16 fixed-point: RATE = (hz << 16) / 48000. */
static inline void of_mixer_set_rate(int voice, int sample_rate_hz) {
    __of_syscall2(OF_SYS_MIXER_SET_RATE, voice, sample_rate_hz);
}

/* Set left/right volume independently. 0-255 each. */
static inline void of_mixer_set_vol_lr(int voice, int vol_l, int vol_r) {
    __of_syscall3(OF_SYS_MIXER_SET_VOL_LR, voice, vol_l, vol_r);
}

/* Enable/disable bidirectional (ping-pong) looping. */
static inline void of_mixer_set_bidi(int voice, int enable) {
    __of_syscall2(OF_SYS_MIXER_SET_BIDI, voice, enable);
}

/* Read current playback position (integer sample index). */
static inline int of_mixer_get_position(int voice) {
    return (int)__of_syscall1(OF_SYS_MIXER_GET_POSITION, voice);
}

/* Set playback position (sample offset). */
static inline void of_mixer_set_position(int voice, int sample_offset) {
    __of_syscall2(OF_SYS_MIXER_SET_POSITION, voice, sample_offset);
}

/* Set rate + stereo volume in one syscall.
 * This is the hot-path call for tracker tick callbacks —
 * one ecall instead of two for the most common per-tick update. */
static inline void of_mixer_set_voice(int voice, int sample_rate_hz,
                                      int vol_l, int vol_r) {
    __of_syscall4(OF_SYS_MIXER_SET_VOICE, voice, sample_rate_hz, vol_l, vol_r);
}

/* Set playback rate directly as 16.16 fixed-point (no division in kernel).
 * Pre-compute with: rate_fp16 = ((uint64_t)sample_rate << 16) / 48000 */
static inline void of_mixer_set_rate_raw(int voice, uint32_t rate_fp16) {
    __of_syscall2(OF_SYS_MIXER_SET_RATE_RAW, voice, (long)rate_fp16);
}

/* Set rate (16.16 raw) + stereo volume — zero-division tracker hot path.
 * Pre-compute note table at init, use raw values in tick callback. */
static inline void of_mixer_set_voice_raw(int voice, uint32_t rate_fp16,
                                          int vol_l, int vol_r) {
    __of_syscall4(OF_SYS_MIXER_SET_VOICE_RAW, voice, (long)rate_fp16, vol_l, vol_r);
}

/* Set volume ramp speed. 0=instant, 1=fast (~5ms full sweep),
 * higher values = slower ramp. Default is 1 after of_mixer_play. */
static inline void of_mixer_set_vol_rate(int voice, int rate) {
    __of_syscall2(OF_SYS_MIXER_SET_VOL_RATE, voice, rate);
}

/* Poll for ended voices. Returns bitmask of voices that finished
 * since last poll. Clears hardware IRQ bits. */
static inline uint32_t of_mixer_poll_ended(void) {
    return (uint32_t)__of_syscall0(OF_SYS_MIXER_POLL_ENDED);
}

/* Allocate sample memory from kernel-managed pool.
 * Returns pointer to write sample data into, or NULL if full.
 * Pass the returned pointer to of_mixer_play(). */
static inline void *of_mixer_alloc_samples(size_t size) {
    return (void *)__of_syscall1(OF_SYS_MIXER_ALLOC_SAMPLES, (long)size);
}

/* Reset sample allocator — frees all sample memory.
 * Call when reloading all sound effects (e.g., level change). */
static inline void of_mixer_free_samples(void) {
    __of_syscall0(OF_SYS_MIXER_FREE_SAMPLES);
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
