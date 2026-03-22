/*
 * of_audio.h -- Audio subsystem API for openfpgaOS
 *
 * 48 kHz stereo PCM + YMF262 OPL3 FM synthesis.
 */

#ifndef OF_AUDIO_H
#define OF_AUDIO_H

#include <stdint.h>

#define OF_AUDIO_RATE   48000
#define OF_AUDIO_FIFO   4096
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

/* Enqueue samples into the kernel ring buffer.
 * The kernel drains them to the hardware FIFO automatically
 * during DMA waits — no app callback needed.
 * Returns number of stereo pairs enqueued. */
static inline int of_audio_enqueue(const int16_t *samples, int count) {
    return (int)__of_syscall2(OF_SYS_AUDIO_ENQUEUE, (long)samples, count);
}

/* Get free space in the kernel audio ring buffer */
static inline int of_audio_ring_free(void) {
    return (int)__of_syscall0(OF_SYS_AUDIO_RING_FREE);
}

static inline void of_audio_opl_write(uint8_t reg, uint8_t val) {
    __of_syscall2(OF_SYS_OPL_WRITE, reg, val);
}

static inline void of_audio_opl_reset(void) {
    __of_syscall0(OF_SYS_OPL_RESET);
}

#else /* OF_PC */

void of_audio_init(void);
int  of_audio_write(const int16_t *samples, int count);
int  of_audio_free(void);
void of_audio_opl_write(uint8_t reg, uint8_t val);
void of_audio_opl_reset(void);
static inline int of_audio_enqueue(const int16_t *s, int c) { (void)s; (void)c; return 0; }
static inline int of_audio_ring_free(void) { return 0; }

#endif /* OF_PC */

#endif /* OF_AUDIO_H */
