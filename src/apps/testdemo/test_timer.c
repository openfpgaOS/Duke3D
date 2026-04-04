#include "test.h"
#include <time.h>
#include <unistd.h>

void test_timer(void) {
    section_start("Timer");

    /* clock_ms returns increasing values */
    uint32_t t1 = clock_ms();
    ASSERT("nonzero", t1 > 0);

    /* usleep(10000) delays ~10ms */
    usleep(50 * 1000);
    uint32_t elapsed = clock_ms() - t1;
    ASSERT("delay", elapsed >= 40 && elapsed < 200);

    /* clock_us has microsecond resolution */
    ASSERT("time_us", clock_us() > 0);

    section_end();
}

void test_timer_edge(void) {
    section_start("Timer Edge");

    /* usleep(0) returns quickly */
    uint32_t t0 = clock_us();
    usleep(0);
    uint32_t elapsed = clock_us() - t0;
    ASSERT("usleep 0", elapsed < 100);

    /* usleep(0) via clock_ms */
    t0 = clock_ms();
    usleep(0);
    ASSERT("usleep 0 ms", clock_ms() - t0 < 5);

    /* usleep(1) returns quickly */
    t0 = clock_us();
    usleep(1);
    elapsed = clock_us() - t0;
    ASSERT("usleep 1", elapsed < 100);

    /* Monotonicity: clock_us never goes backward */
    uint32_t prev = clock_us();
    int mono_ok = 1;
    for (int i = 0; i < 1000; i++) {
        uint32_t now = clock_us();
        if (now < prev) { mono_ok = 0; break; }
        prev = now;
    }
    ASSERT("monotonic", mono_ok);

    section_end();
}
