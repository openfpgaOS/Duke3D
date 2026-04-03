#include "test.h"

void test_timer(void) {
    section_start("Timer");
    uint32_t t1 = of_time_ms();
    ASSERT("nonzero", t1 > 0);
    of_delay_ms(50);
    uint32_t elapsed = of_time_ms() - t1;
    ASSERT("delay", elapsed >= 40 && elapsed < 200);
    ASSERT("time_us", of_time_us() > 0);
    section_end();
}

void test_timer_edge(void) {
    section_start("Timer Edge");

    uint32_t t0 = of_time_us();
    of_delay_us(0);
    uint32_t elapsed = of_time_us() - t0;
    ASSERT("delay_us 0", elapsed < 100);

    t0 = of_time_ms();
    of_delay_ms(0);
    ASSERT("delay_ms 0", of_time_ms() - t0 < 5);

    t0 = of_time_us();
    of_delay_us(1);
    elapsed = of_time_us() - t0;
    ASSERT("delay_us 1", elapsed < 100);

    uint32_t prev = of_time_us();
    int mono_ok = 1;
    for (int i = 0; i < 1000; i++) {
        uint32_t now = of_time_us();
        if (now < prev) { mono_ok = 0; break; }
        prev = now;
    }
    ASSERT("monotonic", mono_ok);

    section_end();
}
