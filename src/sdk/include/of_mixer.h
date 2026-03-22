/*
 * of_mixer.h -- Audio Mixer API for openfpgaOS
 *
 * Multi-voice PCM mixer with resampling.
 * Input: unsigned 8-bit PCM. Output: 48kHz stereo via audio FIFO.
 */

#ifndef OF_MIXER_H
#define OF_MIXER_H

#include <stdint.h>

#ifndef OF_PC

#include "of_syscall.h"
#include "of_syscall_numbers.h"

static inline void of_mixer_init(int max_voices, int output_rate) {
    __of_syscall2(OF_SYS_MIXER_INIT, max_voices, output_rate);
}

static inline int of_mixer_play(const uint8_t *pcm_u8, uint32_t sample_count,
                                uint32_t sample_rate, int priority, int volume) {
    return (int)__of_syscall5(OF_SYS_MIXER_PLAY, (long)pcm_u8, sample_count,
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

static inline void of_mixer_pump(void) {
    __of_syscall0(OF_SYS_MIXER_PUMP);
}

static inline int of_mixer_voice_active(int voice) {
    return (int)__of_syscall1(OF_SYS_MIXER_VOICE_ACTIVE, voice);
}

#else /* OF_PC */

static inline void of_mixer_init(int max_voices, int output_rate) {
    (void)max_voices; (void)output_rate;
}
static inline int of_mixer_play(const uint8_t *pcm_u8, uint32_t sample_count,
                                uint32_t sample_rate, int priority, int volume) {
    (void)pcm_u8; (void)sample_count; (void)sample_rate;
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

#endif /* OF_PC */

#endif /* OF_MIXER_H */
