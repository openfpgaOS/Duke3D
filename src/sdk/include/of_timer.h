/*
 * of_timer.h -- Timer API for openfpgaOS
 */

#ifndef OF_TIMER_H
#define OF_TIMER_H

#include <stdint.h>

#define OF_CPU_HZ       100000000

#ifndef OF_PC

#include "of_syscall.h"
#include "of_syscall_numbers.h"

static inline uint32_t of_time_us(void) {
    return (uint32_t)__of_syscall0(OF_SYS_TIMER_GET_US);
}

static inline uint32_t of_time_ms(void) {
    return (uint32_t)__of_syscall0(OF_SYS_TIMER_GET_MS);
}

static inline void of_delay_us(uint32_t us) {
    __of_syscall1(OF_SYS_TIMER_DELAY_US, us);
}

static inline void of_delay_ms(uint32_t ms) {
    __of_syscall1(OF_SYS_TIMER_DELAY_MS, ms);
}

#else /* OF_PC */

uint32_t of_time_us(void);
uint32_t of_time_ms(void);
void     of_delay_us(uint32_t us);
void     of_delay_ms(uint32_t ms);

#endif /* OF_PC */

#endif /* OF_TIMER_H */
