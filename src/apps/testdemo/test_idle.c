#include "test.h"

static volatile int idle_hook_count;

static void test_idle_callback(void) {
    idle_hook_count++;
}

void test_idle_hook(void) {
    section_start("Idle Hook");

    of_set_idle_hook(test_idle_callback);
    idle_hook_count = 0;

    FILE *f = fopen("slot:1", "rb");
    if (f) {
        uint8_t buf[4096];
        fread(buf, 1, 4096, f);
        fclose(f);
    }

    ASSERT("hook called", idle_hook_count > 0);
    (void)idle_hook_count;

    of_set_idle_hook((void *)0);

    int prev = idle_hook_count;
    f = fopen("slot:1", "rb");
    if (f) {
        uint8_t buf[256];
        fread(buf, 1, 256, f);
        fclose(f);
    }
    ASSERT("hook off", idle_hook_count == prev);

    section_end();
}
