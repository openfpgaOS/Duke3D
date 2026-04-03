#include "test.h"

void test_shutdown(void) {
    section_start("Shutdown");

    volatile uint32_t *shutdown_reg = (volatile uint32_t *)(0x40000000 + 0xB0);
    uint32_t val = *shutdown_reg;
    ASSERT("not pending", (val & 1) == 0);

    *shutdown_reg = 1;
    test_pass("ack write");

    section_end();
}
