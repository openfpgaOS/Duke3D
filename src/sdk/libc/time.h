/* time.h -- openfpgaOS minimal time support */
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

/* clock() returns milliseconds via of_time_ms */
static inline clock_t clock(void) {
    /* Use of_time_ms() -- caller must include of.h before this */
    extern uint32_t of_time_ms(void);
    return (clock_t)of_time_ms();
}

static inline time_t time(time_t *t) {
    extern uint32_t of_time_ms(void);
    time_t val = (time_t)(of_time_ms() / 1000);
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
