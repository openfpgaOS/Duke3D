/*
 * of_audio.h -- Audio subsystem API for openfpgaOS
 *
 * 48 kHz stereo PCM + YMF262 OPL3 FM synthesis.
 */

#ifndef OF_AUDIO_H
#define OF_AUDIO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define OF_AUDIO_RATE   48000
#define OF_AUDIO_FIFO   512
#define OF_OPL_CHANNELS 18

#ifndef OF_PC

#include "of_syscall.h"
#include "of_syscall_numbers.h"

static inline void of_audio_init(void) {
    __of_syscall0(OF_SYS_AUDIO_INIT);
}

static inline int of_audio_write(const int16_t *samples, int count) {
    return (int)__of_syscall2(OF_SYS_AUDIO_WRITE, (long)samples, count);
}

static inline int of_audio_free(void) {
    return (int)__of_syscall0(OF_SYS_AUDIO_GET_FREE);
}

static inline void of_audio_opl_write(uint16_t reg, uint8_t val) {
    __of_syscall2(OF_SYS_OPL_WRITE, reg, val);
}

static inline void of_audio_opl_reset(void) {
    __of_syscall0(OF_SYS_OPL_RESET);
}

#else /* OF_PC */

void of_audio_init(void);
int  of_audio_write(const int16_t *samples, int count);
int  of_audio_free(void);
void of_audio_opl_write(uint16_t reg, uint8_t val);
void of_audio_opl_reset(void);
#endif /* OF_PC */

#ifdef __cplusplus
}
#endif

#endif /* OF_AUDIO_H */
