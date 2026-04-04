/* time.h -- openfpgaOS time support */
#ifndef _OF_TIME_H
#define _OF_TIME_H

#ifdef OF_PC
#include_next <time.h>
#else

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef uint32_t time_t;
typedef uint32_t clock_t;

#define CLOCKS_PER_SEC 1000
#define CLOCK_MONOTONIC 1

struct timespec {
    long tv_sec;
    long tv_nsec;
};

/* clock_gettime via syscall (SYS_clock_gettime64 = 403 on riscv32) */
static inline int clock_gettime(int clk_id, struct timespec *tp) {
    register long a7 __asm__("a7") = 403;
    register long a0 __asm__("a0") = clk_id;
    register long a1 __asm__("a1") = (long)tp;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a7) : "memory");
    return (int)a0;
}

/* Convenience: milliseconds since boot */
static inline uint32_t clock_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* Convenience: microseconds since boot */
static inline uint32_t clock_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000000 + ts.tv_nsec / 1000);
}

static inline clock_t clock(void) {
    return (clock_t)clock_ms();
}

static inline time_t time(time_t *t) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    time_t val = (time_t)ts.tv_sec;
    if (t) *t = val;
    return val;
}

struct tm {
    int tm_sec, tm_min, tm_hour;
    int tm_mday, tm_mon, tm_year;
    int tm_wday, tm_yday, tm_isdst;
};

static inline struct tm *localtime(const time_t *t) {
    static struct tm tm0;
    (void)t;
    return &tm0;
}

#ifdef __cplusplus
}
#endif

#endif /* OF_PC */
#endif /* _OF_TIME_H */
