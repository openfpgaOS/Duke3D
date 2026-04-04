/*
 * of_mixer.h -- Hardware PCM Mixer API for openfpgaOS
 *
 * 32-voice hardware mixer. Mixing runs entirely in FPGA fabric —
 * zero CPU cost during playback. Output: 48kHz stereo via audio FIFO,
 * mixed with OPL3 FM synthesis.
 *
 * Usage:
 *   1. Call of_mixer_init()
 *   2. Allocate sample memory: buf = of_mixer_alloc_samples(size)
 *   3. Load 16-bit signed mono PCM data into buf
 *   4. Play: of_mixer_play(buf, sample_count, sample_rate, 0, volume)
 *
 * Volume: 0-255 (mapped to 4-bit hardware volume 0-15).
 * Resampling: nearest-neighbor via 16.16 fixed-point rate.
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
static inline void *of_mixer_alloc_samples(size_t size) { return malloc(size); }
static inline void of_mixer_free_samples(void) { /* no-op on PC */ }

#endif /* OF_PC */

#ifdef __cplusplus
}
#endif

#endif /* OF_MIXER_H */
