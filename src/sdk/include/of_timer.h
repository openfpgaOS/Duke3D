/*
 * of_timer.h -- Hardware timer interrupt API for openfpgaOS
 *
 * For time queries use clock_gettime() / clock_ms() / clock_us() from <time.h>
 * For delays use usleep() from <unistd.h>
 */

#ifndef OF_TIMER_H
#define OF_TIMER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#ifndef OF_PC

#include "of_syscall.h"
#include "of_syscall_numbers.h"

/* Set a periodic timer callback at the given frequency (Hz).
 * The callback runs in interrupt context — keep it short.
 * Pass NULL or hz=0 to disable. */
static inline void of_timer_set_callback(void (*fn)(void), uint32_t hz) {
    __of_syscall2(OF_SYS_TIMER_SET_CALLBACK, (long)fn, (long)hz);
}

/* Stop the periodic timer and clear the callback. */
static inline void of_timer_stop(void) {
    __of_syscall0(OF_SYS_TIMER_STOP);
}

#else /* OF_PC */

static inline void of_timer_set_callback(void (*fn)(void), uint32_t hz) { (void)fn; (void)hz; }
static inline void of_timer_stop(void) { }

#endif /* OF_PC */

#ifdef __cplusplus
}
#endif

#endif /* OF_TIMER_H */
