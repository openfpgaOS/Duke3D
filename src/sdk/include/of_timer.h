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

#include "of_services.h"

/* Set a periodic timer callback at the given frequency (Hz).
 * The callback runs in interrupt context — keep it short.
 * Pass NULL or hz=0 to disable. */
static inline void of_timer_set_callback(void (*fn)(void), uint32_t hz) {
    OF_SVC->timer_set_callback(fn, hz);
}

/* Stop the periodic timer and clear the callback. */
static inline void of_timer_stop(void) {
    OF_SVC->timer_stop();
}

#else /* OF_PC */

static inline void of_timer_set_callback(void (*fn)(void), uint32_t hz) { (void)fn; (void)hz; }
static inline void of_timer_stop(void) { }

#endif /* OF_PC */

#ifdef __cplusplus
}
#endif

#endif /* OF_TIMER_H */
